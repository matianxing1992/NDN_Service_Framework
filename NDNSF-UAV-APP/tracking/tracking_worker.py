#!/usr/bin/env python3
"""Legacy CPU person-model adapter fed by native-verified frame bytes."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys

import cv2
import numpy as np

CAMERAS = ("UAV1", "UAV2", "UAV3")


def _unique_person_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate backend context field")
        result[key] = value
    return result


def _person_model_resource_sample():
    """Observe this worker after model admission, without changing aggregate samples."""
    import psutil
    import time

    try:
        process = psutil.Process(os.getpid())
        with process.oneshot():
            created = process.create_time()
            rss = process.memory_info().rss
            threads = process.num_threads()
        return {"monotonicNs": time.monotonic_ns(), "pid": process.pid,
                "creationTimeUnixSeconds": created, "rssBytes": rss, "threads": threads}
    except (OSError, psutil.Error) as error:
        return {"error": type(error).__name__[:128]}


def _run_person_backend(arguments: argparse.Namespace, backend_factory=None) -> int:
    """Run model computation only; the native caller makes all identity decisions."""
    from io import BytesIO
    from PIL import Image
    from person_profile import load_person_profile
    from person_reid import BackendFrame, PersonModelBackend

    if (not arguments.reid_model or not arguments.person_window or arguments.state_file
            or arguments.motion_json or arguments.observation_dir or arguments.observation_manifest):
        raise ValueError("person backend requires reid-model/person-window and no legacy state/input")
    profile = load_person_profile(Path(arguments.tracking_profile))
    with Path(arguments.person_window).open("rb") as stream:
        payload = stream.read(profile.limits["maxSnapshotBytes"] + 1)
    if len(payload) > profile.limits["maxSnapshotBytes"]:
        raise ValueError("person backend context exceeds byte limit")
    window = json.loads(payload, object_pairs_hook=_unique_person_pairs)
    expected = {"schema", "runId", "missionId", "windowId", "sequence",
                "computationDigest", "frames", "previousBackendState"}
    if (not isinstance(window, dict) or set(window) != expected
            or window["schema"] != "uav-person-backend-input-v1"):
        raise ValueError("invalid native person backend window schema")
    if (window["runId"] != arguments.run_id or window["missionId"] != arguments.mission_id
            or window["windowId"] != arguments.window_id or window["sequence"] != arguments.sequence
            or type(window["sequence"]) is not int):
        raise ValueError("person backend context does not match native invocation")
    expected_computation = getattr(arguments, "expected_computation_digest", None)
    if expected_computation is not None and window["computationDigest"] != expected_computation:
        raise ValueError("person session computation binding mismatch")
    metadata = window["frames"]
    if (not isinstance(metadata, list) or len(metadata) != 3
            or any(not isinstance(item, dict) for item in metadata)
            or [item.get("cameraId") for item in metadata] != list(CAMERAS)):
        raise ValueError("person backend requires three canonical camera frames")
    frames = []
    for item in metadata:
        if set(item) != {"cameraId", "sourceId", "sourceEpoch", "trackerGeneration",
                         "sequence", "commonPtsUs", "inputDigest", "width", "height"}:
            raise ValueError("invalid native person input frame fields")
        item = dict(item)
        expected_width, expected_height = item.pop("width"), item.pop("height")
        if (type(expected_width) is not int or type(expected_height) is not int
                or expected_width < 1 or expected_height < 1):
            raise ValueError("invalid native person input dimensions")
        path = Path(arguments.input_dir) / (item["cameraId"] + ".jpg")
        with path.open("rb") as stream:
            encoded = stream.read(profile.limits["maxFrameBytes"] + 1)
        if not encoded or len(encoded) > profile.limits["maxFrameBytes"]:
            raise ValueError("person backend input frame exceeds byte limit")
        if "sha256:" + hashlib.sha256(encoded).hexdigest() != item["inputDigest"]:
            raise ValueError("person backend input frame digest mismatch")
        # Check dimensions from the encoded header before allocating a decoded raster.
        with Image.open(BytesIO(encoded)) as header:
            width, height = header.size
            if (header.format != "JPEG" or (width, height) != (expected_width, expected_height)
                    or width > profile.limits["maxFrameWidth"]
                    or height > profile.limits["maxFrameHeight"]):
                raise ValueError("person backend decoded raster exceeds dimension limit")
        image = cv2.imdecode(np.frombuffer(encoded, np.uint8), cv2.IMREAD_COLOR)
        frames.append(BackendFrame(item, image))
    import time
    model_init_start_ns = time.monotonic_ns()
    engine = (PersonModelBackend(profile, Path(arguments.model), Path(arguments.reid_model))
              if backend_factory is None else
              backend_factory(profile, Path(arguments.model), Path(arguments.reid_model)))
    model_init_end_ns = time.monotonic_ns()
    try:
        model_resource_sample = _person_model_resource_sample()
        inference_start_ns = time.monotonic_ns()
        result = engine.process_window(frames, window["previousBackendState"])
        inference_end_ns = time.monotonic_ns()
    finally:
        if backend_factory is None:
            engine.close()
    for field in ("runId", "missionId", "windowId", "sequence", "computationDigest"):
        result[field] = window[field]
    encoded = json.dumps(result, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
    if len(encoded) > profile.limits["maxResultBytes"]:
        raise ValueError("person backend result exceeds byte limit")
    Path(arguments.output_json).write_bytes(encoded)
    print(json.dumps({"event": "PERSON_BACKEND_COMPLETED", "windowId": arguments.window_id,
                      "runId": window["runId"], "missionId": window["missionId"], "pid": os.getpid(),
                      "sequence": window["sequence"], "sourceEpoch": metadata[0]["sourceEpoch"],
                      "clockDomain": "CLOCK_MONOTONIC_NS", "modelInitStartNs": model_init_start_ns,
                      "modelInitEndNs": model_init_end_ns, "inferenceStartNs": inference_start_ns,
                      "inferenceEndNs": inference_end_ns,
                      "modelResourceSample": model_resource_sample,
                      "actualDevice": profile.requested_device}), flush=True)
    return 0


def _run_person_session(arguments: argparse.Namespace) -> int:
    """Serve bounded local jobs; the native owner alone authorizes and commits each window."""
    from person_reid import PersonModelBackend

    engine = None
    identity = None

    def backend(profile, detector, reid):
        nonlocal engine, identity
        current = (profile.digest, str(detector), str(reid))
        if engine is None:
            engine = PersonModelBackend(profile, detector, reid)
            identity = current
        elif current != identity:
            raise ValueError("person session model/profile identity changed")
        return engine

    try:
        while True:
            raw = sys.stdin.buffer.readline(4097)
            if not raw:
                return 0
            if len(raw) > 4096 or not raw.endswith(b"\n") or b"\0" in raw:
                raise ValueError("invalid person session control line")
            message = json.loads(raw, object_pairs_hook=_unique_person_pairs)
            fields = {"schema", "jobId", "runId", "missionId", "windowId", "sequence",
                      "computationDigest"}
            if (not isinstance(message, dict) or set(message) != fields or
                    message["schema"] != "uav-person-worker-request-v1" or
                    any(not isinstance(message[k], str) or not message[k]
                        for k in fields - {"sequence"}) or
                    type(message["sequence"]) is not int or
                    not 1 <= message["sequence"] <= 2**64 - 1 or
                    message["runId"] != arguments.run_id or
                    message["windowId"] != "window-" + str(message["sequence"] - 1) or
                    any(re.fullmatch(r"sha256:[0-9a-f]{64}", message[k]) is None
                        for k in ("jobId", "computationDigest"))):
                raise ValueError("invalid person session request binding")
            job = argparse.Namespace(**vars(arguments))
            job.mission_id = message["missionId"]
            job.window_id = message["windowId"]
            job.sequence = message["sequence"]
            job.expected_computation_digest = message["computationDigest"]
            if _run_person_backend(job, backend) != 0:
                raise RuntimeError("person session backend did not complete")
            print("SPEC219_PERSON_SESSION_DONE jobId=" + message["jobId"], flush=True)
    finally:
        if engine is not None:
            engine.close()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True)
    parser.add_argument("--input-dir", required=True)
    parser.add_argument("--output-json", required=True)
    parser.add_argument("--annotated-dir", required=True)
    parser.add_argument("--state-file")
    parser.add_argument("--candidate-state-file")
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--mission-id", required=True)
    parser.add_argument("--window-id", required=True)
    parser.add_argument("--sequence", type=int, default=1)
    parser.add_argument("--pts-us", type=int, default=0)
    parser.add_argument("--motion-json")
    parser.add_argument("--motion-digest")
    parser.add_argument("--fail-on-motion-unknown", action="store_true")
    parser.add_argument("--observation-dir")
    parser.add_argument("--observation-manifest")
    parser.add_argument("--confidence", type=float, default=0.25)
    parser.add_argument("--tracking-profile", required=True)
    parser.add_argument("--reid-model")
    parser.add_argument("--person-window")
    parser.add_argument("--person-session", action="store_true")
    args = parser.parse_args(argv)
    if args.person_session and (not args.reid_model or not args.person_window or args.state_file
                                or args.motion_json or args.observation_dir
                                or args.observation_manifest):
        parser.error("person session requires pinned person inputs and no legacy state/input")
    if args.sequence < 1 or args.pts_us < 0:
        parser.error("sequence must be positive and pts-us non-negative")
    try:
        if args.person_session:
            return _run_person_session(args)
        return _run_person_backend(args)
    except Exception as error:
        print(json.dumps({"event": "TRACKING_FAILED", "error": str(error)}), flush=True)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
