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


class AppCoreEnvelopeMigrationTest(unittest.TestCase):
    def test_retired_python_repo_cannot_create_a_provider_or_capability(self):
        from py_repoclient import RepoCompatibilityError

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for operation, call in (
                ("RepoNodeApp", lambda: RepoNodeApp(database_path=root / "repo.sqlite3")),
                ("StorageCapability", lambda: StorageCapability(repo_node="/repo/A", free_bytes=1024)),
                ("RepoObjectManifest", lambda: RepoObjectManifest(object_name="/model/stage0")),
            ):
                with self.subTest(operation=operation):
                    with self.assertRaises(RepoCompatibilityError) as error:
                        call()
                    self.assertEqual(error.exception.code, "SPEC192_COMPATIBILITY_V1")
                    self.assertEqual(error.exception.operation, operation)
                    self.assertEqual(list(root.iterdir()), [])

    def test_core_hint_roundtrip_survives_retirement_of_python_repo_decoders(self):
        from py_repoclient import RepoCompatibilityError

        hint = ProviderCapabilityHint(
            provider_name="/repo/core", service_name="/NDNSF/DistributedRepo",
            runtime_hint=GenericProviderRuntimeHint(
                provider_name="/repo/core", capacity_hints={"freeBytes": 4096}),
            service_payload_schema="ndnsf-repo-capability-v1",
            service_payload={"repoNode": "/repo/core", "freeBytes": 4096})
        payload = encode_ack_metadata({
            "repoNode": "/repo/legacy", "freeBytes": 1, **hint.to_ack_fields()})
        fields = parse_ack_metadata(payload)
        decoded = ProviderCapabilityHint.from_ack_fields(fields)
        self.assertEqual(decoded.provider_name, "/repo/core")
        self.assertEqual(decoded.service_payload["freeBytes"], 4096)
        self.assertEqual(fields["repoNode"], "/repo/legacy")
        candidate = AckCandidate(
            "/provider/repo", "/NDNSF/DistributedRepo", "req-1", True, "repo-ready", payload)
        with self.assertRaises(RepoCompatibilityError) as error:
            repo_client_capability_from_ack(candidate)
        self.assertEqual(error.exception.code, "SPEC192_COMPATIBILITY_V1")
        self.assertEqual(error.exception.operation, "capability_from_ack")

    def test_retired_network_client_cannot_emit_a_collaboration(self):
        from py_repoclient import RepoCompatibilityError

        class User:
            def begin_collaboration(self, *args, **kwargs):
                raise AssertionError("retired Python Repo client sent a request")

        with self.assertRaises(RepoCompatibilityError) as error:
            NetworkDistributedRepoClient(User(), "/NDNSF/DistributedRepo")
        self.assertEqual(error.exception.code, "SPEC192_COMPATIBILITY_V1")
        self.assertEqual(error.exception.operation, "NetworkDistributedRepoClient")

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
