#!/usr/bin/env python3
"""Deploy Qwen GGUF + llama-server artifacts into DistributedRepo."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from ndnsf import ServiceUser
from ndnsf_distributed_inference.app_sdk import APPDeployment
from py_repoclient import RepoClient

from llama_server_lib import (
    build_llama_server_artifact_references,
    write_artifact_references,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="/tmp/ndnsf-di-llama-server-policy.yaml")
    parser.add_argument("--generated-policy-dir", default="/tmp/ndnsf-di-llama-server-generated")
    parser.add_argument("--model", required=True,
                        help="Path to Qwen2.5-0.5B GGUF model")
    parser.add_argument("--llama-server", required=True,
                        help="Path to llama-server executable")
    parser.add_argument("--out", default="/tmp/ndnsf-di-llama-server-artifacts.json")
    parser.add_argument("--repo-service", default="/NDNSF/DistributedRepo")
    parser.add_argument("--replication-factor", type=int, default=1)
    parser.add_argument("--local-smoke-export", action="store_true",
                        help="Write a local-payload artifact reference without contacting NDNSF repo")
    args = parser.parse_args()

    if args.local_smoke_export:
        references = build_llama_server_artifact_references(
            None,
            model_path=args.model,
            llama_server_path=args.llama_server,
            replication_factor=args.replication_factor,
            include_local_payload_paths=True,
        )
    else:
        deployment = APPDeployment.from_config(
            args.config,
            generated_policy_dir=args.generated_policy_dir,
        ).deployment
        user = ServiceUser(
            group=deployment.group,
            controller=deployment.controller,
            user=deployment.user,
            trust_schema=deployment.trust_schema,
            permission_wait_ms=6000,
            handler_threads=1,
            ack_threads=1,
            adaptive_admission=False,
        )
        repo = RepoClient(
            user=user,
            repo_service_name=args.repo_service,
            ack_timeout_ms=1500,
            timeout_ms=60000,
        )
        references = build_llama_server_artifact_references(
            repo,
            model_path=args.model,
            llama_server_path=args.llama_server,
            replication_factor=args.replication_factor,
            publisher_identity=deployment.user,
        )

    target = write_artifact_references(args.out, references)
    print(
        "LLAMA_SERVER_ARTIFACT_DEPLOY_OK",
        f"out={target}",
        f"roles={','.join(sorted(references.get('roles', {})))}",
        f"bytes={len(json.dumps(references, sort_keys=True).encode())}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
