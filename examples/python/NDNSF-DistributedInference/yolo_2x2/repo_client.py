#!/usr/bin/env python3
"""Publish and verify a YOLO model with the native DistributedRepo API."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import tempfile

from ndnsf import ServiceUser
from ndnsf_distributed_inference.app_sdk import APPDeployment
from py_repoclient import (
    ArtifactCapabilityRequirements,
    ArtifactReference,
    RepoClient,
    RepoWriteConsistency,
    SelectedRepoSet,
    SelectedRepoTarget,
)


CONFIG_FILE = "examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_policy.yaml"
REPO_SERVICE = "/NDNSF/DistributedRepo"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir",
                        default="/tmp/ndnsf-di-yolo-2x2-policy")
    parser.add_argument("--model", default="yolo26n.pt")
    parser.add_argument("--replication-factor", type=int, default=2)
    parser.add_argument("--max-bytes", type=int, default=0,
                        help="Optional smoke-test limit; 0 stores the full file")
    parser.add_argument("--ack-timeout-ms", type=int, default=1500)
    parser.add_argument("--timeout-ms", type=int, default=20000)
    args = parser.parse_args()

    if args.replication_factor <= 0:
        raise ValueError("replication-factor must be positive")
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
        adaptive_admission=False,
    )
    repo = RepoClient(
        user=user,
        repo_service_name=REPO_SERVICE,
        ack_timeout_ms=args.ack_timeout_ms,
        timeout_ms=args.timeout_ms,
    )
    source = Path(args.model)
    payload = source.read_bytes()
    if args.max_bytes > 0:
        payload = payload[:args.max_bytes]
    digest = hashlib.sha256(payload).hexdigest()
    logical_name = f"/NDNSF-DI/ARTIFACT/AI/YOLO/2x2/real-model/{digest}"
    reference = ArtifactReference(
        logical_name=logical_name,
        content_digest=digest,
        size_bytes=len(payload),
        root_manifest_name=logical_name + "/root",
        publisher_identity=deployment.user,
        policy_epoch="/Policy/yolo-2x2/v1",
    )
    requirements = ArtifactCapabilityRequirements()
    requirements.artifact = reference
    candidates = repo.discover_repo_candidates(
        requirements, timeout_ms=args.timeout_ms
    ).candidates
    if len(candidates) < args.replication_factor:
        raise RuntimeError(
            f"Repo discovery returned {len(candidates)} candidates; "
            f"need {args.replication_factor}"
        )
    selected = SelectedRepoSet()
    selected.targets = []
    for candidate in candidates[:args.replication_factor]:
        target = SelectedRepoTarget()
        target.repo_node = candidate.repo_node
        target.provider_identity = candidate.provider_identity
        target.service_prefix = candidate.service_prefix
        selected.targets.append(target)
    selected.seed_repo_id = selected.targets[0].repo_node
    selected.consistency = RepoWriteConsistency.ALL

    with tempfile.TemporaryDirectory(prefix="ndnsf-yolo-repo-publish-") as temp:
        source_path = Path(temp) / (source.name or "model.bin")
        source_path.write_bytes(payload)
        result = repo.publish_file(
            source_path, reference, selected, timeout_ms=args.timeout_ms)
        if not result.success or result.handle is None:
            raise RuntimeError("native Repo publication failed")
        destination = Path(temp) / "fetched.bin"
        repo.fetch_file(result.handle, destination, timeout_ms=args.timeout_ms)
        if destination.read_bytes() != payload:
            raise RuntimeError("native Repo fetch did not reproduce the published bytes")

    print("YOLO_2X2_NATIVE_REPO_OK")
    print("logical_name:", logical_name)
    print("digest:", "sha256:" + digest)
    print("size_bytes:", len(payload))
    print("replicas:", len(selected.targets))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
