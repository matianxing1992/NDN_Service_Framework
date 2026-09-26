#!/usr/bin/env python3
"""Produce an estimated Qwen2.5 storage/GPU ladder without live actions."""

from __future__ import annotations

import argparse
import json


MODELS = {
    "0.5B": {"parameters_b": 0.49, "gpu": "rtx_5000", "license": "Apache-2.0"},
    "1.5B": {"parameters_b": 1.54, "gpu": "rtx_5000", "license": "Apache-2.0"},
    "3B": {"parameters_b": 3.09, "gpu": "rtx_5000", "license": "Qwen-Research"},
    "7B": {"parameters_b": 7.61, "gpu": "rtx_5000", "license": "Apache-2.0"},
    "14B": {"parameters_b": 14.7, "gpu": "rtx_6000-or-h100_80gb", "license": "Apache-2.0"},
    "32B": {"parameters_b": 32.5, "gpu": "h100_80gb", "license": "Apache-2.0"},
    "72B": {"parameters_b": 72.7, "gpu": "multi-h100_80gb", "license": "Qwen"},
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--quota-gib", type=float, required=True)
    parser.add_argument("--reserve-gib", type=float, default=20.0)
    parser.add_argument("--strategy", choices=("sequential", "retain-all"), default="sequential")
    parser.add_argument("--models", default=",".join(MODELS))
    args = parser.parse_args()
    if args.quota_gib <= 0 or args.reserve_gib < 0 or args.reserve_gib >= args.quota_gib:
        parser.error("quota must be positive and reserve must be smaller than quota")

    names = [item.strip() for item in args.models.split(",") if item.strip()]
    unknown = sorted(set(names) - set(MODELS))
    if unknown:
        parser.error("unknown models: " + ",".join(unknown))

    cells = []
    for name in names:
        item = MODELS[name]
        weight_gib = item["parameters_b"] * 2_000_000_000 / (1024 ** 3)
        # Planning only: source + ONNX + manifests/caches while conversion is active.
        projected_gib = weight_gib * 2.35 + 5.0
        cells.append({
            "model": "Qwen2.5-" + name + "-Instruct",
            "parametersB": item["parameters_b"],
            "estimatedFp16WeightGiB": round(weight_gib, 2),
            "estimatedProjectPeakGiB": round(projected_gib, 2),
            "firstGpuClass": item["gpu"],
            "licenseClass": item["license"],
        })

    required = max(c["estimatedProjectPeakGiB"] for c in cells) if args.strategy == "sequential" else sum(c["estimatedProjectPeakGiB"] for c in cells)
    usable = args.quota_gib - args.reserve_gib
    output = {
        "schema": "ndnsf-di-itiger-qwen-matrix-plan-v1",
        "authority": "ESTIMATED_NOT_PREFLIGHT",
        "strategy": args.strategy,
        "quotaGiB": args.quota_gib,
        "reserveGiB": args.reserve_gib,
        "usableGiB": usable,
        "estimatedRequiredGiB": round(required, 2),
        "eligibleByEstimate": required <= usable,
        "models": cells,
        "warning": "Actual repository files, export amplification, cache, quota, and scratch must be measured before download or submission.",
    }
    print(json.dumps(output, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
