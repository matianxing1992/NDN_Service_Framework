#!/usr/bin/env python3
"""Run the NDNSF controller for the generic DistributedRepo example."""

from __future__ import annotations

import argparse
from ndnsf_distributed_inference import APPController


CONFIG_FILE = "examples/python/NDNSF-DistributedRepo/generic_object_store/repo_policy.yaml"
def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir",
                        default="/tmp/ndnsf-distributed-repo-generic-policy")
    args = parser.parse_args()
    controller = APPController.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
    )
    print("controller ready", flush=True)
    controller.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
