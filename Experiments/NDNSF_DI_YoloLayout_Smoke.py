#!/usr/bin/env python3
"""Fast regression for custom YOLO ONNX layout policy generation.

This smoke intentionally does not start MiniNDN. It verifies the current stable
custom-layout boundary: the YOLO-specific splitter can export the requested
layout, the exported ONNX chunks reproduce full-model output locally, and the
generated policy passes DI validation. Full network-level custom-layout
execution is still experimental; use the 2x2 MiniNDN case for the stable network
regression.

With --parallel-output-shards, the same smoke verifies the experimental true-NxM
YOLO output-shard prototype: same-stage shards are parallel roles and a /Merge
role restores the full prediction tensor.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]


def run(command: list[str], env: dict[str, str]) -> str:
    print("$ " + " ".join(command))
    proc = subprocess.run(
        command,
        cwd=str(REPO),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    print(proc.stdout, end="")
    if proc.returncode != 0:
        raise SystemExit(proc.returncode)
    return proc.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--layout", default="3x2",
                        help="YOLO stage-by-shard layout, e.g. 1x3, 2x3, 3x2, 3x3")
    parser.add_argument("--out-dir", default="",
                        help="Output directory. Defaults to /tmp/ndnsf-di-yolo-layout-<layout>.")
    parser.add_argument("--parallel-output-shards", action="store_true",
                        help="Validate the experimental true-NxM YOLO output-shard prototype")
    parser.add_argument("--parallel-detect-scale-shards", action="store_true",
                        help="Validate the YOLO Detect-scale DAG splitter")
    args = parser.parse_args()

    layout = args.layout.strip().lower().replace("*", "x")
    safe_layout = layout.replace("/", "-")
    if args.parallel_detect_scale_shards and args.parallel_output_shards:
        raise SystemExit("--parallel-detect-scale-shards and --parallel-output-shards are mutually exclusive")
    if args.parallel_detect_scale_shards:
        mode_suffix = "-parallel-detect-scale"
    elif args.parallel_output_shards:
        mode_suffix = "-parallel-output"
    else:
        mode_suffix = ""
    out_dir = Path(args.out_dir or f"/tmp/ndnsf-di-yolo-layout-{safe_layout}{mode_suffix}")
    policy = out_dir / "yolo_policy.yaml"
    generated_policy_dir = out_dir / "generated-policy"
    env = dict(os.environ)
    env["PYTHONPATH"] = ":".join([
        str(REPO / "NDNSF-DistributedInference"),
        str(REPO / "pythonWrapper"),
        str(REPO / "examples/python/NDNSF-DistributedInference/yolo_2x2"),
        env.get("PYTHONPATH", ""),
    ])

    split_command = [
        sys.executable,
        "examples/python/NDNSF-DistributedInference/yolo_2x2/split_model.py",
        "--auto-split",
        "--layout",
        layout,
        "--out-dir",
        str(out_dir / "model"),
        "--policy",
        str(policy),
    ]
    if args.parallel_output_shards:
        split_command.append("--parallel-output-shards")
    if args.parallel_detect_scale_shards:
        split_command.append("--parallel-detect-scale-shards")
    split_output = run(split_command, env)
    if "YOLO_LAYOUT_LOCAL_VERIFY" not in split_output or "ok=true" not in split_output:
        raise SystemExit(f"YOLO layout local verification failed for layout={layout}")
    if args.parallel_output_shards:
        if "semantics=parallel-output-channel-shards" not in split_output:
            raise SystemExit("parallel-output smoke did not generate parallel-output semantics")
        if "stage_shards_parallel=true" not in split_output:
            raise SystemExit("parallel-output smoke did not mark stage shards parallel")
    if args.parallel_detect_scale_shards:
        if "semantics=parallel-detect-scale-shards" not in split_output:
            raise SystemExit("parallel-detect-scale smoke did not generate detect-scale semantics")
        if "stage_shards_parallel=true" not in split_output:
            raise SystemExit("parallel-detect-scale smoke did not mark stage shards parallel")

    run([
        sys.executable,
        "-m",
        "ndnsf_distributed_inference.policy",
        "--config",
        str(policy),
        "--out-dir",
        str(generated_policy_dir),
        "--print-summary",
    ], env)

    print(
        "YOLO_LAYOUT_SMOKE_OK "
        f"layout={layout} "
        f"parallel_output_shards={str(args.parallel_output_shards).lower()} "
        f"parallel_detect_scale_shards={str(args.parallel_detect_scale_shards).lower()} "
        f"policy={policy} generated_policy_dir={generated_policy_dir}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
