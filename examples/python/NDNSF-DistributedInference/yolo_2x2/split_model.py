#!/usr/bin/env python3
"""Generate deterministic YOLO-style 2x2 shards and NDNSF-DI policy."""

from __future__ import annotations

from pathlib import Path

from yolo_2x2_lib import (
    parse_args_with_common,
    split_model,
    yolo_dynamic_splitter_output,
    yolo_splitter_output,
)


def main() -> int:
    parser = parse_args_with_common("Split the YOLO-style 2x2 demo model")
    parser.add_argument("--out-dir", default="/tmp/ndnsf-yolo-2x2")
    parser.add_argument("--policy", default="")
    parser.add_argument("--dynamic-provisioning", action="store_true")
    parser.add_argument("--trust-anchor-file", default="")
    args = parser.parse_args()

    split = split_model(args.out_dir)
    if args.dynamic_provisioning:
        output = yolo_dynamic_splitter_output(
            split,
            trust_anchor_file=args.trust_anchor_file,
        )
    else:
        output = yolo_splitter_output(split)
    policy = Path(args.policy) if args.policy else Path(args.out_dir) / "yolo_policy.yaml"
    output.write_policy_config(policy)
    print("YOLO_2X2_POLICY", policy)
    for artifact in output.service("/AI/YOLO/2x2Inference").artifacts:
        print("YOLO_2X2_ARTIFACT", artifact.role, artifact.path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
