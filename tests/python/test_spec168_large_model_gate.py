#!/usr/bin/env python3
"""Bounded local qualification for the Spec 168 large-model control path."""

from __future__ import annotations

import hashlib
from pathlib import Path
import tempfile
import unittest

from py_repoclient import (
    AdaptiveArtifactTransfer,
    AdaptiveTransferOptions,
    ArtifactSegmentDisposition,
    ArtifactReference,
    AtomicArtifactDestination,
)
from ndnsf_distributed_inference.adapters.qwen.placement import (
    QWEN36_27B_LAYER_RANGES,
    QWEN36_STAGE_ROLES,
    build_qwen_three_stage_adapter,
    build_qwen36_27b_three_stage_adapter,
)


def digest(label: str) -> str:
    return "sha256:" + hashlib.sha256(label.encode()).hexdigest()


class Spec168LargeModelGateTest(unittest.TestCase):
    def test_adapter_emits_three_stage_graph_and_capacity_safe_partition(self):
        weights = {
            role: 19 * 1024**3 + index * 128 * 1024**2
            for index, role in enumerate(QWEN36_STAGE_ROLES)
        }
        adapter = build_qwen36_27b_three_stage_adapter(
            artifact_digests_by_role={
                role: digest("artifact:" + role) for role in QWEN36_STAGE_ROLES
            },
            weight_bytes_by_role=weights,
        )
        model = adapter.describe_model(
            "Qwen/Qwen3.6-27B", digest("model"), digest("tokenizer"),
            source_revision="6a9e13bd6fc8f0983b9b99948120bc37f49c13e9",
        )
        graph = adapter.graph.inspect(model)
        candidates = adapter.splitter.enumerate_candidates(model, graph)
        self.assertEqual(len(candidates), 1)
        candidate = candidates[0]
        self.assertEqual(candidate.execution_plan.roles, QWEN36_STAGE_ROLES)
        self.assertEqual(len(candidate.execution_plan.dependencies), 2)
        self.assertEqual(
            candidate.estimated_costs["decoder_layers"],
            QWEN36_27B_LAYER_RANGES[-1][1],
        )
        self.assertGreater(sum(weights.values()), 32_760 * 1024**2)
        for role, requirement in candidate.requirements_by_role.items():
            peak = requirement.estimated_peak_gpu_memory_bytes
            self.assertIsNotNone(peak)
            self.assertLess(peak, 32_760 * 1024**2)
            self.assertEqual(candidate.artifacts_by_role[role][0], digest("artifact:" + role))

        with self.assertRaisesRegex(ValueError, "contiguous"):
            build_qwen_three_stage_adapter(
                model_name="Qwen/Qwen3.6-27B",
                revision="invalid-range",
                layer_ranges=((0, 21), (22, 42), (42, 64)),
                artifact_digests_by_role={
                    role: digest("artifact:" + role) for role in QWEN36_STAGE_ROLES
                },
                weight_bytes_by_role=weights,
            )

    def test_bounded_transfer_window_backlog_retry_and_unequal_arrival(self):
        options = AdaptiveTransferOptions()
        options.initial_window = 2
        options.maximum_window = 4
        options.verification_backlog_limit = 2
        options.maximum_retries = 1
        options.segment_timeout_ms = 5
        transfer = AdaptiveArtifactTransfer(3, options)
        self.assertEqual([item.segment_no for item in transfer.poll(0)], [0, 1])
        self.assertEqual(
            transfer.receive(1, 4, 6, 1), ArtifactSegmentDisposition.ACCEPTED)
        self.assertEqual(
            transfer.receive(0, 4, 6, 2), ArtifactSegmentDisposition.ACCEPTED)
        self.assertEqual(
            transfer.receive(1, 4, 6, 3), ArtifactSegmentDisposition.DUPLICATE)
        transfer.mark_verified(0)
        transfer.mark_verified(1)
        self.assertEqual([item.segment_no for item in transfer.poll(4)], [2])
        transfer.receive(2, 4, 6, 5)
        transfer.mark_verified(2)
        self.assertTrue(transfer.snapshot().complete)

        retry = AdaptiveArtifactTransfer(1, options)
        retry.poll(0)
        retry.expire(5)
        requests = retry.poll(6)
        self.assertEqual(len(requests), 1)
        self.assertTrue(requests[0].retransmission)
        retry.receive(0, 4, 6, 7)
        retry.mark_verified(0)
        snapshot = retry.snapshot()
        self.assertTrue(snapshot.complete)
        self.assertEqual(snapshot.timeout_count, 1)
        self.assertEqual(snapshot.retransmission_count, 1)

    def test_atomic_destination_resumes_out_of_order_ranges_by_identity(self):
        payload = b"large-model-bounded-range-fixture"
        reference = ArtifactReference(
            logical_name="/model/qwen36/stage-0",
            digest_algorithm="sha256",
            content_digest=hashlib.sha256(payload).hexdigest(),
            size_bytes=len(payload),
            format_version="artifact-manifest-v2",
            root_manifest_name="/model/qwen36/root",
            publisher_identity="/publisher",
            policy_epoch="spec168",
        )
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "stage-0.bin"
            sink = AtomicArtifactDestination(
                destination, reference, "fetch-stage-0", max_range_bytes=8)
            # Deliberately unequal arrival order; no destination is visible
            # until every verified range is present.
            sink.write_range(16, payload[16:24])
            sink.write_range(24, payload[24:32])
            sink.write_range(32, payload[32:])
            sink.write_range(0, payload[:8])
            sink.write_range(8, payload[8:16])
            self.assertEqual(sink.finalize().read_bytes(), payload)

            resumed_destination = Path(temporary) / "stage-1.bin"
            first = AtomicArtifactDestination(
                resumed_destination, reference, "fetch-stage-1", max_range_bytes=8)
            first.write_range(0, payload[:8])
            first.abort(preserve_progress=True)
            wrong_reference = ArtifactReference(
                logical_name=reference.logical_name,
                digest_algorithm="sha256",
                content_digest=hashlib.sha256(payload + b"x").hexdigest(),
                size_bytes=len(payload),
                format_version="artifact-manifest-v2",
                root_manifest_name=reference.root_manifest_name,
                publisher_identity=reference.publisher_identity,
                policy_epoch=reference.policy_epoch,
            )
            with self.assertRaisesRegex(ValueError, "identity"):
                AtomicArtifactDestination(
                    resumed_destination, wrong_reference,
                    "fetch-stage-1", max_range_bytes=8)
            resumed = AtomicArtifactDestination(
                resumed_destination, reference, "fetch-stage-1", max_range_bytes=8)
            missing = resumed.missing_ranges()
            self.assertEqual(sum(length for _offset, length in missing), len(payload) - 8)
            self.assertEqual(missing[0], (8, 8))
            resumed.write_range(16, payload[16:24])
            resumed.write_range(24, payload[24:32])
            resumed.write_range(32, payload[32:])
            resumed.write_range(8, payload[8:16])
            self.assertEqual(resumed.finalize().read_bytes(), payload)


if __name__ == "__main__":
    unittest.main()
