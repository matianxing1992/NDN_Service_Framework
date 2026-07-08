#!/usr/bin/env python3
"""Tests for NDNSF core service-discovery snapshots."""

from __future__ import annotations

import time
import unittest

from ndnsf import (
    DRAIN_ACTIVE,
    DRAIN_DRAINING,
    DRAIN_MAINTENANCE,
    DRAIN_OFFLINE,
    GenericProviderRuntimeHint,
    NdnsdProviderState,
    ProviderCapabilityHint,
    ServiceDiscoveryRecord,
    ServiceDiscoverySnapshot,
    normalize_drain_state,
    now_ms,
    provider_ready_for_new_request,
)


class CoreServiceDiscoveryTest(unittest.TestCase):
    def test_provider_capability_hint_maps_to_ready_record(self) -> None:
        current = now_ms()
        hint = ProviderCapabilityHint(
            provider_name="/provider/A",
            service_name="/Inference/NativeTracer",
            ready=True,
            drain_state=DRAIN_ACTIVE,
            runtime_hint=GenericProviderRuntimeHint(
                provider_name="/provider/A",
                queue_length=1,
                capacity_hints={"freeGpuMemoryMb": 4096},
            ),
            service_payload_schema="ndnsf-di-runtime-ack-v1",
            service_payload={"fragment": "stage0"},
            timestamp_ms=current,
            expires_at_ms=current + 5000,
        )

        record = ServiceDiscoveryRecord.from_provider_capability_hint(hint)
        snapshot = ServiceDiscoverySnapshot.from_records("/Inference/NativeTracer", [hint])

        self.assertTrue(record.ready_for_new_request(now_ms_value=current))
        self.assertEqual(record.runtime_hint.queue_length, 1)
        self.assertEqual(record.metadata["fragment"], "stage0")
        self.assertEqual(snapshot.provider_names, ("/provider/A",))
        self.assertEqual(len(snapshot.ready_records(now_ms_value=current)), 1)
        self.assertTrue(provider_ready_for_new_request(hint, now_ms_value=current))

    def test_draining_capability_hint_is_not_ready(self) -> None:
        hint = ProviderCapabilityHint(
            provider_name="/provider/B",
            service_name="/HELLO",
            ready=True,
            drain_state=DRAIN_DRAINING,
            reason_code="DRAINING",
            message="finishing active calls",
        )

        record = ServiceDiscoveryRecord.from_provider_capability_hint(hint)
        snapshot = ServiceDiscoverySnapshot.from_records("/HELLO", [record])

        self.assertFalse(record.ready_for_new_request())
        self.assertEqual(record.drain_state, DRAIN_DRAINING)
        self.assertEqual(len(snapshot.ready_records()), 0)
        self.assertEqual(len(snapshot.draining_records()), 1)

    def test_ndnsd_provider_state_maps_runtime_status_to_drain_state(self) -> None:
        current_s = int(time.time())
        state = NdnsdProviderState(
            provider="/provider/C",
            service_name="/Video/Stream",
            service_lifetime_s=30,
            publish_timestamp_s=current_s,
            meta={"runtimeStatus": "provisioning"},
        )

        record = ServiceDiscoveryRecord.from_ndnsd_provider_state(state)

        self.assertFalse(record.ready_for_new_request())
        self.assertEqual(record.drain_state, "PROVISIONING")
        self.assertEqual(record.reason_code, "PROVISIONING")

    def test_raw_dict_input_can_represent_stale_and_maintenance_records(self) -> None:
        current = 10_000
        stale = {
            "providerName": "/provider/stale",
            "serviceName": "/HELLO",
            "ready": True,
            "lastSeenMs": 1_000,
            "freshnessMs": 100,
        }
        maintenance = {
            "provider": "/provider/maint",
            "serviceName": "/HELLO",
            "ready": True,
            "drainState": DRAIN_MAINTENANCE,
        }

        snapshot = ServiceDiscoverySnapshot.from_records("/HELLO", [stale, maintenance])

        self.assertEqual(normalize_drain_state("down"), DRAIN_OFFLINE)
        self.assertEqual(len(snapshot.ready_records(now_ms_value=current)), 0)
        self.assertEqual(len(snapshot.stale_records(now_ms_value=current)), 1)
        self.assertEqual(len(snapshot.unavailable_records(now_ms_value=current)), 2)
        self.assertFalse(provider_ready_for_new_request(maintenance, now_ms_value=current))


if __name__ == "__main__":
    unittest.main()

