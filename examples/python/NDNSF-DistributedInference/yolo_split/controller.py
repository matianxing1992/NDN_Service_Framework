#!/usr/bin/env python3
"""ServiceController role for the YOLO split-collaboration example."""

from __future__ import annotations

import argparse

from ndnsf_distributed_inference import (
    APPController,
)
from yolo_split_lib import (
    CONFIG_FILE,
    optional_local_nfd,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=CONFIG_FILE,
                        help="User-facing distributed inference policy config")
    parser.add_argument("--generated-policy-dir", default="/tmp/ndnsf-di-yolo-policy",
                        help="Directory for generated trust schema and policy files")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--start-local-nfd", action="store_true")
    args = parser.parse_args()

    controller = APPController.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
    )
    identities = controller.deployment.bootstrap_identities
    if args.dry_run:
        print("Run Python ServiceController for YOLO split collaboration")
        print("config:", args.config)
        print("generated trust schema:", controller.deployment.trust_schema)
        print("generated policy:", controller.deployment.policy_file)
        print("bootstrap:", ", ".join(identities))
        return 0

    with optional_local_nfd(args.start_local_nfd):
        controller.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
