#!/usr/bin/env python3
"""Generic NDNSF admission metadata contract tests."""

from __future__ import annotations

import unittest

from ndnsf_distributed_inference.runtime_v1 import (
    AdmissionLeaseStatus,
    GenericAckMetadata,
    GenericAdmissionLease,
    GenericProviderRuntimeHint,
    PeerNetworkMetric,
    ProviderAdmissionLeaseTable,
    encode_ack_metadata,
    parse_ack_metadata,
)


class GenericAdmissionMetadataTest(unittest.TestCase):
    def test_generic_ack_metadata_round_trip(self) -> None:
        lease = GenericAdmissionLease(
            lease_id="lease-1",
            request_id="req-1",
            service_name="/Inference/NativeTracer",
            provider_name="/provider/A",
            expires_at_ms=4102444800000,
            resource_binding_schema="test-binding",
            resource_binding={"roleId": "/Stage/0"},
        )
        metadata = GenericAckMetadata(
            provider_runtime_hint=GenericProviderRuntimeHint(
                provider_name="/provider/A",
                queue_length=2,
                estimated_queue_wait_ms=15,
                peer_metrics=(PeerNetworkMetric(
                    src_peer="/provider/A",
                    dst_peer="/provider/B",
                    rtt_ms=12,
                    bandwidth_mbps=500,
                    loss_rate=0.01,
                ),),
            ),
            lease_offers=(lease,),
            service_payload_schema="app-test-v1",
            service_payload={"fragmentStates": []},
        )
        fields = parse_ack_metadata(encode_ack_metadata(metadata.to_ack_fields()))
        parsed = GenericAckMetadata.from_ack_fields(fields)
        self.assertEqual(parsed.provider_runtime_hint.provider_name, "/provider/A")
        self.assertEqual(parsed.provider_runtime_hint.queue_length, 2)
        self.assertEqual(parsed.provider_runtime_hint.peer_metrics[0].dst_peer, "/provider/B")
        self.assertEqual(parsed.lease_offers[0].lease_id, "lease-1")
        self.assertEqual(parsed.service_payload_schema, "app-test-v1")

    def test_lease_manager_grant_consume_release_and_rejects_reuse(self) -> None:
        table = ProviderAdmissionLeaseTable()
        lease = table.grant(GenericAdmissionLease(
            lease_id="lease-1",
            request_id="req-1",
            service_name="/S",
            provider_name="/P",
            expires_at_ms=2000,
            resource_binding={"roleId": "/R"},
        ))
        self.assertEqual(lease.status, AdmissionLeaseStatus.GRANTED)
        ok = table.consume(
            lease_id="lease-1",
            request_id="req-1",
            service_name="/S",
            provider_name="/P",
            resource_binding={"roleId": "/R"},
            now_ms_value=1000,
        )
        self.assertTrue(ok.status)
        self.assertEqual(ok.reason_code, "LEASE_CONSUMED")
        again = table.consume(
            lease_id="lease-1",
            request_id="req-1",
            service_name="/S",
            provider_name="/P",
            resource_binding={"roleId": "/R"},
            now_ms_value=1001,
        )
        self.assertFalse(again.status)
        self.assertEqual(again.reason_code, "LEASE_ALREADY_CONSUMED")
        self.assertEqual(table.counters()["consumed"], 1)

    def test_lease_validation_reports_expired_and_mismatched(self) -> None:
        expired_table = ProviderAdmissionLeaseTable()
        expired_table.grant(GenericAdmissionLease(
            lease_id="expired",
            request_id="req-1",
            service_name="/S",
            provider_name="/P",
            expires_at_ms=10,
            resource_binding={"roleId": "/R"},
        ))
        expired = expired_table.consume(
            lease_id="expired",
            request_id="req-1",
            service_name="/S",
            provider_name="/P",
            resource_binding={"roleId": "/R"},
            now_ms_value=11,
        )
        self.assertFalse(expired.status)
        self.assertEqual(expired.reason_code, "LEASE_EXPIRED")

        mismatch_table = ProviderAdmissionLeaseTable()
        mismatch_table.grant(GenericAdmissionLease(
            lease_id="mismatch",
            request_id="req-1",
            service_name="/S",
            provider_name="/P",
            expires_at_ms=2000,
            resource_binding={"roleId": "/R"},
        ))
        mismatch = mismatch_table.consume(
            lease_id="mismatch",
            request_id="req-1",
            service_name="/S",
            provider_name="/P",
            resource_binding={"roleId": "/Other"},
            now_ms_value=100,
        )
        self.assertFalse(mismatch.status)
        self.assertEqual(mismatch.reason_code, "LEASE_BINDING_MISMATCH")

    def test_peer_network_metric_is_directed(self) -> None:
        forward = PeerNetworkMetric.from_dict({
            "srcPeer": "/A",
            "dstPeer": "/B",
            "rttMs": 10,
            "bandwidthMbps": 1000,
        })
        reverse = PeerNetworkMetric.from_dict({
            "srcPeer": "/B",
            "dstPeer": "/A",
            "rttMs": 100,
            "bandwidthMbps": 10,
        })
        self.assertNotEqual(forward.src_peer, reverse.src_peer)
        self.assertEqual(forward.dst_peer, "/B")
        self.assertEqual(reverse.dst_peer, "/A")


if __name__ == "__main__":
    unittest.main()
