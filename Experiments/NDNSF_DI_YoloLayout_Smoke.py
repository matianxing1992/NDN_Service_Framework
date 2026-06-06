#!/usr/bin/env python3
"""Fast regression for custom YOLO ONNX layout policy generation.

This smoke intentionally does not start MiniNDN. It verifies the current stable
custom-layout boundary: the YOLO-specific splitter can export the requested
layout, the exported ONNX chunks reproduce full-model output locally, and the
generated policy passes DI validation. Full network-level custom-layout
execution is still experimental; use the 2x2 MiniNDN case for the stable network
regression.
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
    args = parser.parse_args()

    layout = args.layout.strip().lower().replace("*", "x")
    safe_layout = layout.replace("/", "-")
    out_dir = Path(args.out_dir or f"/tmp/ndnsf-di-yolo-layout-{safe_layout}")
    policy = out_dir / "yolo_policy.yaml"
    generated_policy_dir = out_dir / "generated-policy"
    env = dict(os.environ)
    env["PYTHONPATH"] = ":".join([
        str(REPO / "NDNSF-DistributedInference"),
        str(REPO / "pythonWrapper"),
        str(REPO / "examples/python/NDNSF-DistributedInference/yolo_2x2"),
        env.get("PYTHONPATH", ""),
    ])

    split_output = run([
        sys.executable,
        "examples/python/NDNSF-DistributedInference/yolo_2x2/split_model.py",
        "--auto-split",
        "--layout",
        layout,
        "--out-dir",
        str(out_dir / "model"),
        "--policy",
        str(policy),
    ], env)
    if "YOLO_LAYOUT_LOCAL_VERIFY" not in split_output or "ok=true" not in split_output:
        raise SystemExit(f"YOLO layout local verification failed for layout={layout}")

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
        f"layout={layout} policy={policy} generated_policy_dir={generated_policy_dir}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
