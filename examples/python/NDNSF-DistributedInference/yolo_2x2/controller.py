#!/usr/bin/env python3
"""Run the NDNSF controller for the YOLO 2x2 + DistributedRepo example."""

from __future__ import annotations

import argparse

from ndnsf_distributed_inference import APPController


CONFIG_FILE = "examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_policy.yaml"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir",
                        default="/tmp/ndnsf-di-yolo-2x2-policy")
    args = parser.parse_args()
    controller = APPController.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
    )
    return controller.run()


if __name__ == "__main__":
    raise SystemExit(main())
