#!/usr/bin/env python3
"""Verify that a second Spec 158 App build reused every foundation image."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


FOUNDATIONS = ("ml-devel", "ml-runtime", "ndn-devel", "ndn-runtime")


def load(path: Path) -> dict[str, object]:
    value = json.loads(path.read_text())
    if value.get("schemaVersion") != "spec158-layered-build-v1":
        raise ValueError(f"MANIFEST_SCHEMA_INVALID:{path}")
    if value.get("status") != "PASS":
        raise ValueError(f"MANIFEST_NOT_PASS:{path}")
    return value


def verify(first_path: Path, second_path: Path) -> dict[str, object]:
    first = load(first_path)
    second = load(second_path)
    first_images = first["images"]
    second_images = second["images"]
    assert isinstance(first_images, dict) and isinstance(second_images, dict)
    parent_ids: dict[str, str] = {}
    for product in FOUNDATIONS:
        first_id = first_images[product]["imageId"]
        second_id = second_images[product]["imageId"]
        if first_id != second_id:
            raise ValueError(
                f"FOUNDATION_IMAGE_DRIFT:{product}:{first_id}:{second_id}"
            )
        parent_ids[product] = str(first_id)
    if second.get("executedProducts") != ["app-runtime"]:
        raise ValueError("SECOND_BUILD_EXECUTED_FOUNDATION")
    first_app = first_images["app-runtime"]
    second_app = second_images["app-runtime"]
    if first_app["tag"] == second_app["tag"]:
        raise ValueError("APP_IDENTITY_NOT_DISTINCT")
    for gate in ("contentScan", "staticProbe"):
        value = second.get(gate)
        if not isinstance(value, dict) or value.get("status") != "PASS":
            raise ValueError(f"SECOND_BUILD_GATE_NOT_PASS:{gate}")
    return {
        "schemaVersion": "spec158-app-reuse-proof-v1",
        "status": "PASS",
        "firstManifest": str(first_path),
        "secondManifest": str(second_path),
        "foundationImageIds": parent_ids,
        "firstApp": {
            "tag": first_app["tag"],
            "imageId": first_app["imageId"],
        },
        "secondApp": {
            "tag": second_app["tag"],
            "imageId": second_app["imageId"],
        },
        "secondExecutedProducts": second["executedProducts"],
        "contentScan": second["contentScan"],
        "staticProbe": second["staticProbe"],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--first", required=True)
    parser.add_argument("--second", required=True)
    parser.add_argument("--output")
    args = parser.parse_args()
    try:
        report = verify(Path(args.first).resolve(), Path(args.second).resolve())
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        report = {
            "schemaVersion": "spec158-app-reuse-proof-v1",
            "status": "FAIL",
            "reasonCode": str(error),
        }
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    print(text, end="")
    if args.output:
        Path(args.output).write_text(text)
    return 0 if report["status"] == "PASS" else 4


if __name__ == "__main__":
    raise SystemExit(main())
