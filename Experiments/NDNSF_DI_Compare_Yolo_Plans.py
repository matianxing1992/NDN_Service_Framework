#!/usr/bin/env python3
"""Compare local and distributed YOLO inference plans for NDNSF-DI."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import time
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
PY_DIR = REPO / "examples/python/NDNSF-DistributedInference/yolo_split"


def python_path() -> str:
    return ":".join([
        str(REPO / "NDNSF-DistributedInference"),
        str(REPO / "pythonWrapper"),
        str(PY_DIR),
        str(REPO / "examples/python/NDNSF-DistributedInference/yolo_2x2"),
        os.environ.get("PYTHONPATH", ""),
    ])


def ensure_auto_split_policy(work_dir: Path) -> Path:
    policy = work_dir / "yolo_policy.yaml"
    if policy.exists():
        return policy
    work_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        "python3",
        str(PY_DIR / "split_model.py"),
        "--model",
        "yolo26n.pt",
        "--input-size",
        "32",
        "--auto-split",
        "--out-dir",
        str(work_dir / "model"),
        "--policy",
        str(policy),
    ], cwd=str(REPO), env={**os.environ, "PYTHONPATH": python_path()}, check=True)
    return policy


def run_local_full_onnx(policy: Path, iterations: int) -> dict:
    env = {**os.environ, "PYTHONPATH": python_path()}
    code = r'''
import json
import sys
import time
from pathlib import Path

import numpy as np
from yolo_split_lib import full_forward_from_policy_onnx, make_input

policy = Path(sys.argv[1])
iterations = int(sys.argv[2])
x = make_input(32)
latencies = []
for _ in range(iterations):
    start = time.perf_counter()
    y = full_forward_from_policy_onnx(policy, x)
    latencies.append((time.perf_counter() - start) * 1000.0)
assert y is not None
payload = {
    "plan": "single-node-full-onnx",
    "iterations": iterations,
    "shape": list(np.asarray(y).shape),
    "avgMs": sum(latencies) / len(latencies),
    "minMs": min(latencies),
    "maxMs": max(latencies),
}
print(json.dumps(payload, sort_keys=True))
'''
    out = subprocess.check_output(
        ["python3", "-c", code, str(policy), str(iterations)],
        cwd=str(REPO),
        env=env,
        text=True,
    )
    return json.loads(out)


def run_minindn_case(case: str) -> dict:
    proc = subprocess.run([
        "sudo",
        "-E",
        "python3",
        str(REPO / "Experiments/NDNSF_DI_Run_Minindn_Regressions.py"),
        "--case",
        case,
    ], cwd=str(REPO), text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    marker = re.search(
        r"NDNSF_DI_REGRESSION_OK case=(?P<case>\S+) .* elapsed_s=(?P<elapsed>[0-9.]+)",
        proc.stdout,
    )
    if proc.returncode != 0 or marker is None:
        print(proc.stdout)
        raise RuntimeError(f"MiniNDN case {case} failed with rc={proc.returncode}")
    return {
        "plan": f"minindn-{case}",
        "elapsedS": float(marker.group("elapsed")),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--work-dir", default=str(REPO / "results/yolo_di_comparison"))
    parser.add_argument("--iterations", type=int, default=5)
    parser.add_argument("--include-minindn-auto-split", action="store_true")
    parser.add_argument("--include-minindn-2x2", action="store_true")
    parser.add_argument("--output", default="")
    args = parser.parse_args()

    work_dir = Path(args.work_dir)
    policy = ensure_auto_split_policy(work_dir)
    results = [run_local_full_onnx(policy, max(1, args.iterations))]
    if args.include_minindn_auto_split:
        results.append(run_minindn_case("auto-split"))
    if args.include_minindn_2x2:
        results.append(run_minindn_case("yolo-2x2"))

    payload = {
        "policy": str(policy),
        "results": results,
    }
    if args.output:
        target = Path(args.output)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")
    print(json.dumps(payload, indent=2, sort_keys=True))
    print(
        "NDNSF_DI_COMPARISON_RESULT "
        f"plans={len(results)} output={args.output or '<stdout>'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
