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
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any

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


def _video_record(path: Path) -> dict[str, Any]:
    if not path.is_file() or path.stat().st_size == 0:
        raise ArtifactError(f"missing or empty video: {path}")
    return {"path": str(path.resolve()), "sizeBytes": path.stat().st_size,
            "digest": digest_file(path), "probe": _run_ffprobe(path)}


def _model_record(path: Path) -> dict[str, Any]:
    if not path.is_file() or path.stat().st_size == 0:
        raise ArtifactError(f"missing or empty model: {path}")
    return {"path": str(path.resolve()), "sizeBytes": path.stat().st_size,
            "digest": digest_file(path)}


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
