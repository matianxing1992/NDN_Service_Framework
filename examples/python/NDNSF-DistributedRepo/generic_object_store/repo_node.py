#!/usr/bin/env python3
"""Run one NDNSF-DistributedRepo node for the generic object-store example."""

from __future__ import annotations

import argparse

from ndnsf_distributed_inference import APPDeployment
from py_repoclient.orchestration import RepoNodeApp


CONFIG_FILE = "examples/python/NDNSF-DistributedRepo/generic_object_store/repo_policy.yaml"
REPO_SERVICE = "/NDNSF/DistributedRepo"
CONFIG_OBJECT = (
    "/example/repo/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/CONFIG/repo_policy.yaml"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir",
                        default="/tmp/ndnsf-distributed-repo-generic-policy")
    parser.add_argument("--provider-id", required=True)
    parser.add_argument("--repo-node", required=True)
    parser.add_argument("--free-bytes", type=int, default=2_000_000_000)
    parser.add_argument("--memory-cache-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--preallocate-bytes", type=int, default=0)
    parser.add_argument("--failure-domain", default="")
    parser.add_argument("--storage-dir", default="")
    parser.add_argument("--repo-mode", default="persistent")
    parser.add_argument("--accepts-backup-replica", default="true")
    parser.add_argument("--peer-repo-node", action="append", default=[])
    parser.add_argument("--catalog-sync-interval-s", type=float, default=10.0)
    parser.add_argument("--config-object", default=CONFIG_OBJECT,
                        help="Repo object name that stores the deployment config")
    parser.add_argument("--advertise-stored-prefixes", action="store_true",
                        help="Advertise stored Data prefixes through NLSR")
    parser.add_argument(
        "--artifact-format", action="append",
        choices=("artifact-manifest-v2", "exact-packet-v1"),
        help=(
            "Explicit supported artifact format; default is legacy "
            "exact-packet-v1 only"
        ),
    )
    parser.add_argument("--artifact-policy-epoch", default="default")
    parser.add_argument("--artifact-resume", action="store_true")
    parser.add_argument("--artifact-replica-receipts", action="store_true")
    parser.add_argument(
        "--artifact-read-only",
        action="store_true",
        help="Disable new artifact-manifest-v2 writes without deleting bytes",
    )
    parser.add_argument(
        "--artifact-max-write-schema-generation",
        type=int,
        default=None,
        help="Fail closed for v2 writes when the DB schema is newer",
    )
    args = parser.parse_args()

    deployment = APPDeployment.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
    ).deployment
    app = RepoNodeApp(
        repo_node=args.repo_node,
        service_name=REPO_SERVICE,
        provider_id=args.provider_id,
        group=deployment.group,
        controller=deployment.controller,
        provider_prefix=deployment.provider_prefix,
        trust_schema=deployment.trust_schema,
        free_bytes=args.free_bytes,
        failure_domain=args.failure_domain,
        storage_classes=("config", "log", "binary", "intermediate"),
        storage_dir=args.storage_dir or None,
        repo_mode=args.repo_mode,
        accepts_backup_replica=str(args.accepts_backup_replica).lower()
        not in {"0", "false", "no", "off"},
        peer_repo_nodes=(),
        catalog_sync_interval_s=args.catalog_sync_interval_s,
        memory_cache_bytes=args.memory_cache_bytes,
        preallocate_bytes=args.preallocate_bytes,
        advertise_stored_prefixes=args.advertise_stored_prefixes,
        artifact_format_versions=tuple(
            args.artifact_format or ("exact-packet-v1",)
        ),
        artifact_policy_epoch=args.artifact_policy_epoch,
        artifact_supports_resume=args.artifact_resume,
        artifact_supports_replica_receipts=(
            args.artifact_replica_receipts
        ),
        artifact_writes_enabled=not args.artifact_read_only,
        artifact_max_write_schema_generation=(
            args.artifact_max_write_schema_generation
        ),
    )
    app.seed_object(
        args.config_object,
        open(args.config, "rb").read(),
        object_type="deployment-config",
        policy_epoch="/Policy/generic-repo/bootstrap",
    )
    print(
        f"repo_node ready provider={args.repo_node} "
        f"seeded_config={args.config_object}",
        flush=True,
    )
    return app.run()


if __name__ == "__main__":
    raise SystemExit(main())
