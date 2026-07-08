#!/usr/bin/env python3
"""Core/app boundary envelope tests."""

from __future__ import annotations

import unittest

from ndnsf import (
    GenericAdmissionLease,
    GenericProviderRuntimeHint,
    PeerNetworkMetric,
    ProviderCapabilityHint,
    ProviderNetworkMatrix,
    RejectionReason,
    ServiceOperationState,
    ServiceOperationStatus,
    StreamAdaptiveFetcherState,
    StreamHealth,
    StreamHealthState,
    StreamInfo,
    StreamMetrics,
    encode_ack_metadata,
    is_recommended_rejection_reason,
    parse_ack_metadata,
)


class CoreBoundaryEnvelopeTest(unittest.TestCase):
    def test_service_operation_status_round_trip_and_terminal_state(self) -> None:
        status = ServiceOperationStatus(
            operation_id="op-1",
            operation="STORE",
            service_name="/NDNSF/DistributedRepo/STORE",
            provider_name="/repo/A",
            state=ServiceOperationState.RUNNING,
            progress=0.5,
            result_reference={"objectName": "/objects/x"},
        )

        parsed = ServiceOperationStatus.from_dict({
            "operationId": status.operation_id,
            "operation": status.operation,
            "serviceName": status.service_name,
            "providerName": status.provider_name,
            "state": "DONE",
            "progress": 1.0,
            "resultReference": status.result_reference,
        })

        self.assertFalse(status.terminal)
        self.assertTrue(parsed.terminal)
        self.assertEqual(parsed.result_reference["objectName"], "/objects/x")

    def test_provider_capability_hint_round_trip_keeps_core_and_app_payloads(self) -> None:
        hint = ProviderCapabilityHint(
            provider_name="/provider/A",
            service_name="/Inference/NativeTracer",
            ready=False,
            reason_code=RejectionReason.QUEUE_FULL.value,
            runtime_hint=GenericProviderRuntimeHint(
                provider_name="/provider/A",
                queue_length=4,
                estimated_queue_wait_ms=30,
            ),
            lease_offers=(GenericAdmissionLease(
                lease_id="lease-1",
                request_id="req-1",
                service_name="/Inference/NativeTracer",
                provider_name="/provider/A",
            ),),
            service_payload_schema="ndnsf-di-runtime-v1",
            service_payload={"fragmentResidency": "GPU_LOADED"},
        )

        fields = parse_ack_metadata(encode_ack_metadata(hint.to_ack_fields()))
        parsed = ProviderCapabilityHint.from_ack_fields(fields)

        self.assertFalse(parsed.ready)
        self.assertEqual(parsed.reason_code, "QUEUE_FULL")
        self.assertEqual(parsed.runtime_hint.queue_length, 4)
        self.assertEqual(parsed.lease_offers[0].lease_id, "lease-1")
        self.assertEqual(parsed.service_payload["fragmentResidency"], "GPU_LOADED")
        self.assertTrue(is_recommended_rejection_reason(parsed.reason_code))

    def test_provider_network_matrix_ranks_transfer_targets(self) -> None:
        matrix = ProviderNetworkMatrix((
            PeerNetworkMetric("/A", "/B", rtt_ms=5, bandwidth_mbps=1000, loss_rate=0.0),
            PeerNetworkMetric("/A", "/C", rtt_ms=50, bandwidth_mbps=10, loss_rate=0.1),
        ))

        ranked = matrix.rank_transfer_candidates("/A", ["/C", "/B"], 1024 * 1024)
        best = matrix.best_transfer_target("/A", ["/C", "/B"], 1024 * 1024)

        self.assertEqual(ranked[0]["dstPeer"], "/B")
        self.assertEqual(best["dstPeer"], "/B")
        self.assertLess(ranked[0]["totalMs"], ranked[1]["totalMs"])

    def test_stream_health_reports_degraded_congested_and_stale_without_app_video_policy(self) -> None:
        info = StreamInfo("stream-1", 2, "/uav/video/stream-1")
        metrics = StreamMetrics(gaps=1)
        degraded = StreamHealth.from_stream(info, metrics, now_ms_value=1000)
        self.assertEqual(degraded.state, StreamHealthState.DEGRADED)

        fetcher = StreamAdaptiveFetcherState()
        fetcher.set_backlog_pressure(0.9)
        congested = StreamHealth.from_stream(
            info,
            StreamMetrics(),
            fetch_decision=fetcher.decide(),
            now_ms_value=1000,
        )
        stale = StreamHealth.from_stream(
            info,
            StreamMetrics(),
            last_chunk_ms=1,
            stale_after_ms=100,
            now_ms_value=1000,
        )

        self.assertEqual(congested.state, StreamHealthState.CONGESTED)
        self.assertEqual(stale.state, StreamHealthState.STALE)
        self.assertIn("metrics", degraded.to_dict())


if __name__ == "__main__":
    unittest.main()
