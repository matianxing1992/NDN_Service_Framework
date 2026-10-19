#!/usr/bin/env python3
"""MiniNDN source adapter for the selected-provider ownership regression."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import time

import cv2

tracking_libexec = Path(os.environ.get(
    "NDNSF_UAV_TRACKING_LIBEXEC", "/usr/local/libexec/ndnsf-uav/tracking"))
sys.path.insert(0, str(tracking_libexec))
from observation_source import (load_observation_profile, load_source_generation_manifest,
                                profile_events, source_generation_for_window, write_event)


def main() -> int:
    parser = argparse.ArgumentParser()
    for name in ("camera", "profile", "event-stream-dir", "source-id",
                 "source-generations", "world-frame-id"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--source-epoch", type=int, required=True)
    parser.add_argument("--sample-rate-hz", type=float, required=True)
    parser.add_argument("--window-count", type=int, required=True)
    args = parser.parse_args()

    assets = Path(os.environ["NDNSF_UAV_DEMO_ASSET_DIR"])
    run_root = Path(os.environ["NDNSF_SPEC191_RUN_ROOT"])
    receipt_path = run_root / "private/config/external-calibration-refs.json"
    profile = load_observation_profile(Path(args.profile))
    profile["deliveryMode"] = "live-push"
    profile["sampleRateHz"] = args.sample_rate_hz
    profile["frameCount"] = args.window_count
    profile["source"]["kind"] = "external-fd"
    generations_path = Path(args.source_generations)
    generations = load_source_generation_manifest(generations_path, args.window_count)
    manifest_digest = "sha256:" + hashlib.sha256(generations_path.read_bytes()).hexdigest()
    print("UAV_OBSERVATION_SOURCE_ADAPTER_READY generation-manifest-sha256=" +
          manifest_digest, flush=True)

    deadline = time.monotonic() + 120.0
    while not receipt_path.is_file() and time.monotonic() < deadline:
        time.sleep(0.05)
    if not receipt_path.is_file():
        raise TimeoutError("selected Calibration-Provider did not publish a receipt")
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    if (receipt.get("providerIdentity") != "/example/uav/calibration-provider" or
            receipt.get("role") != "Calibration-Provider"):
        raise RuntimeError("receipt is not from the selected Calibration-Provider")
    for camera in ("UAV1", "UAV2", "UAV3"):
        reference = receipt["references"][camera]
        if reference.get("producerIdentity") != receipt["providerIdentity"]:
            raise RuntimeError("calibration reference producer does not match provider")
        profile["cameras"][camera]["calibration"]["externalDataRef"] = {
            "exactName": reference["exactName"],
            "contentSha256": reference["contentSha256"],
            "producerIdentity": reference["producerIdentity"],
        }

    video = assets / ("uav" + args.camera[-1] + "_1min.mp4")
    capture = cv2.VideoCapture(str(video))
    if not capture.isOpened():
        raise RuntimeError("cannot open simulated pixel source: " + str(video))
    fault = os.environ.get("SPEC210_T012_FAULT", "none")
    for window in range(args.window_count):
        ok, frame = capture.read()
        if not ok:
            raise RuntimeError("cannot decode simulated reference frame")
        ok, encoded = cv2.imencode(".jpg", frame)
        if not ok:
            raise RuntimeError("cannot encode simulated reference frame")
        generation = source_generation_for_window(generations, window)
        source_id = generation["sourceIds"][args.camera]
        epoch = generation["sourceEpoch"]
        capture_us = int(round(window * 1_000_000 / args.sample_rate_hz))
        fifo = Path(args.event_stream_dir) / ("window-" + str(window) + ".jsonl")
        with fifo.open("wb", buffering=0) as stream:
            for kind, payload in profile_events(
                    profile, args.camera, window + 1, capture_us, encoded.tobytes(),
                    source_id=source_id, epoch=epoch,
                    image_width=frame.shape[1], image_height=frame.shape[0]):
                if (fault == "authorized-camera-producer" and args.camera == "UAV3" and
                        window == 1 and kind == "calibration"):
                    # The camera publishes its valid local calibration for this
                    # frame. The selected provider owns the other windows' refs.
                    payload.pop("externalDataRef", None)
                    print("SPEC210_CAMERA_CALIBRATION_INJECTED camera=UAV3 "
                          "window=window-1 producer=/example/uav/drone/UAV3", flush=True)
                write_event(stream, kind, payload)
        print("UAV_OBSERVATION_SOURCE_ADAPTER_WINDOW_COMPLETE window=" + str(window) +
              " source_id=" + source_id + " epoch=" + str(epoch), flush=True)
    capture.release()
    print("UAV_OBSERVATION_SOURCE_ADAPTER_COMPLETE camera=" + args.camera, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
