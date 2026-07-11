#!/usr/bin/env python3
"""Spec 090 typed ACK compatibility contract tests."""

from __future__ import annotations

import unittest
from pathlib import Path

from ndnsf import (
    AckCompatibilityCounters,
    AckCompatibilityMode,
    AckMetadataDecodeError,
    GenericProviderRuntimeHint,
    GenericAckMetadata,
    ProviderCapabilityHint,
    decode_provider_capability_ack,
    encode_ack_metadata,
    encode_provider_capability_ack,
)


def hint(*, queue: int = 2, schema: str = "ndnsf-provider-capability-v2"):
    return ProviderCapabilityHint(
        schema=schema,
        provider_name="/P/one",
        service_name="/Inference/Test",
        ready=True,
        runtime_hint=GenericProviderRuntimeHint(
            provider_name="/P/one",
            active_work_count=1,
            queue_length=queue,
            capacity_hints={"gpuMemoryMb": 4096},
        ),
        service_payload_schema="test-capability-v1",
        service_payload={"role": "/Stage/0", "residency": "GPU_LOADED"},
    )


class TypedAckCompatibilityTest(unittest.TestCase):
    def test_current_encoder_emits_one_typed_root(self) -> None:
        wire = encode_provider_capability_ack(hint())
        self.assertIn(b"providerCapabilityHint=json64:", wire)
        self.assertNotIn(b"queue=", wire)
        result = decode_provider_capability_ack(wire)
        self.assertEqual(result.source, "typed")
        self.assertEqual(result.hint.schema, "ndnsf-provider-capability-v2")
        self.assertEqual(result.hint.runtime_hint.queue_length, 2)
        self.assertEqual(result.hint.service_payload["residency"], "GPU_LOADED")

    def test_v1_typed_reader_is_supported_during_epoch(self) -> None:
        result = decode_provider_capability_ack(
            encode_provider_capability_ack(hint(schema="ndnsf-provider-capability-v1")))
        self.assertEqual(result.hint.schema, "ndnsf-provider-capability-v1")

    def test_legacy_only_requires_explicit_mixed_mode(self) -> None:
        legacy = encode_ack_metadata({
            "provider": "/P/legacy",
            "service": "/Inference/Test",
            "status": "ready",
            "queue": 3,
            "activeWorkers": 1,
            "role": "/Stage/0",
        })
        with self.assertRaises(AckMetadataDecodeError):
            decode_provider_capability_ack(legacy)
        counters = AckCompatibilityCounters()
        result = decode_provider_capability_ack(
            legacy,
            mode=AckCompatibilityMode.MIXED,
            counters=counters,
        )
        self.assertEqual(result.source, "legacy")
        self.assertEqual(result.hint.provider_name, "/P/legacy")
        self.assertEqual(result.hint.runtime_hint.queue_length, 3)
        self.assertEqual(counters.snapshot()["legacy"], 1)

    def test_matching_dual_is_counted_without_conflict(self) -> None:
        fields = hint().to_ack_fields()
        fields.update({"provider": "/P/one", "queue": 2, "activeWorkers": 1})
        counters = AckCompatibilityCounters()
        result = decode_provider_capability_ack(
            encode_ack_metadata(fields),
            mode="mixed",
            counters=counters,
        )
        self.assertEqual(result.conflicting_fields, ())
        self.assertEqual(counters.snapshot()["matchingDual"], 1)

    def test_conflicting_dual_uses_typed_and_counts_fields(self) -> None:
        fields = hint(queue=2).to_ack_fields()
        fields.update({"provider": "/P/wrong", "queue": 99, "activeWorkers": 7})
        counters = AckCompatibilityCounters()
        result = decode_provider_capability_ack(
            encode_ack_metadata(fields),
            mode=AckCompatibilityMode.MIXED,
            counters=counters,
        )
        self.assertEqual(result.hint.provider_name, "/P/one")
        self.assertEqual(result.hint.runtime_hint.queue_length, 2)
        self.assertIn("providerName", result.conflicting_fields)
        self.assertIn("queueLength", result.conflicting_fields)
        snapshot = counters.snapshot()
        self.assertEqual(snapshot["conflictingDual"], 1)
        self.assertEqual(snapshot["fieldConflicts"]["queueLength"], 1)

    def test_malformed_typed_does_not_fall_back_to_valid_legacy(self) -> None:
        counters = AckCompatibilityCounters()
        wire = encode_ack_metadata({
            "providerCapabilityHint": "not-an-object",
            "provider": "/P/legacy",
            "service": "/Inference/Test",
        })
        with self.assertRaisesRegex(AckMetadataDecodeError, "malformed"):
            decode_provider_capability_ack(
                wire, mode=AckCompatibilityMode.MIXED, counters=counters)
        self.assertEqual(counters.snapshot()["malformedTyped"], 1)
        self.assertEqual(counters.snapshot()["legacy"], 0)

    def test_unknown_typed_version_does_not_fall_back(self) -> None:
        counters = AckCompatibilityCounters()
        fields = hint(schema="ndnsf-provider-capability-v999").to_ack_fields()
        fields["provider"] = "/P/legacy"
        with self.assertRaisesRegex(AckMetadataDecodeError, "unknown"):
            decode_provider_capability_ack(
                encode_ack_metadata(fields),
                mode=AckCompatibilityMode.MIXED,
                counters=counters,
            )
        self.assertEqual(counters.snapshot()["unknownTypedVersion"], 1)

    def test_counter_restart_begins_from_zero(self) -> None:
        first = AckCompatibilityCounters()
        decode_provider_capability_ack(
            encode_provider_capability_ack(hint()), counters=first)
        self.assertEqual(first.snapshot()["typed"], 1)
        restarted = AckCompatibilityCounters()
        self.assertEqual(restarted.snapshot()["typed"], 0)
        self.assertEqual(restarted.snapshot()["fieldConflicts"], {})

    def test_generic_ack_metadata_remains_independent(self) -> None:
        metadata = GenericAckMetadata(
            provider_runtime_hint=GenericProviderRuntimeHint(
                provider_name="/P/one", queue_length=4),
            service_payload_schema="domain-v1",
            service_payload={"domainState": "preserved"},
        )
        fields = metadata.to_ack_fields()
        restored = GenericAckMetadata.from_ack_fields(fields)
        self.assertEqual(restored.provider_runtime_hint.queue_length, 4)
        self.assertEqual(restored.service_payload["domainState"], "preserved")

    def test_current_producers_do_not_emit_flat_capability_aliases(self) -> None:
        root = Path(__file__).resolve().parents[2]
        native = (root / "NDNSF-DistributedInference/cpp/ndnsf-di/"
                  "NativeProviderReadiness.cpp").read_text(encoding="utf-8")
        repo = (root / "NDNSF-DistributedRepo/pythonWrapper/py_repoclient/"
                "orchestration.py").read_text(encoding="utf-8")
        provider = (root / "NDNSF-DistributedInference/"
                    "ndnsf_distributed_inference/provider.py").read_text(encoding="utf-8")
        self.assertNotIn('payload << "roles="', native)
        self.assertIn("ndnsf-provider-capability-v2", native)
        self.assertNotIn("flat_legacy_fields", repo)
        self.assertNotIn("fields.update(ProviderCapabilityHint(", provider)


if __name__ == "__main__":
    unittest.main()
