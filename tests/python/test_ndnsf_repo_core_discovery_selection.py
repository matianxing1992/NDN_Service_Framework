#!/usr/bin/env python3
"""DistributedRepo ACK selection honors core readiness envelopes."""

from __future__ import annotations

import unittest

from ndnsf import (
    AckCandidate,
    GenericProviderRuntimeHint,
    ProviderCapabilityHint,
    encode_ack_metadata,
)
from py_repoclient import (
    capability_from_ack,
    discovery_record_from_ack,
    ready_capability_from_ack,
)
from py_repoclient import _capacity_selector


def _candidate(provider: str, payload: bytes, *, status: bool = True) -> AckCandidate:
    return AckCandidate(
        provider,
        "/NDNSF/DistributedRepo",
        "req-1",
        status,
        "ack",
        payload,
    )


def _repo_hint_payload(provider: str, *, ready: bool = True, drain_state: str = "ACTIVE") -> bytes:
    hint = ProviderCapabilityHint(
        provider_name=provider,
        service_name="/NDNSF/DistributedRepo",
        ready=ready,
        drain_state=drain_state,
        runtime_hint=GenericProviderRuntimeHint(
            provider_name=provider,
            capacity_hints={"freeBytes": 4096},
        ),
        service_payload_schema="ndnsf-repo-capability-v1",
        service_payload={
            "repoNode": provider,
            "freeBytes": 4096,
            "usedBytes": 12,
            "availability": 0.95,
            "storageClasses": ["model"],
            "repoMode": "persistent",
        },
    )
    return encode_ack_metadata(hint.to_ack_fields())


class RepoCoreDiscoverySelectionTest(unittest.TestCase):
    def test_ready_typed_hint_yields_ready_capability(self) -> None:
        candidate = _candidate("/repo/A", _repo_hint_payload("/repo/A"))

        record = discovery_record_from_ack(candidate)
        capability = ready_capability_from_ack(candidate)

        self.assertTrue(record.ready_for_new_request())
        self.assertIsNotNone(capability)
        self.assertEqual(capability.repo_node, "/repo/A")
        self.assertEqual(capability.free_bytes, 4096)

    def test_draining_typed_hint_is_not_selectable_even_with_capacity(self) -> None:
        candidate = _candidate(
            "/repo/B",
            _repo_hint_payload("/repo/B", ready=True, drain_state="DRAINING"),
        )

        record = discovery_record_from_ack(candidate)

        self.assertFalse(record.ready_for_new_request())
        self.assertIsNotNone(capability_from_ack(candidate))
        self.assertIsNone(ready_capability_from_ack(candidate))

    def test_unready_typed_hint_is_not_selected(self) -> None:
        ready = _candidate("/repo/ready", _repo_hint_payload("/repo/ready"))
        unready = _candidate("/repo/unready", _repo_hint_payload("/repo/unready", ready=False))
        selector = _capacity_selector(replication_factor=2, object_size=128)

        selected = selector([unready, ready])

        self.assertEqual(selected, ["/repo/ready"])

    def test_legacy_ack_remains_ready_fallback(self) -> None:
        payload = (
            b"repoNode=/repo/legacy;freeBytes=2048;usedBytes=0;"
            b"availability=1;storageClasses=model;"
        )
        candidate = _candidate("/repo/legacy-provider", payload)

        record = discovery_record_from_ack(candidate)
        capability = ready_capability_from_ack(candidate)

        self.assertTrue(record.ready_for_new_request())
        self.assertIsNotNone(capability)
        self.assertEqual(capability.repo_node, "/repo/legacy")

    def test_all_unready_candidates_return_empty_selection(self) -> None:
        selector = _capacity_selector(replication_factor=1, object_size=128)
        selected = selector([
            _candidate("/repo/A", _repo_hint_payload("/repo/A", ready=False)),
            _candidate("/repo/B", _repo_hint_payload("/repo/B", drain_state="DRAINING")),
        ])

        self.assertEqual(selected, [])


if __name__ == "__main__":
    unittest.main()

