#!/usr/bin/env python3
"""Run the NDNSF controller for the llama-server DI example."""

from __future__ import annotations

import argparse

from ndnsf_distributed_inference import APPController


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="/tmp/ndnsf-di-llama-server-policy.yaml")
    parser.add_argument("--generated-policy-dir", default="/tmp/ndnsf-di-llama-server-generated")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    controller = APPController.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
    )
    if args.dry_run:
        print("LLAMA_SERVER_CONTROLLER_DRY_RUN")
        print("config:", args.config)
        print("generated trust schema:", controller.deployment.trust_schema)
        print("generated policy:", controller.deployment.policy_file)
        print("bootstrap:", ", ".join(controller.deployment.bootstrap_identities))
        return 0
    return controller.run()


if __name__ == "__main__":
    raise SystemExit(main())
