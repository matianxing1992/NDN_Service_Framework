#!/usr/bin/env python3
"""Create a frozen controlled multi-view dataset registration.

The adapter consumes a directory containing ``samples.json``.  It never
downloads data and refuses a registration without synchronization, calibration,
ground truth, and an explicit license/source record.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from multiview_contract import dump_json, sha256_file  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, help="dataset directory")
    parser.add_argument("--output", required=True)
    parser.add_argument("--dataset-id", default="coperception-uav-registered-v1")
    parser.add_argument("--license", required=True)
    parser.add_argument("--model-profile-id", default="vehicle-mvcnn-v1")
    args = parser.parse_args()
    source = Path(args.source).resolve()
    metadata_path = source / "samples.json"
    if not metadata_path.is_file():
        print("samples.json is required", file=sys.stderr)
        return 2
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    if not metadata.get("synchronized") or not metadata.get("calibration"):
        print("registration requires synchronized=true and calibration metadata", file=sys.stderr)
        return 2
    if not metadata.get("ground_truth"):
        print("registration requires ground_truth metadata", file=sys.stderr)
        return 2
    samples = metadata.get("samples")
    if not isinstance(samples, list) or not samples:
        print("registration requires non-empty samples", file=sys.stderr)
        return 2
    frozen_samples = []
    for sample in samples:
        views = sample.get("views", [])
        if not sample.get("sample_id") or not sample.get("target_id") or len(views) < 2:
            print("each sample needs an id, target, and at least two views", file=sys.stderr)
            return 2
        frozen_views = []
        for view in views:
            path = (source / str(view["file"])).resolve()
            if not path.is_file():
                print(f"missing sample view: {path}", file=sys.stderr)
                return 2
            frozen_views.append({
                "viewId": str(view["view_id"]),
                "file": str(path),
                "sha256": sha256_file(path),
                "captureTimeMs": int(view.get("capture_time_ms", 0)),
                "cameraPose": view.get("camera_pose", {}),
            })
        frozen_samples.append({
            "sampleId": str(sample["sample_id"]),
            "targetId": str(sample["target_id"]),
            "groundTruthLabel": str(sample.get("ground_truth_label", "car")),
            "views": frozen_views,
        })
    registration = {
        "schema": "ndnsf-uav-multiview-dataset-registration/v1",
        "datasetId": args.dataset_id,
        "source": str(metadata.get("source", "local-controlled-dataset")),
        "license": args.license,
        "modelProfileId": args.model_profile_id,
        "sourceManifestDigest": "sha256:" + sha256_file(metadata_path),
        "synchronized": True,
        "calibration": metadata["calibration"],
        "groundTruth": metadata["ground_truth"],
        "oneViewRule": "first view in each frozen sample",
        "groupingRule": "all views in a sample, evaluated at 1/2/4/6 prefixes",
        "viewCounts": [1, 2, 4, 6],
        "preprocessingPolicy": metadata.get(
            "preprocessing_policy",
            "RGB decode; deterministic center crop/resize is fixed by the registered model profile",
        ),
        "sampleSizeRationale": metadata.get(
            "sample_size_rationale",
            "The checked-in fixture is an execution qualification sample; it is not sufficient for a scientific accuracy claim.",
        ),
        "samples": frozen_samples,
        "scientificAccuracyClaimAllowed": bool(metadata.get("scientific_accuracy_claim_allowed", False)),
    }
    dump_json(Path(args.output), registration)
    print(f"registered={args.dataset_id} samples={len(frozen_samples)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
