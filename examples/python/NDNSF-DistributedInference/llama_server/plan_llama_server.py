#!/usr/bin/env python3
"""Generate a Qwen2.5-0.5B GGUF + llama-server NDNSF-DI policy."""

from __future__ import annotations

import argparse
from pathlib import Path

from llama_server_lib import MODEL_NAME, SERVICE, write_policy


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--policy", default="/tmp/ndnsf-di-llama-server-policy.yaml")
    parser.add_argument("--service", default=SERVICE)
    parser.add_argument("--model", default=MODEL_NAME,
                        help="GGUF model path or artifact name, e.g. "
                             "Qwen2.5-0.5B-Instruct-Q4_K_M.gguf")
    parser.add_argument("--llama-server", default="llama-server",
                        help="Path to the pre-deployed llama-server executable")
    parser.add_argument("--providers", type=int, default=2)
    parser.add_argument("--predeployed-only", action="store_true",
                        help="Do not include GGUF/runtime artifact metadata; "
                             "providers are assumed to be manually prepared.")
    args = parser.parse_args()

    policy = write_policy(
        args.policy,
        model_path=args.model,
        llama_server_path=args.llama_server,
        service=args.service,
        provider_count=args.providers,
        include_artifacts=not args.predeployed_only,
    )
    print(
        "LLAMA_SERVER_QWEN_POLICY_OK",
        f"service={args.service}",
        "model_format=gguf",
        "runtime_backend=llama.cpp",
        f"providers={args.providers}",
        f"artifact_deployment={not args.predeployed_only}",
        f"policy={policy}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
