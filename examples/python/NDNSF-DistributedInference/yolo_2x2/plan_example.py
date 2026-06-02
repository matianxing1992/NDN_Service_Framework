#!/usr/bin/env python3
"""Inspect a YOLO 2x2 distributed-inference service policy.

This example intentionally focuses on the application-facing API. It shows how
an AI developer describes a two-stage, two-shard split without importing NDNSF
Core objects. 2x2 is only one convenient layout.
"""

from __future__ import annotations

from pathlib import Path
import hashlib

from ndnsf_distributed_inference import (
    APPDeployment,
    LocalDistributedRepo,
    PlacementPolicy,
    RepoObjectManifest,
    StorageCapability,
)


SERVICE = "/AI/YOLO/2x2Inference"
CONFIG_FILE = str(Path(__file__).with_name("yolo_policy.yaml"))


def fake_onnx_payload(stage: int, shard: int) -> bytes:
    """Small placeholder artifact for plan/API smoke tests.

    A real application would pass an ONNX shard path or bytes exported by its
    model splitter.
    """

    return f"placeholder-yolo-onnx-stage={stage}-shard={shard}\n".encode()


def build_repo() -> LocalDistributedRepo:
    return LocalDistributedRepo([
        StorageCapability(
            "/NDNSF-DistributeInference/example/repo/A",
            free_bytes=4_000_000_000,
            recent_load=0.10,
            availability_score=0.99,
            failure_domain="rack-a",
        ),
        StorageCapability(
            "/NDNSF-DistributeInference/example/repo/B",
            free_bytes=3_000_000_000,
            recent_load=0.05,
            availability_score=0.98,
            failure_domain="rack-b",
        ),
        StorageCapability(
            "/NDNSF-DistributeInference/example/repo/C",
            free_bytes=2_000_000_000,
            recent_load=0.25,
            availability_score=0.97,
            failure_domain="rack-c",
        ),
    ])


def inspect_yolo_2x2_policy():
    deployment = APPDeployment.from_config(
        CONFIG_FILE,
        generated_policy_dir="/tmp/ndnsf-di-yolo-2x2-policy",
    )
    repo = build_repo()
    placement = PlacementPolicy(replication_factor=2)
    service = deployment.deployment.service_policy(SERVICE)
    manifests = {}

    for stage in range(2):
        for shard in range(2):
            payload = fake_onnx_payload(stage, shard)
            object_name = (
                "/NDNSF-DistributeInference/example/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/"
                "NDNSF-DI/ARTIFACT/AI/YOLO/2x2"
                f"/Stage/{stage}/Shard/{shard}/model"
            )
            manifest = repo.put(
                object_name=object_name,
                payload=payload,
                object_type="onnx-model",
                policy=placement,
                policy_epoch="/Policy/yolo-2x2/v1",
            )
            manifests[f"/Stage/{stage}/Shard/{shard}"] = manifest.to_dict()

    return service, manifests, deployment, repo


def validate_repo_artifacts(manifests, repo: LocalDistributedRepo) -> None:
    for manifest_dict in manifests.values():
        manifest = RepoObjectManifest.from_dict(manifest_dict)
        payload = repo.fetch_object(manifest.object_name, manifest)
        digest = hashlib.sha256(payload).hexdigest()
        if digest != manifest.sha256:
            raise RuntimeError(f"repo artifact hash mismatch for {manifest.object_name}")


def validate_repo_intermediate_data(repo: LocalDistributedRepo) -> None:
    payload = b"activation tensor from stage0 shards to stage1 shards"
    manifest = repo.put(
        object_name=(
            "/NDNSF-DistributeInference/example/user/NDNSF-DISTRIBUTED-REPO/OBJECT/"
            "NDNSF-DI/INTERMEDIATE/AI/YOLO/2x2/session-0/stage0-to-stage1"
        ),
        payload=payload,
        object_type="activation",
        policy=PlacementPolicy(replication_factor=1),
        policy_epoch="/Policy/yolo-2x2/v1",
    )
    fetched = repo.fetch_object(manifest.object_name, manifest)
    if fetched != payload:
        raise RuntimeError("repo intermediate activation fetch mismatch")


def main() -> int:
    service, manifests, deployment, repo = inspect_yolo_2x2_policy()
    validate_repo_artifacts(manifests, repo)
    validate_repo_intermediate_data(repo)
    print("APP API service policy smoke")
    print("service:", service.name)
    print("policy trust schema:", deployment.trust_schema)
    print("repo objects:", len(repo.inventory()))
    print("repo nodes:")
    for capability in repo.capabilities:
        print(f"  {capability.repo_node} free={capability.free_bytes} "
              f"used={capability.used_bytes} load={capability.recent_load}")
    print("roles:")
    for role in service.roles:
        manifest = manifests[role]
        replicas = ",".join(manifest["replicaNodes"])
        print(f"  {role} repo={manifest['objectName']} replicas={replicas}")
    print("dependencies:")
    for dep in service.dependencies:
        producers = ",".join(dep.producers)
        consumers = ",".join(dep.consumers)
        print(f"  {dep.key_scope}: {producers} -> {consumers} "
              f"topic={dep.topic_prefix}")
    print("provider role dependency views:")
    dependency_graph = deployment.deployment.dependency_graph_for_service(SERVICE)
    for role in service.roles:
        view = dependency_graph.for_role(role)
        print(f"  {role}: "
              f"inputs={[edge.key_scope for edge in view.inputs]} "
              f"outputs={[edge.key_scope for edge in view.outputs]} "
              f"internal={[edge.key_scope for edge in view.internal]}")
    print("DISTRIBUTED_REPO_YOLO_2X2_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
