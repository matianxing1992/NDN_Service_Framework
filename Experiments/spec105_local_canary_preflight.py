#!/usr/bin/env python3
"""Create one immutable Spec 105 local-canary preflight record."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import time
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
CONTROLLING = REPO / "specs/105-ndnsf-di-deployment-readiness/evidence/telemetry-performance-check.md"
CAMPAIGN = REPO / "examples/ndnsf-di-qwen-pilot.campaign.json"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--record-id", required=True)
    args = parser.parse_args()
    if args.out.exists():
        raise SystemExit(f"output already exists: {args.out}")
    args.out.mkdir(parents=True)
    controlling_text = CONTROLLING.read_text(encoding="utf-8")
    controlling_status = "BLOCK" if "**Verdict**: **BLOCK**" in controlling_text else "UNKNOWN"
    payload = {
        "schema": "ndnsf-di-local-canary-preflight-v1",
        "recordId": args.record_id,
        "createdAtMs": int(time.time() * 1000),
        "sourceCommit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
        "host": {
            "hostname": platform.node(),
            "kernel": platform.release(),
            "machine": platform.machine(),
            "cpuCount": os.cpu_count(),
            "memoryBytes": os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES"),
            "diskFreeBytes": shutil.disk_usage(REPO).free,
        },
        "backend": {
            "kind": "onnxruntime-cpu",
            "device": "local-cpu",
            "physicalGpuEvidence": False,
        },
        "profile": {
            "campaign": str(CAMPAIGN.relative_to(REPO)),
            "campaignSha256": sha256(CAMPAIGN),
            "measurementSeconds": 60,
            "offeredRps": 1.0,
            "matchedCells": ["single-node-qwen", "three-provider-minindn-qwen"],
        },
        "controllingGate": {
            "task": "T062",
            "status": controlling_status,
            "evidence": str(CONTROLLING.relative_to(REPO)),
        },
        "status": "NOT_RUN_BLOCK" if controlling_status == "BLOCK" else "READY",
        "reason": "T062_IMMUTABLE_PERFORMANCE_BLOCK" if controlling_status == "BLOCK" else "",
        "liveQwenExecuted": False,
    }
    (args.out / "preflight.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 2 if controlling_status == "BLOCK" else 0


if __name__ == "__main__":
    raise SystemExit(main())
