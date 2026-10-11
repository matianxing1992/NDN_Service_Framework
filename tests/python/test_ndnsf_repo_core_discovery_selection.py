#!/usr/bin/env python3
"""Native Repo value codecs and retired Python discovery boundaries."""

from __future__ import annotations

import unittest

from py_repoclient import (
    RepoCacheStatus,
    RepoCompatibilityError,
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
import py_repoclient.orchestration as legacy_repo


class RepoCoreDiscoverySelectionTest(unittest.TestCase):
    def test_repo_operations_have_versioned_public_or_internal_services(self) -> None:
        self.assertEqual(
            repo_service_for_operation("FETCH"),
            "/NDNSF/DistributedRepo/Object/v1/FETCH",
        )
        self.assertEqual(
            repo_service_for_operation("CATALOG_DELTA"),
            "/NDNSF/DistributedRepo/Internal/v1/CATALOG_DIGEST",
        )
        self.assertEqual(len(repo_versioned_services()), 11)
        for retired_operation in ("RESERVE_CAPACITY", "RELEASE_CAPACITY"):
            with self.assertRaisesRegex(ValueError, "unsupported repo operation"):
                repo_service_for_operation(retired_operation)

    def test_python_owned_request_codec_and_service_validation_are_retired(self) -> None:
        with self.assertRaises(RepoCompatibilityError) as codec_error:
            legacy_repo.encode_repo_request("FETCH")
        self.assertEqual(codec_error.exception.code, "SPEC192_COMPATIBILITY_V1")
        with self.assertRaises(RepoCompatibilityError) as server_error:
            legacy_repo.RepoNodeApp()
        self.assertEqual(server_error.exception.code, "SPEC192_COMPATIBILITY_V1")

    def test_native_manifest_status_reference_and_cache_codecs_round_trip(self) -> None:
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
        parsed_manifest = parse_manifest_json(manifest.to_json())
        self.assertEqual(parsed_manifest.packet_names, manifest.packet_names)
        self.assertEqual(parsed_manifest.generation, 4)
        self.assertEqual(parsed_manifest.parent_generation, 3)
        self.assertEqual(parsed_manifest.required_write_acks, 2)
        self.assertEqual(parsed_manifest.operation_id, "op-4")

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

    def test_python_owned_discovery_and_selection_imports_are_retired(self) -> None:
        for operation in (
            capability_from_ack,
            ready_capability_from_ack,
            discovery_record_from_ack,
        ):
            with self.subTest(operation=operation.__name__):
                with self.assertRaises(RepoCompatibilityError) as caught:
                    operation(object())
                self.assertEqual(caught.exception.code, "SPEC192_COMPATIBILITY_V1")


if __name__ == "__main__":
    unittest.main()
