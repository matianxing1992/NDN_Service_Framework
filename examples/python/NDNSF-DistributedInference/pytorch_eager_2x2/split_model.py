#!/usr/bin/env python3
"""Generate PyTorch eager 2x2 shards and NDNSF-DI policy."""

from __future__ import annotations

from pathlib import Path

from pytorch_2x2_lib import parse_args_with_common, pytorch_splitter_output, split_model


def main() -> int:
    parser = parse_args_with_common("Split a tiny PyTorch eager MLP into 2x2 parts")
    parser.add_argument("--out-dir", default="/tmp/ndnsf-pytorch-eager-2x2")
    parser.add_argument("--policy", default="")
    args = parser.parse_args()

    split = split_model(args.out_dir)
    output = pytorch_splitter_output(split)
    policy = Path(args.policy) if args.policy else Path(args.out_dir) / "pytorch_policy.yaml"
    output.write_policy_config(policy)
    print("PYTORCH_2X2_POLICY", policy)
    for artifact in output.service("/AI/PyTorch/Eager2x2Inference").artifacts:
        print("PYTORCH_2X2_ARTIFACT", artifact.role, artifact.path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
