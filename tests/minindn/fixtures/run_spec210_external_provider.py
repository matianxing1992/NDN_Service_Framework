#!/usr/bin/env python3
"""Add the isolated Spec210 external Calibration-Provider to the MiniNDN plan."""

from __future__ import annotations

import dataclasses
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "NDNSF-UAV-APP/tracking"))

import cv2  # noqa: E402
from observation_source import encode_event, load_observation_profile, profile_events  # noqa: E402

from Experiments.UAV import run_multicamera_tracking_demo as demo  # noqa: E402

FIXTURES = Path(__file__).resolve().parent
POLICY = FIXTURES / "spec210-external-calibration.policies"
PROVIDER_IDENTITY = "/example/uav/calibration-provider"


def argument_path(name: str) -> Path:
    return Path(sys.argv[sys.argv.index(name) + 1]).expanduser().resolve()


def with_external_provider(*args, **kwargs):
    specs = _ORIGINAL_PROCESS_SPECS(*args, **kwargs)
    root = Path(args[0])
    output = Path(args[1])
    trust_schema = Path(kwargs["trust_schema"])
    generation_manifest = Path(kwargs["source_generations_path"])

    controller_index = next(index for index, item in enumerate(specs)
                            if item.name == "controller")
    controller = specs[controller_index]
    controller_command = list(controller.command)
    controller_command[controller_command.index("--policy-file") + 1] = str(POLICY)
    identities_index = controller_command.index("--ensure-identities") + 1
    if PROVIDER_IDENTITY not in controller_command[identities_index].split(","):
        controller_command[identities_index] += "," + PROVIDER_IDENTITY
    specs[controller_index] = dataclasses.replace(
        controller, command=tuple(controller_command))

    ground_station_index = next(index for index, item in enumerate(specs)
                                if item.name == "ground-station")
    ground_station = specs[ground_station_index]
    specs[ground_station_index] = dataclasses.replace(
        ground_station,
        command=ground_station.command + ("--calibration-provider", PROVIDER_IDENTITY))

    payload_dir = output / "private/config/spec210-calibration-payloads"
    payload_dir.mkdir(parents=True, exist_ok=True)
    profile = load_observation_profile(argument_path("--observation-profile"))
    capture = cv2.VideoCapture(str(argument_path("--source") / "uav1_1min.mp4"))
    if not capture.isOpened():
        raise RuntimeError("cannot open the Spec191 video used to size provider calibrations")
    image_width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
    image_height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
    capture.release()
    payload_paths = []
    for camera in ("UAV1", "UAV2", "UAV3"):
        calibration = next(payload for kind, payload in profile_events(
            profile, camera, 1, 0, b"fixture-frame", source_id="spec210-provider-" + camera,
            epoch=1, image_width=image_width, image_height=image_height)
                           if kind == "calibration")
        encoded = json.loads(encode_event("calibration", calibration))
        payload = (json.dumps(encoded["payload"], ensure_ascii=False, sort_keys=True,
                              separators=(",", ":")) + "\n").encode("utf-8")
        path = payload_dir / f"calibration-{camera}.json"
        path.write_bytes(payload)
        payload_paths.append(str(path))

    helper = Path(os.environ["NDNSF_SPEC210_CALIBRATION_PROVIDER_HELPER"]).resolve()
    receipt = output / "private/config/external-calibration-refs.json"
    provider = demo.ProcessSpec(
        "external-source-calibration-provider", "uav1", PROVIDER_IDENTITY, root,
        (str(helper), str(trust_schema), str(receipt), str(generation_manifest),
         *payload_paths, "/example/uav/controller"),
        output / "private/calibration-provider")
    specs.insert(1, provider)
    return specs


_ORIGINAL_PROCESS_SPECS = demo._process_specs
demo._process_specs = with_external_provider

raise SystemExit(demo.main(sys.argv[1:]))
