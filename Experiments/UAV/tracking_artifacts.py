#!/usr/bin/env python3
"""Freeze and validate the Spec191 UAV tracking input/model passport.

Offline by default: no model, video, or upstream source is downloaded. A
real-data run must provide exactly three videos and a license acceptance.
The checked-in image fixture is for contract tests only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from fractions import Fraction
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any, Mapping

ROOT = Path(__file__).resolve().parents[2]
FUNCTIONAL_FIXTURE = ROOT / "NDNSF-UAV-APP/testdata/multiview-car"
DEFAULT_MODEL = ROOT / "NDNSF-UAV-APP/models/mvcnn_vehicle_cpu.onnx"
VIDEO_NAMES = ("uav1_1min.mp4", "uav2_1min.mp4", "uav3_1min.mp4")


class ArtifactError(RuntimeError):
    pass


def digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def _run_ffprobe(path: Path) -> dict[str, Any]:
    if shutil.which("ffprobe") is None:
        return {"status": "unavailable"}
    completed = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height,r_frame_rate,nb_frames,duration",
         "-of", "json", str(path)], text=True, capture_output=True,
        check=False, timeout=20)
    if completed.returncode != 0:
        raise ArtifactError(f"ffprobe failed for {path}: {completed.stderr.strip()}")
    streams = json.loads(completed.stdout or "{}").get("streams", [])
    return dict(streams[0]) if streams else {"status": "no-video-stream"}


def _video_record(path: Path, *, expected_digest: str | None = None) -> dict[str, Any]:
    if not path.is_file() or path.stat().st_size == 0:
        raise ArtifactError(f"missing or empty video: {path}")
    digest = digest_file(path)
    if expected_digest is not None and digest != expected_digest:
        raise ArtifactError("person source video digest mismatch")
    return {"path": str(path.resolve()), "sizeBytes": path.stat().st_size,
            "digest": digest, "probe": _run_ffprobe(path)}


def _model_record(path: Path) -> dict[str, Any]:
    if not path.is_file() or path.stat().st_size == 0:
        raise ArtifactError(f"missing or empty model: {path}")
    return {"path": str(path.resolve()), "sizeBytes": path.stat().st_size,
            "digest": digest_file(path)}


def validate_live_source_model(model: Path, license_text: str) -> dict[str, Any]:
    """Freeze the detector artifact for an FD-fed source without requiring MP4s."""
    if not license_text.strip():
        raise ArtifactError("a non-empty license acceptance record is required")
    model_record = _model_record(model.resolve())
    digest = hashlib.sha256(json.dumps(
        {"model": model_record, "license": license_text.strip()},
        sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    return {
        "schema": "spec191-uav-artifact-passport/v1",
        "mode": "live-source-model-only",
        "licenseAcceptance": license_text.strip(),
        "model": model_record,
        "candidateDigest": "sha256:" + digest,
        "scientificAccuracyClaimAllowed": False,
    }


def validate_video_reference_assets(source: Path, model: Path,
                                   license_text: str) -> dict[str, Any]:
    """Freeze video-reference inputs used by the live-push adapter."""
    passport = validate_real_assets(source, model, license_text)
    passport["mode"] = "video-reference-simulated"
    digest = hashlib.sha256(json.dumps(
        {"mode": passport["mode"], "videos": passport["videos"],
         "model": passport["model"], "license": passport["licenseAcceptance"]},
        sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    passport["candidateDigest"] = "sha256:" + digest
    return passport


def validate_real_assets(source: Path, model: Path, license_text: str) -> dict[str, Any]:
    source = source.resolve()
    if not source.is_dir():
        raise ArtifactError(f"dataset root is not a directory: {source}")
    if not license_text.strip():
        raise ArtifactError("a non-empty license acceptance record is required")
    videos = []
    for camera, name in zip(("UAV1", "UAV2", "UAV3"), VIDEO_NAMES):
        record = _video_record(source / name)
        record["cameraId"] = camera
        videos.append(record)
    model_record = _model_record(model.resolve())
    if any(item["probe"].get("status") == "no-video-stream" for item in videos):
        raise ArtifactError("each input must contain a video stream")
    digest = hashlib.sha256(json.dumps(
        {"videos": videos, "model": model_record, "license": license_text.strip()},
        sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    return {"schema": "spec191-uav-artifact-passport/v1", "mode": "real-data",
            "licenseAcceptance": license_text.strip(), "videos": videos,
            "model": model_record, "candidateDigest": "sha256:" + digest,
            "scientificAccuracyClaimAllowed": False}


def load_person_source_manifest(path: Path, limits: Mapping[str, int]) -> dict[str, Any]:
    """Admit three immutable video inputs and an explicit media-time mapping.

    No labels or evaluation paths belong in this source-only manifest. The
    declared offsets are not proof of physical camera synchronization.
    """
    path = path.expanduser().resolve(strict=True)
    with path.open("rb") as stream:
        raw = stream.read(65537)
    if not raw or len(raw) > 65536:
        raise ArtifactError("person source manifest must be within 64 KiB")

    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise ArtifactError("duplicate person source manifest key")
            value[key] = item
        return value

    def exact(value, keys):
        if not isinstance(value, dict) or set(value) != set(keys):
            raise ArtifactError("person source manifest has missing or forbidden fields")

    def integer(value, maximum=2**63 - 1, minimum=0):
        if type(value) is not int or not minimum <= value <= maximum:
            raise ArtifactError("person source integer is outside its bound")
        return value

    def text(value, bound=256):
        if (not isinstance(value, str) or not value or len(value.encode()) > bound or
                any(ord(character) < 32 for character in value)):
            raise ArtifactError("invalid person source text")
        return value

    try:
        value = json.loads(raw, object_pairs_hook=unique)
        fields = ("schema", "dataset", "commonStartUs", "durationUs", "cameras")
        exact(value, fields + (("datasetOrigin",) if "datasetOrigin" in value else ()))
        if value["schema"] != "uav-person-source-manifest-v1":
            raise ArtifactError("unsupported person source manifest schema")
        origin = value.get("datasetOrigin", "unspecified")
        if "datasetOrigin" in value and origin not in ("real-captured", "synthetic-rendered"):
            raise ArtifactError("unsupported person dataset origin")
        exact(value["dataset"], ("name", "uri", "license"))
        for key in ("name", "uri", "license"):
            text(value["dataset"][key], 2048)
        start = integer(value["commonStartUs"])
        duration = integer(value["durationUs"], 300000000, 500000)
        if duration % 500000 or start > 2**63 - 1 - duration or limits["sampleHz"] != 2:
            raise ArtifactError("person source interval must contain whole 2 Hz windows")
        if not isinstance(value["cameras"], list) or len(value["cameras"]) != 3:
            raise ArtifactError("person source manifest requires three camera views")
        inodes, digests, views = set(), set(), set()
        rows, source_ids = [], {}
        for index, camera in enumerate(("UAV1", "UAV2", "UAV3")):
            item = value["cameras"][index]
            exact(item, ("cameraId", "viewId", "videoPath", "videoDigest", "videoStartUs"))
            if item["cameraId"] != camera:
                raise ArtifactError("person source cameras must be ordered UAV1/UAV2/UAV3")
            view = text(item["viewId"], 128)
            video_start = integer(item["videoStartUs"])
            if video_start > 2**63 - 1 - duration:
                raise ArtifactError("person source video interval overflows")
            candidate = Path(text(item["videoPath"], 4096)).expanduser()
            video = (path.parent / candidate).resolve(strict=True) if not candidate.is_absolute() else candidate.resolve(strict=True)
            info = video.stat()
            if not video.is_file() or not 0 < info.st_size <= 4 * 1024**3:
                raise ArtifactError("person video must be a nonempty file within 4 GiB")
            record = _video_record(video, expected_digest=item["videoDigest"])
            inode = (info.st_dev, info.st_ino)
            if inode in inodes or record["digest"] in digests or view in views:
                raise ArtifactError("person source views must use distinct videos and view IDs")
            inodes.add(inode)
            digests.add(record["digest"])
            views.add(view)
            probe = record["probe"]
            width, height = int(probe["width"]), int(probe["height"])
            rate = float(Fraction(probe["r_frame_rate"]))
            seconds = float(probe["duration"])
            if (not 1 <= width <= limits["maxFrameWidth"] or
                    not 1 <= height <= limits["maxFrameHeight"] or
                    not math.isfinite(rate) or not 2 <= rate <= 60 or
                    not math.isfinite(seconds) or not 0 < seconds <= 3600):
                raise ArtifactError("person video dimensions/rate/duration exceed preparation bounds")
            identity = {key: field for key, field in item.items() if key != "videoPath"}
            mapping = json.dumps(dict(identity, commonStartUs=start),
                                 sort_keys=True, separators=(",", ":")).encode()
            source_ids[camera] = "person-" + camera + "-" + hashlib.sha256(mapping).hexdigest()[:32]
            rows.append(dict(item, videoPath=str(video), probe=probe, sizeBytes=info.st_size,
                             framePeriodUs=math.ceil(1000000 / rate)))
        return dict(value, datasetOrigin=origin, cameras=rows, sourceIds=source_ids, windowCount=duration // 500000,
                    manifestPath=str(path), manifestDigest="sha256:" + hashlib.sha256(raw).hexdigest(),
                    maxClockSkewUs=limits["maxClockSkewUs"], maxFrameBytes=limits["maxFrameBytes"],
                    maxFrameWidth=limits["maxFrameWidth"], maxFrameHeight=limits["maxFrameHeight"])
    except (OSError, ValueError, KeyError, TypeError, ZeroDivisionError) as error:
        raise ArtifactError("invalid person source manifest: " + str(error)) from error


def functional_passport() -> dict[str, Any]:
    entries = json.loads((FUNCTIONAL_FIXTURE / "manifest.json").read_text())
    views = []
    for item in entries["views"]:
        path = FUNCTIONAL_FIXTURE / item["file"]
        views.append({"viewId": item["view_id"], "path": str(path.resolve()),
                      "digest": digest_file(path)})
    return {"schema": "spec191-uav-artifact-passport/v1", "mode": "functional-test-only",
            "views": views, "source": entries["fixture_id"],
            "scientificAccuracyClaimAllowed": False,
            "reason": "does not satisfy the three-video real-data gate"}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--license", default="")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--functional-fixture", action="store_true")
    args = parser.parse_args(argv)
    try:
        passport = functional_passport() if args.functional_fixture else validate_real_assets(
            args.source or Path("."), args.model, args.license)
    except (ArtifactError, OSError, json.JSONDecodeError) as exc:
        print(f"artifact preflight failed: {exc}", file=sys.stderr)
        return 2
    rendered = json.dumps(passport, indent=2, sort_keys=True) + "\n"
    if args.output:
        target = args.output.resolve()
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_suffix(target.suffix + ".tmp")
        temporary.write_text(rendered, encoding="utf-8")
        os.replace(temporary, target)
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
