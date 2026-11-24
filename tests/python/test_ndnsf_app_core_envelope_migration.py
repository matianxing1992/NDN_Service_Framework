#!/usr/bin/env python3
"""Repo/UAV/DI migration tests for reusable NDNSF core envelopes."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from ndnsf import (
    GenericProviderRuntimeHint,
    ProviderCapabilityHint,
    ServiceOperationStatus,
    StreamHealth,
    encode_ack_metadata,
    parse_ack_metadata,
)
from ndnsf_distributed_inference.artifact_deployment import ArtifactProvisioningState
from ndnsf_distributed_inference.runtime_v1 import ModelFragmentKey, ProviderFragmentInventoryManager


class AppCoreEnvelopeMigrationTest(unittest.TestCase):
    def test_core_hint_roundtrip_preserves_typed_payload(self):
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
