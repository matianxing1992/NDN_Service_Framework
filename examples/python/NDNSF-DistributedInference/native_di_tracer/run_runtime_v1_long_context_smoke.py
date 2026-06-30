#!/usr/bin/env python3
"""Local Runtime v1 long-context smoke for NDNSF-DI.

This is a fast contract smoke. It does not claim MiniNDN network evidence; it
checks that long-context metadata, cache placement, telemetry export, and
Runtime v1 report generation are wired before the heavier MiniNDN campaign is
run.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from ndnsf_distributed_inference.runtime_v1 import (
    RuntimeTelemetryV1,
    export_telemetry_csv,
    runtime_v1_smoke,
    write_json,
    write_runtime_report,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    payload = runtime_v1_smoke()
    write_json(args.out_dir / "long-context-smoke.json", payload)
    export_telemetry_csv(
        args.out_dir / "telemetry.csv",
        [RuntimeTelemetryV1(provider=payload["cachePlacement"]["provider"])],
    )
    write_runtime_report(
        args.out_dir / "runtime-v1-report.json",
        telemetry={payload["cachePlacement"]["provider"]: RuntimeTelemetryV1(
            provider=payload["cachePlacement"]["provider"])},
        decision_table=[{
            "condition": "resident prefix cache hit",
            "decision": "reuse provider-local KV state",
            "provider": payload["cachePlacement"]["provider"],
        }],
    )
    print(json.dumps({
        "status": "ok",
        "outDir": str(args.out_dir),
        "allocation": payload["allocation"],
        "cachePlacement": payload["cachePlacement"],
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
