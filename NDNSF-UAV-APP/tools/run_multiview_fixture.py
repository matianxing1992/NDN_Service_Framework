#!/usr/bin/env python3
"""Run the reproducible six-view functional gate for Spec 177."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from multiview_recognition_worker import run  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--views", type=int, default=6)
    parser.add_argument("--minimum-views", type=int, default=2)
    parser.add_argument("--model", default="")
    parser.add_argument("--model-digest", default="")
    parser.add_argument("--profile-id", default="")
    parser.add_argument("--provider", default="/provider/compute")
    parser.add_argument("--mission-id", default="mission-uav-mv")
    parser.add_argument("--job-id", default="recognition-001")
    parser.add_argument("--attempt", type=int, default=1)
    parser.add_argument("--confidence", type=float, default=0.25)
    parser.add_argument("--mode", choices=("functional", "real"), default="functional",
                        help="functional adapter for contract tests or strict MVCNN ONNX")
    parser.add_argument("--model-registry", default=str(ROOT / "NDNSF-UAV-APP/configs/uav_multiview_models.json"))
    parser.add_argument("--paired-baseline", action="store_true")
    args = parser.parse_args()
    result = run(args)
    print(f"status={result.get('status')} views={result.get('acceptedViewCount', len(result.get('contributingViews', [])))}")
    if result.get("status") == "completed":
        print(f"pooledFeatureDigest={result['fusionEvidence']['pooledFeatureDigest']}")
        print(f"result={Path(args.output).resolve() / 'result.json'}")
        return 0
    return 3


if __name__ == "__main__":
    raise SystemExit(main())
