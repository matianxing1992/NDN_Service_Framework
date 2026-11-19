#!/usr/bin/env python3
"""Run one persistent-in-job DistributedRepo node for Spec 162."""

import argparse
from pathlib import Path

from ndnsf_distributed_inference.app_sdk import APPDeployment
from py_repoclient.orchestration import RepoNodeApp


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True)
    parser.add_argument("--generated-policy-dir", required=True)
    parser.add_argument("--provider-id", default="")
    parser.add_argument("--provider-prefix", required=True)
    parser.add_argument("--repo-node", required=True)
    parser.add_argument("--storage-dir", required=True)
    parser.add_argument("--state-root", required=True)
    parser.add_argument("--free-bytes", type=int, required=True)
    parser.add_argument("--bootstrap-token-file", required=True)
    parser.add_argument(
        "--test-only-allow-ephemeral-app-state", action="store_true",
        help="Allow the named volatile state root only for an explicit test run.")
    args = parser.parse_args()
    deployment = APPDeployment.from_config(
        args.config,
        state_root=args.state_root,
        identity=f"repo-node-{args.provider_id or '0'}",
        generated_policy_dir=args.generated_policy_dir,
        test_only_allow_ephemeral_state_root=(
            args.test_only_allow_ephemeral_app_state),
    ).deployment
    app = RepoNodeApp(
        repo_node=args.repo_node,
        service_name="/NDNSF/DistributedRepo",
        provider_id=args.provider_id,
        group=deployment.group,
        controller=deployment.controller,
        provider_prefix=args.provider_prefix,
        trust_schema=deployment.trust_schema,
        free_bytes=args.free_bytes,
        failure_domain=args.repo_node,
        storage_dir=args.storage_dir,
        memory_cache_bytes=64 * 1024 * 1024,
        handler_threads=4,
        ack_threads=2,
        bootstrap_token=Path(args.bootstrap_token_file).read_text(
            encoding="utf-8").strip(),
        artifact_format_versions=("artifact-manifest-v2", "exact-packet-v1"),
        artifact_supports_resume=True,
        artifact_supports_replica_receipts=True,
    )
    print(
        "SPEC162_REPO_NODE_STARTING",
        f"repoNode={args.repo_node}",
        f"freeBytes={args.free_bytes}",
        flush=True,
    )
    return app.run()


if __name__ == "__main__":
    raise SystemExit(main())
