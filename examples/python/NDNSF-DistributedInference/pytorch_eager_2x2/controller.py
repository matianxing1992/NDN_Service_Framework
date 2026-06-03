#!/usr/bin/env python3
"""ServiceController for the PyTorch-defined fully connected ONNX 2x2 example."""

from __future__ import annotations

from ndnsf_distributed_inference import APPController

from pytorch_2x2_lib import optional_local_nfd, parse_args_with_common


def main() -> int:
    parser = parse_args_with_common("Run PyTorch-defined ONNX 2x2 controller")
    args = parser.parse_args()
    controller = APPController.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
    )
    if args.dry_run:
        print("Run PyTorch-defined ONNX 2x2 controller")
        print("config:", args.config)
        print("policy:", controller.deployment.policy_file)
        return 0
    with optional_local_nfd(args.start_local_nfd):
        controller.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
