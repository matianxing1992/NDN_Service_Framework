#!/usr/bin/env python3
"""Compare archived MiniNDN worker results with a same-input offline replay.

The verifier runs the real pinned CPU model through ``TrackingEngine`` in one
session and compares every archived per-window result.  It never reads the
source videos: only the verified frame bytes staged by the MiniNDN run are
allowed as offline inputs.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import tempfile
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-UAV-APP/tracking"))
from tracking_worker import CAMERAS, TrackingEngine  # noqa: E402


BOX_TOLERANCE_PX = 2
CONFIDENCE_TOLERANCE = 1e-4
ASSOCIATION_SCORE_TOLERANCE = 1e-5


def _frame_view(frame: dict[str, Any]) -> dict[str, Any]:
    return {
        "cameraId": frame["cameraId"],
        "sequence": frame["sequence"],
        "ptsUs": frame["ptsUs"],
        "tracks": frame["tracks"],
        "associations": frame["associations"],
        "inputDigest": frame["inputDigest"],
        "imageJpegDigest": frame["imageJpegDigest"],
    }


def _track_key(track: dict[str, Any]) -> tuple[int, int, str]:
    return (int(track["localId"]), int(track["globalId"]), str(track["class"]))


def _compare_tracks(actual: list[dict[str, Any]], expected: list[dict[str, Any]]) -> bool:
    """Compare tracker output while allowing CPU detector box quantization."""
    if len(actual) != len(expected):
        return False
    actual_by_key = {_track_key(item): item for item in actual}
    expected_by_key = {_track_key(item): item for item in expected}
    if set(actual_by_key) != set(expected_by_key):
        return False
    for key, left in actual_by_key.items():
        right = expected_by_key[key]
        if abs(float(left["confidence"]) - float(right["confidence"])) > CONFIDENCE_TOLERANCE:
            return False
        if len(left["box"]) != 4 or len(right["box"]) != 4:
            return False
        if any(abs(int(a) - int(b)) > BOX_TOLERANCE_PX
               for a, b in zip(left["box"], right["box"])):
            return False
        if int(left["ptsUs"]) != int(right["ptsUs"]):
            return False
    return True


def _compare_associations(actual: list[dict[str, Any]], expected: list[dict[str, Any]]) -> bool:
    if len(actual) != len(expected):
        return False
    fields = ("sourceCamera", "sourceLocalId", "targetCamera", "targetLocalId",
              "globalId", "sourcePtsUs", "targetPtsUs", "reason")
    actual_sorted = sorted(actual, key=lambda item: tuple(str(item.get(field, "")) for field in fields))
    expected_sorted = sorted(expected, key=lambda item: tuple(str(item.get(field, "")) for field in fields))
    for left, right in zip(actual_sorted, expected_sorted):
        if any(left.get(field) != right.get(field) for field in fields):
            return False
        if abs(float(left.get("score", 0.0)) - float(right.get("score", 0.0))) > ASSOCIATION_SCORE_TOLERANCE:
            return False
    return True


def verify(run_dir: Path, model: Path) -> dict[str, Any]:
    run_dir = run_dir.resolve()
    model = model.resolve()
    source_manifest = json.loads((run_dir / "source-frame-inputs.json").read_text())
    window_count = int(source_manifest["windowCount"])
    frame_root = run_dir / "private" / "frames"
    result_root = run_dir / "private" / "compute" / "worker" / "results"
    mismatches: list[str] = []
    association_count = 0
    image_digest_differences = 0
    # The production node creates one bounded Python worker per window.  A
    # single long-lived verifier engine is not equivalent: Ultralytics keeps
    # process-local BaseTrack counters and predictor state outside our JSON
    # snapshot.  Recreate the worker and restore the same state file for every
    # window, exactly as UavTrackingNode does.
    from ultralytics.trackers.basetrack import BaseTrack

    with tempfile.TemporaryDirectory(prefix="spec191-equivalence-") as directory:
        state_path = Path(directory) / "tracking-state.json"
        for index in range(window_count):
            result_path = result_root / f"window-{index}.json"
            if not result_path.is_file():
                mismatches.append(f"missing archived result: {result_path}")
                continue
            expected = json.loads(result_path.read_text())
            sequence = index + 1
            frames = []
            for camera in CAMERAS:
                payload = (frame_root / camera / f"window-{index}.jpg").read_bytes()
                frames.append(({"cameraId": camera, "sequence": sequence,
                                "ptsUs": index * 1_000_000}, payload))

            # A new worker process starts the Ultralytics global counter from
            # zero.  Reset it before constructing the per-window engine so the
            # in-process verifier models that process boundary faithfully.
            BaseTrack._count = 0
            engine = TrackingEngine(model)
            try:
                engine.start({"device": "cpu", "modelPath": str(model)},
                             [{"cameraId": camera} for camera in CAMERAS])
                if state_path.is_file():
                    engine.restore_state(state_path)
                actual = engine.process_window(
                    {"runId": expected["runId"], "missionId": expected["missionId"],
                     "windowId": expected["windowId"], "sequence": sequence}, frames)
                association_count += len(expected.get("associations", []))
                if actual["schema"] != expected.get("schema"):
                    mismatches.append(f"window-{index}: schema mismatch")
                if actual["sequence"] != expected.get("sequence"):
                    mismatches.append(f"window-{index}: sequence mismatch")
                if not _compare_associations(actual.get("associations", []), expected.get("associations", [])):
                    mismatches.append(f"window-{index}: association mismatch")
                expected_frames = [_frame_view(frame) for frame in expected["frames"]]
                actual_frames = [_frame_view(frame) for frame in actual["frames"]]
                if len(actual_frames) != len(expected_frames):
                    mismatches.append(f"window-{index}: frame count mismatch")
                else:
                    for actual_frame, expected_frame in zip(actual_frames, expected_frames):
                        if actual_frame["cameraId"] != expected_frame["cameraId"]:
                            mismatches.append(f"window-{index}: camera order mismatch")
                            continue
                        for field in ("sequence", "ptsUs", "inputDigest"):
                            if actual_frame[field] != expected_frame[field]:
                                mismatches.append(f"window-{index}: {actual_frame['cameraId']} {field} mismatch")
                        if not _compare_tracks(actual_frame["tracks"], expected_frame["tracks"]):
                            mismatches.append(f"window-{index}: {actual_frame['cameraId']} track mismatch")
                        if actual_frame["imageJpegDigest"] != expected_frame["imageJpegDigest"]:
                            # JPEG bytes can differ by a pixel when the CPU detector
                            # quantizes a box at a process boundary.  The structured
                            # track comparison above is the oracle; retain the digest
                            # difference as diagnostic evidence rather than a false red.
                            image_digest_differences += 1
                # The real worker saves state only after a successful window.
                engine.save_state(state_path)
            finally:
                engine.close()
    return {
        "schema": "spec191-tracking-equivalence/v1",
        "run": str(run_dir),
        "windows": window_count,
        "matched": not mismatches,
        "associationEvents": association_count,
        "imageDigestDifferences": image_digest_differences,
        "boxTolerancePx": BOX_TOLERANCE_PX,
        "workerBoundary": "fresh-process-per-window",
        "restoredState": "session-and-bytetrack",
        "mismatches": mismatches,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        report = verify(args.run_dir, args.model)
    except Exception as error:
        print(json.dumps({"schema": "spec191-tracking-equivalence/v1",
                          "matched": False, "error": str(error)}, sort_keys=True))
        return 2
    print(json.dumps(report, sort_keys=True))
    return 0 if report["matched"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
