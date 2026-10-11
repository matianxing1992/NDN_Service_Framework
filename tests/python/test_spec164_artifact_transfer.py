#!/usr/bin/env python3
"""Spec 164 T007 adaptive transfer and collaboration-control tests."""

from __future__ import annotations

import unittest
from types import SimpleNamespace

import ndnsf
from py_repoclient import (
    AdaptiveArtifactTransfer,
    AdaptiveTransferOptions,
    ArtifactReference,
    ArtifactSegmentDisposition,
    ReplicaLeaseControlFlow,
    ReplicaLeaseCollaborationClient,
    ReplicaLeaseControlState,
    artifact_reference_from_dict,
    artifact_upload_lease_from_dict,
    decode_upload_lease_assignment,
    encode_upload_lease_ack,
)


def make_reference() -> ArtifactReference:
    return artifact_reference_from_dict({
        "logicalName": "/artifact/spec164/t007",
        "digestAlgorithm": "sha256",
        "contentDigest": "ab" * 32,
        "sizeBytes": 8192,
        "formatVersion": "artifact-manifest-v2",
        "rootManifestName": "/publisher/manifests/spec164/t007",
        "publisherIdentity": "/publisher",
        "policyEpoch": "policy-epoch-1",
    })


def make_lease(provider: str, suffix: str):
    return artifact_upload_lease_from_dict({
        "leaseId": f"lease-{suffix}",
        "operationId": "operation-1",
        "repoNode": provider,
        "artifact": {
            "logicalName": "/artifact/spec164/t007",
            "digestAlgorithm": "sha256",
            "contentDigest": "ab" * 32,
            "sizeBytes": 8192,
            "formatVersion": "artifact-manifest-v2",
            "rootManifestName": "/publisher/manifests/spec164/t007",
            "publisherIdentity": "/publisher",
            "policyEpoch": "policy-epoch-1",
        },
        "reservedBytes": 8192,
        "issuedAtMs": 1000,
        "expiresAtMs": 10000,
        "replayId": f"replay-{suffix}",
    }, 2000)


class ArtifactTransferTests(unittest.TestCase):
    def test_scheduler_reorders_suppresses_duplicates_and_backpressures(self):
        options = AdaptiveTransferOptions()
        options.initial_window = 4
        options.maximum_window = 8
        options.verification_backlog_limit = 2
        transfer = AdaptiveArtifactTransfer(3, options)

        first = transfer.poll(1000)
        self.assertEqual([item.segment_no for item in first], [0, 1])
        self.assertEqual(
            transfer.receive(1, 100, 120, 1001),
            ArtifactSegmentDisposition.ACCEPTED,
        )
        self.assertEqual(
            transfer.receive(0, 100, 120, 1002),
            ArtifactSegmentDisposition.ACCEPTED,
        )
        self.assertEqual(
            transfer.receive(1, 100, 120, 1003),
            ArtifactSegmentDisposition.DUPLICATE,
        )
        self.assertEqual(transfer.poll(1004), [])
        transfer.mark_verified(0)
        second = transfer.poll(1005)
        self.assertEqual([item.segment_no for item in second], [2])
        transfer.mark_verified(1)
        transfer.receive(2, 100, 120, 1006)
        transfer.mark_verified(2)
        snapshot = transfer.snapshot()
        self.assertTrue(snapshot.complete)
        self.assertEqual(snapshot.duplicate_count, 1)
        self.assertEqual(snapshot.interest_count, 3)

    def test_timeout_is_retransmitted_and_counted(self):
        options = AdaptiveTransferOptions()
        options.initial_window = 1
        options.maximum_window = 2
        options.verification_backlog_limit = 2
        options.maximum_retries = 1
        options.segment_timeout_ms = 10
        transfer = AdaptiveArtifactTransfer(1, options)
        transfer.poll(0)
        transfer.expire(10)
        retry = transfer.poll(11)
        self.assertEqual(len(retry), 1)
        self.assertTrue(retry[0].retransmission)
        transfer.receive(0, 4096, 4200, 12)
        transfer.mark_verified(0)
        snapshot = transfer.snapshot()
        self.assertTrue(snapshot.complete)
        self.assertEqual(snapshot.timeout_count, 1)
        self.assertEqual(snapshot.retransmission_count, 1)
        self.assertEqual(snapshot.retransmitted_bytes, 4200)

    def test_control_count_is_independent_of_segment_count(self):
        leases = [make_lease("/repo/a", "a"), make_lease("/repo/b", "b")]
        counts = []
        for request_id in ("small-artifact", "large-artifact"):
            flow = ReplicaLeaseControlFlow()
            flow.begin_collaboration(request_id)
            flow.close_acks(3)
            flow.commit_plan(leases, 2000)
            snapshot = flow.snapshot()
            self.assertEqual(snapshot.state, ReplicaLeaseControlState.PLAN_COMMITTED)
            self.assertEqual(snapshot.selected_replica_count, 2)
            counts.append(snapshot.control_operation_count)
        self.assertEqual(counts, [3, 3])

    def test_live_fetch_binding_rejects_unbounded_options_before_network(self):
        with self.assertRaisesRegex(
                ValueError, "adaptive segmented fetch requires bounded"):
            ndnsf.fetch_adaptive_segmented_data_packets(
                "/not/contacted",
                lambda _packet: None,
                initial_window=0,
            )

    def test_retired_collaboration_adapter_cannot_start_a_request(self):
        from py_repoclient import RepoCompatibilityError

        class User:
            def __init__(self):
                self.requests = []

            def begin_collaboration(self, *args, **kwargs):
                self.requests.append((args, kwargs))
                raise AssertionError("retired adapter emitted a collaboration")

        user = User()
        with self.assertRaises(RepoCompatibilityError) as error:
            ReplicaLeaseCollaborationClient(user, "/Repo/Artifact/Publish")
        self.assertEqual(error.exception.code, "SPEC192_COMPATIBILITY_V1")
        self.assertEqual(error.exception.operation, "ReplicaLeaseCollaborationClient")
        self.assertEqual(error.exception.replacement, "use ArtifactRepositoryApi.make_persistent")
        self.assertEqual(user.requests, [])

if __name__ == "__main__":
    unittest.main()
