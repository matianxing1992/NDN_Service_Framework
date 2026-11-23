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
from ndnsf import _ndnsf


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

    def test_native_collaboration_reader_accepts_python_wire_field_names(self) -> None:
        """Keep the Python producer and native role selector interoperable."""
        for field in ("providerCapabilityHint", "provider_capability_hint"):
            with self.subTest(field=field):
                root = hint().to_ack_fields()["providerCapabilityHint"]
                wire = encode_ack_metadata({field: root})
                fields, kind, error, projection, conflicts = (
                    _ndnsf._read_provider_capability_ack(wire))
                self.assertEqual((kind, error), ("typed", ""))
                self.assertEqual(fields[field]["service_payload"]["role"], "/Stage/0")
                self.assertEqual(projection, fields[field])
                self.assertIsNone(conflicts)
                self.assertEqual(decode_provider_capability_ack(wire).hint.provider_name, "/P/one")

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

    def test_native_legacy_projection_preserves_alias_precedence_and_bounds(self) -> None:
        wire = (b"queueDepth=-2.5;queue=30;inflight=2.9;"
                b"status=ADMISSION-REJECTED;custom=retained;")
        result = decode_provider_capability_ack(
            wire, mode="mixed", provider_name="/P/context", service_name="/Inference/Test")
        self.assertEqual(result.hint.provider_name, "/P/context")
        self.assertEqual(result.hint.runtime_hint.queue_length, 0)
        self.assertEqual(result.hint.runtime_hint.active_work_count, 2)
        self.assertFalse(result.hint.ready)
        self.assertEqual(result.hint.service_payload["runtimeStatus"], "ADMISSION-REJECTED")
        self.assertEqual(result.fields["custom"], "retained")
        with self.assertRaisesRegex(AckMetadataDecodeError, "invalid work count"):
            decode_provider_capability_ack(
                wire + b"queueLength=3.1garbage;", mode="mixed",
                provider_name="/P/context", service_name="/Inference/Test")

    def test_matching_dual_is_counted_without_conflict(self) -> None:
        fields = hint().to_ack_fields()
        fields.update({"provider": "/P/one", "queueLength": "2.0", "queue": 99,
                       "activeWorkers": 1})
        counters = AckCompatibilityCounters()
        result = decode_provider_capability_ack(
            encode_ack_metadata(fields),
            mode="mixed",
            counters=counters,
        )
        self.assertEqual(result.conflicting_fields, ())
        self.assertEqual(result.hint.runtime_hint.queue_length, 2)
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

    def test_empty_and_corrupt_typed_fields_do_not_downgrade(self) -> None:
        legacy = b"provider=/P/legacy;service=/Inference/Test;role=/Stage/0;"
        for invalid in (b"providerCapabilityHint=;",
                        b"provider_capability_hint=json64:e30=garbage;"):
            with self.subTest(invalid=invalid):
                counters = AckCompatibilityCounters()
                with self.assertRaisesRegex(AckMetadataDecodeError, "malformed"):
                    decode_provider_capability_ack(legacy + invalid, mode="mixed", counters=counters)
                self.assertEqual(counters.snapshot()["malformedTyped"], 1)
                self.assertEqual(counters.snapshot()["legacy"], 0)
        wrapped = b"x-providerCapabilityHint=json64:e30=;"
        self.assertEqual(decode_provider_capability_ack(legacy + wrapped, mode="mixed").source,
                         "legacy")

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
        provider = (root / "NDNSF-DistributedInference/"
                    "ndnsf_distributed_inference/provider.py").read_text(encoding="utf-8")
        self.assertNotIn('payload << "roles="', native)
        self.assertIn("ndnsf-provider-capability-v2", native)
        self.assertNotIn("fields.update(ProviderCapabilityHint(", provider)


if __name__ == "__main__":
    unittest.main()
