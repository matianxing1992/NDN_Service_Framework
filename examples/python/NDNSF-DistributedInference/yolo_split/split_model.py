#!/usr/bin/env python3
"""Split YOLO and emit NDNSF-DistributedInference deployment policy."""

from __future__ import annotations

import argparse
from pathlib import Path

from yolo_split_lib import (
    DEFAULT_INPUT_SIZE,
    export_yolo_split_onnx,
    yolo_splitter_output,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="yolo26n.pt")
    parser.add_argument("--input-size", type=int, default=DEFAULT_INPUT_SIZE)
    parser.add_argument("--out-dir", default="/tmp/ndnsf-yolo-split")
    parser.add_argument("--policy", default="",
                        help="Generated yolo_policy.yaml path")
    args = parser.parse_args()

    output_dir = Path(args.out_dir)
    exported = export_yolo_split_onnx(args.model, output_dir, args.input_size)
    splitter_output = yolo_splitter_output(exported)
    policy_path = Path(args.policy) if args.policy else output_dir / "yolo_policy.yaml"
    splitter_output.write_policy_config(policy_path)

    service = splitter_output.service("/AI/YOLO/SplitInference")
    print("YOLO_SPLIT_POLICY", policy_path)
    for artifact in service.artifacts:
        print(
            "YOLO_SPLIT_ARTIFACT",
            artifact.role,
            artifact.artifact_name,
            artifact.path,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
