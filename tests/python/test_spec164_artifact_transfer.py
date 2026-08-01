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

    def test_collaboration_adapter_uses_one_request_and_bound_selections(self):
        class FakeInvocation:
            request_id = "request-1"

            def __init__(self, *, accept=True, operation_id="operation-1"):
                self.commit_calls = []
                self.accept = accept
                self.operation_id = operation_id

            def acks_closed(self, _timeout_ms=None):
                return SimpleNamespace(
                    digest="sha256:" + "1" * 64,
                    candidates=(
                        SimpleNamespace(
                            provider_name="/repo/a", status=True,
                            payload=encode_upload_lease_ack(
                                make_lease("/repo/a", "a"))),
                        SimpleNamespace(
                            provider_name="/repo/b", status=True,
                            payload=encode_upload_lease_ack(
                                make_lease("/repo/b", "b"))),
                        SimpleNamespace(
                            provider_name="/repo/c", status=False,
                            payload=b""),
                    ),
                )

            def commit_plan(self, **kwargs):
                self.commit_calls.append(kwargs)
                return self.accept

        class FakeUser:
            def __init__(self):
                self.begin_calls = []
                self.invocation = FakeInvocation()

            def begin_collaboration(self, service, payload, **kwargs):
                self.begin_calls.append((service, payload, kwargs))
                return self.invocation

        user = FakeUser()
        client = ReplicaLeaseCollaborationClient(user, "/Repo/Artifact/Publish")
        pending = client.begin(
            make_reference(),
            requested_replicas=2,
            operation_id="operation-1",
            request_id="request-1",
        )
        self.assertTrue(pending.commit_leases(
            [make_lease("/repo/a", "a"), make_lease("/repo/b", "b")],
            now_ms=2000,
        ))
        self.assertEqual(len(user.begin_calls), 1)
        self.assertEqual(len(user.invocation.commit_calls), 1)
        commit = user.invocation.commit_calls[0]
        self.assertEqual(
            commit["role_provider_assignments"],
            {"artifact-replica-0": "/repo/a", "artifact-replica-1": "/repo/b"},
        )
        self.assertEqual(len(commit["assignment_payloads_by_role"]), 2)
        decoded = decode_upload_lease_assignment(
            commit["assignment_payloads_by_role"]["artifact-replica-0"],
            now_ms=2000,
        )
        self.assertEqual(decoded.repo_node, "/repo/a")
        self.assertEqual(decoded.artifact.content_digest, "ab" * 32)
        self.assertEqual(pending.snapshot().control_operation_count, 3)
        ack_leases = pending.leases_from_acks(now_ms=2000)
        self.assertEqual(
            [lease.repo_node for lease in ack_leases],
            ["/repo/a", "/repo/b"],
        )

        user2 = FakeUser()
        pending2 = ReplicaLeaseCollaborationClient(
            user2, "/Repo/Artifact/Publish"
        ).begin(
            make_reference(),
            requested_replicas=1,
            operation_id="operation-2",
        )
        with self.assertRaisesRegex(
                ValueError, "^repo-lease-selection-outside-ack:"):
            pending2.commit_leases(
                [make_lease("/repo/not-acked", "x")], now_ms=2000
            )

        user3 = FakeUser()
        user3.invocation = FakeInvocation(
            accept=False, operation_id="operation-3"
        )
        pending3 = ReplicaLeaseCollaborationClient(
            user3, "/Repo/Artifact/Publish"
        ).begin(
            make_reference(),
            requested_replicas=1,
            operation_id="operation-3",
        )
        self.assertFalse(pending3.commit_leases(
            [artifact_upload_lease_from_dict({
                **{
                    "leaseId": "lease-a",
                    "operationId": "operation-3",
                    "repoNode": "/repo/a",
                    "artifact": make_reference().to_dict(),
                    "reservedBytes": 8192,
                    "issuedAtMs": 1000,
                    "expiresAtMs": 10000,
                    "replayId": "replay-a",
                }
            }, 2000)], now_ms=2000
        ))
        self.assertEqual(
            pending3.snapshot().state,
            ReplicaLeaseControlState.ACK_CLOSED,
        )
        self.assertEqual(pending3.snapshot().control_operation_count, 1)


if __name__ == "__main__":
    unittest.main()
