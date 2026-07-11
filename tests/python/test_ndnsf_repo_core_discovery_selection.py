#!/usr/bin/env python3
"""DistributedRepo ACK selection honors core readiness envelopes."""

from __future__ import annotations

import unittest
from unittest.mock import patch

from ndnsf import (
    AckCandidate,
    GenericProviderRuntimeHint,
    ProviderCapabilityHint,
    encode_ack_metadata,
)
from py_repoclient import (
    RepoCacheStatus,
    RepoDataReference,
    RepoObjectManifest,
    RepoOperationStatus,
    capability_from_ack,
    discovery_record_from_ack,
    parse_cache_status_json,
    parse_data_reference_json,
    parse_manifest_json,
    parse_operation_status_json,
    ready_capability_from_ack,
    repo_service_for_operation,
    repo_versioned_services,
)
from py_repoclient import _capacity_selector
from py_repoclient.orchestration import RepoNodeApp, encode_repo_request


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
    @staticmethod
    def _repo_app_validation_fixture() -> RepoNodeApp:
        app = RepoNodeApp.__new__(RepoNodeApp)
        app.service_name = "/NDNSF/DistributedRepo"
        app.provider_name = "/example/provider/repo-a"
        app.provider_prefix = "/example/provider"
        app.peer_provider_identities = frozenset()
        return app

    def test_repo_operations_have_versioned_public_or_internal_services(self) -> None:
        self.assertEqual(
            repo_service_for_operation("FETCH"),
            "/NDNSF/DistributedRepo/Object/v1/FETCH",
        )
        self.assertEqual(
            repo_service_for_operation("CATALOG_DELTA"),
            "/NDNSF/DistributedRepo/Internal/v1/CATALOG_DIGEST",
        )
        self.assertEqual(len(repo_versioned_services()), 13)

    def test_versioned_service_rejects_payload_operation_mismatch(self) -> None:
        app = self._repo_app_validation_fixture()
        with self.assertRaisesRegex(
                PermissionError, "repo-operation-service-mismatch"):
            app._validate_versioned_request(
                repo_service_for_operation("FETCH"),
                encode_repo_request("DELETE", objectName="/data/x"),
                "/example/user/alice",
            )

    def test_internal_service_rejects_missing_or_ordinary_client_identity(self) -> None:
        app = self._repo_app_validation_fixture()
        service = repo_service_for_operation("CATALOG_DELTA")
        payload = encode_repo_request("CATALOG_DELTA", sinceEpoch=0)
        with self.assertRaisesRegex(PermissionError, "repo-peer-identity-required"):
            app._validate_versioned_request(service, payload, "")
        with self.assertRaisesRegex(PermissionError, "repo-peer-identity-required"):
            app._validate_versioned_request(
                service, payload, "/example/user/alice")

    def test_internal_service_accepts_authenticated_provider_identity(self) -> None:
        app = self._repo_app_validation_fixture()
        request = app._validate_versioned_request(
            repo_service_for_operation("CATALOG_DELTA"),
            encode_repo_request("CATALOG_DELTA", sinceEpoch=0),
            "/example/provider/repo-b",
        )
        self.assertEqual(request["operation"], "CATALOG_DELTA")

    def test_manifest_binding_preserves_canonical_lifecycle_fields(self) -> None:
        manifest = RepoObjectManifest()
        manifest.object_name = "/data/model/v=1"
        manifest.packet_names = ["/data/model/v=1/seg=0"]
        manifest.generation = 4
        manifest.parent_generation = 3
        manifest.write_consistency = "QUORUM"
        manifest.required_write_acks = 2
        manifest.confirmed_replica_nodes = ["/repo/A", "/repo/B"]
        manifest.operation_id = "op-4"
        manifest.lifecycle_state = "COMMITTED"

        parsed = parse_manifest_json(manifest.to_json())
        self.assertEqual(parsed.packet_names, manifest.packet_names)
        self.assertEqual(parsed.generation, 4)
        self.assertEqual(parsed.parent_generation, 3)
        self.assertEqual(parsed.required_write_acks, 2)
        self.assertEqual(parsed.operation_id, "op-4")

    def test_reference_status_and_cache_bindings_round_trip(self) -> None:
        reference = RepoDataReference()
        reference.object_name = "/data/video/v=1"
        reference.data_prefix = "/data/video/v=1"
        reference.has_final_segment = True
        reference.final_segment = 8
        reference.expected_size = 4096
        parsed_reference = parse_data_reference_json(reference.to_json())
        self.assertEqual(parsed_reference.final_segment, 8)
        self.assertEqual(parsed_reference.expected_size, 4096)

        status = RepoOperationStatus()
        status.operation_id = "insert-1"
        status.operation = "INSERT"
        status.state = "DONE"
        status.completed_segments = 9
        status.total_segments = 9
        parsed_status = parse_operation_status_json(status.to_json())
        self.assertEqual(parsed_status.state, "DONE")
        self.assertEqual(parsed_status.completed_segments, 9)

        cache = RepoCacheStatus()
        cache.authoritative_backend = "sqlite"
        cache.cache_policy = "lru"
        cache.budget_bytes = 1024
        cache.hits = 3
        parsed_cache = parse_cache_status_json(cache.to_json())
        self.assertEqual(parsed_cache.authoritative_backend, "sqlite")
        self.assertEqual(parsed_cache.hits, 3)

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

    def test_legacy_ack_requires_explicit_mixed_reader(self) -> None:
        payload = (
            b"repoNode=/repo/legacy;freeBytes=2048;usedBytes=0;"
            b"availability=1;storageClasses=model;"
        )
        candidate = _candidate("/repo/legacy-provider", payload)

        with patch.dict("os.environ", {"NDNSF_ACK_COMPATIBILITY_MODE": "mixed"}):
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
