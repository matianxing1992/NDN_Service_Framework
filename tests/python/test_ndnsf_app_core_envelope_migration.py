#!/usr/bin/env python3
"""Repo/UAV/DI migration tests for reusable NDNSF core envelopes."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from ndnsf import (
    AckCandidate,
    GenericProviderRuntimeHint,
    ProviderCapabilityHint,
    ServiceOperationStatus,
    StreamHealth,
    encode_ack_metadata,
    parse_ack_metadata,
)
from ndnsf_distributed_inference.artifact_deployment import ArtifactProvisioningState
from py_repoclient.orchestration import (
    NetworkDistributedRepoClient,
    RepoNodeApp,
    RepoObjectManifest,
    StorageCapability,
)
from ndnsf_distributed_inference.runtime_v1 import ModelFragmentKey, ProviderFragmentInventoryManager
from py_repoclient import capability_from_ack as repo_client_capability_from_ack


class _FakeRepoStore:
    def __init__(self) -> None:
        self.objects: dict[str, tuple[RepoObjectManifest, bytes]] = {}

    def inventory(self) -> list[str]:
        return list(self.objects)


class AppCoreEnvelopeMigrationTest(unittest.TestCase):
    def test_repo_ack_carries_only_core_provider_capability_hint(self) -> None:
        repo = RepoNodeApp.__new__(RepoNodeApp)
        repo.repo_node = "/repo/A"
        repo.service_name = "/NDNSF/DistributedRepo"
        repo.capability = StorageCapability(
            repo_node="/repo/A",
            free_bytes=1024,
            used_bytes=128,
            recent_load=0.25,
            availability_score=0.9,
            failure_domain="rack-1",
            storage_classes=("model",),
            repo_mode="persistent",
            accepts_backup_replica=True,
        )
        repo.capacity_bytes = 1024
        repo.memory_cache_bytes = 4096
        repo._cache_bytes = 256
        repo._db = None
        repo._store = _FakeRepoStore()
        repo._has_manifest = lambda _name: False
        repo._has_object = lambda _name: False
        repo._capability = lambda: repo.capability
        repo._runtime_snapshot = lambda: {
            "metricsTimestampMs": 1,
            "inflightReads": 0,
            "inflightWrites": 0,
            "inflightRepair": 0,
            "queueDepth": 0,
            "storageReadLatencyMs": 0.0,
            "storageWriteLatencyMs": 0.0,
            "rejected": 0,
        }

        decision = repo._ack(b'{"operation":"CAPABILITY"}')
        fields = parse_ack_metadata(decision.payload)
        hint = ProviderCapabilityHint.from_ack_fields(fields)

        self.assertTrue(decision.status)
        self.assertNotIn("repoNode", fields)
        self.assertEqual(set(fields), {"providerCapabilityHint"})
        self.assertEqual(hint.provider_name, "/repo/A")
        self.assertEqual(hint.service_payload["freeBytes"], 1024)
        self.assertEqual(hint.service_payload_schema, "ndnsf-repo-capability-v1")

    def test_network_repo_client_parses_core_capability_hint_from_ack(self) -> None:
        repo = RepoNodeApp.__new__(RepoNodeApp)
        repo.repo_node = "/repo/A"
        repo.service_name = "/NDNSF/DistributedRepo"
        repo.capability = StorageCapability(repo_node="/repo/A", free_bytes=2048)
        repo.capacity_bytes = 2048
        repo.memory_cache_bytes = 0
        repo._cache_bytes = 0
        repo._db = None
        repo._store = _FakeRepoStore()
        repo._has_manifest = lambda _name: False
        repo._has_object = lambda _name: False
        repo._capability = lambda: repo.capability
        repo._runtime_snapshot = lambda: {
            "metricsTimestampMs": 1,
            "inflightReads": 0,
            "inflightWrites": 0,
            "inflightRepair": 0,
            "queueDepth": 0,
            "storageReadLatencyMs": 0.0,
            "storageWriteLatencyMs": 0.0,
            "rejected": 0,
        }
        decision = repo._ack(b'{"operation":"STORE","objectName":"/missing"}')

        candidate = AckCandidate(
            "/provider/repo-A",
            "/NDNSF/DistributedRepo",
            "req-1",
            decision.status,
            decision.message,
            decision.payload,
        )
        fields = NetworkDistributedRepoClient._parse_ack_payload(candidate.payload)

        self.assertEqual(fields["repoNode"], "/repo/A")
        self.assertEqual(int(fields["freeBytes"]), 2048)

    def test_distributed_repo_client_prefers_core_hint_over_conflicting_legacy_fields(self) -> None:
        hint = ProviderCapabilityHint(
            provider_name="/repo/core",
            service_name="/NDNSF/DistributedRepo",
            runtime_hint=GenericProviderRuntimeHint(
                provider_name="/repo/core",
                capacity_hints={"freeBytes": 4096},
            ),
            service_payload_schema="ndnsf-repo-capability-v1",
            service_payload={
                "repoNode": "/repo/core",
                "freeBytes": 4096,
                "usedBytes": 12,
                "load": 0.1,
                "availability": 0.95,
                "storageClasses": ["model", "intermediate"],
                "repoMode": "persistent",
            },
        )
        payload = encode_ack_metadata({
            "repoNode": "/repo/legacy",
            "freeBytes": 1,
            **hint.to_ack_fields(),
        })
        candidate = AckCandidate(
            "/provider/repo",
            "/NDNSF/DistributedRepo",
            "req-1",
            True,
            "repo-ready",
            payload,
        )

        capability = repo_client_capability_from_ack(candidate)

        self.assertIsNotNone(capability)
        self.assertEqual(capability.repo_node, "/repo/core")
        self.assertEqual(capability.free_bytes, 4096)
        self.assertEqual(list(capability.storage_classes), ["model", "intermediate"])

    def test_repo_operation_helper_emits_core_status_and_data_reference(self) -> None:
        repo = RepoNodeApp.__new__(RepoNodeApp)
        repo.repo_node = "/repo/A"
        repo.service_name = "/NDNSF/DistributedRepo"
        manifest = RepoObjectManifest(
            object_name="/model/stage0",
            object_type="model-artifact",
            sha256="abc123",
            size=12,
            segment_count=2,
        )

        payload = repo._operation_status_payload("STORE", "stored", manifest=manifest)
        status = ServiceOperationStatus.from_dict(payload["operationStatus"])

        self.assertTrue(status.terminal)
        self.assertEqual(status.operation, "STORE")
        self.assertEqual(payload["dataProductReference"]["object_name"], "/model/stage0")
        self.assertEqual(payload["dataProductReference"]["object_class"], "model-artifact")

    def test_di_inventory_manager_exposes_core_capability_hint(self) -> None:
        key = ModelFragmentKey(
            model_id="qwen",
            model_digest="sha256:model",
            runtime_backend="onnx-cpu",
            fragment_digest="sha256:stage0",
        )
        with tempfile.TemporaryDirectory() as tmp:
            artifact = Path(tmp) / "stage0.onnx"
            artifact.write_bytes(b"x")
            manager = ProviderFragmentInventoryManager("/provider/A", queue_length=2)
            manager.register_fragment(key, disk_path=artifact)

            hint = manager.capability_hint("/Inference/NativeTracer")

        self.assertEqual(hint.provider_name, "/provider/A")
        self.assertEqual(hint.runtime_hint.queue_length, 2)
        self.assertEqual(hint.service_payload_schema, "ndnsf-di-runtime-ack-v1")
        self.assertEqual(hint.service_payload["fragment_states"][0]["residency"], "DISK_RESIDENT")

    def test_artifact_provisioning_ack_carries_core_operation_status(self) -> None:
        state = ArtifactProvisioningState(
            component="stage0",
            initial_status="installing",
            initial_message="materializing",
        )
        ack = state.ack()
        fields = parse_ack_metadata(ack.payload)
        status = ServiceOperationStatus.from_dict(fields["operationStatus"])

        self.assertFalse(ack.status)
        self.assertEqual(fields["runtimeStatus"], "installing")
        self.assertEqual(status.operation, "ARTIFACT_PROVISION")
        self.assertEqual(status.state.value, "RUNNING")

    def test_uav_can_map_generic_stream_metrics_to_core_stream_health(self) -> None:
        from ndnsf import StreamInfo, StreamMetrics

        info = StreamInfo("uav-video", 1, "/uav/drone/video")
        metrics = StreamMetrics(gaps=1, nacks=1)
        health = StreamHealth.from_stream(info, metrics, now_ms_value=1000)

        self.assertEqual(health.state.value, "DEGRADED")
        self.assertEqual(health.reason, "loss-or-gap")


if __name__ == "__main__":
    unittest.main()
