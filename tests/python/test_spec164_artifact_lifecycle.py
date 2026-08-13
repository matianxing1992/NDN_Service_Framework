#!/usr/bin/env python3
"""Spec 164 T008 trusted single-replica lifecycle integration tests."""

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from ndnsf import make_segmented_data_packets
from py_repoclient import (
    AdaptiveArtifactTransfer,
    AdaptiveTransferOptions,
    AuthenticatedReplicaReceipt,
    ArtifactReplicaSession,
    AtomicArtifactDestination,
    HmacReceiptAuthenticator,
    SqliteRepositoryPersistence,
    artifact_upload_lease_from_dict,
    resolve_active_artifact,
)
from test_spec164_artifact_manifest import ManifestFixture


class ArtifactLifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = ManifestFixture()
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.persistence = SqliteRepositoryPersistence(
            self.root / "repo.sqlite3", "spec164-t008-test"
        )
        self.authenticator = HmacReceiptAuthenticator(
            "/repo/1", "/repo/1/KEY/receipt-1", b"R" * 32
        )

    def tearDown(self) -> None:
        self.persistence.close()
        self.temporary.cleanup()
        self.fixture.close()

    def session(self, operation_id: str = "operation-success"):
        fixture = self.fixture
        lease = artifact_upload_lease_from_dict({
            "leaseId": f"lease-{operation_id}",
            "operationId": operation_id,
            "repoNode": "/repo/1",
            "artifact": fixture.artifact_dict,
            "reservedBytes": len(fixture.payload),
            "issuedAtMs": 1000,
            "expiresAtMs": 10000,
            "replayId": f"replay-{operation_id}",
        }, 2000)
        return ArtifactReplicaSession(
            persistence=self.persistence,
            operation_id=operation_id,
            repo_node="/repo/1",
            generation=1,
            upload_lease=lease,
            lease_validation_time_ms=2000,
            artifact=fixture.artifact,
            signed_root=fixture.signed_root,
            pages=[fixture.page],
            chunks=[fixture.chunk],
            capability=fixture.capability,
            trust_policy=fixture.policy,
            receipt_authenticator=self.authenticator,
            limits=fixture.limits,
        )

    def test_full_commit_receipt_activation_and_atomic_retrieval(self) -> None:
        session = self.session()
        with self.assertRaisesRegex(LookupError, "repo-artifact-not-active"):
            resolve_active_artifact(
                self.persistence,
                self.fixture.artifact.logical_name,
                self.fixture.artifact.policy_epoch,
            )

        session.reserve(2001)
        packets = make_segmented_data_packets(
            "/publisher/artifact/chunk/0",
            self.fixture.payload,
            max_segment_size=4,
        )
        options = AdaptiveTransferOptions()
        options.initial_window = 2
        options.maximum_window = 2
        options.verification_backlog_limit = 2
        transfer = AdaptiveArtifactTransfer(len(packets), options)
        self.assertEqual(
            [request.segment_no for request in transfer.poll(2001)], [0, 1]
        )
        received = {}
        for packet in reversed(packets):
            transfer.receive(
                packet.segment,
                len(packet.content),
                len(packet.wire),
                2002,
            )
            received[packet.segment] = packet.content
        session.receive_chunk(
            0,
            b"".join(received[index] for index in sorted(received)),
            now_ms=2002,
        )
        for packet in packets:
            transfer.mark_verified(packet.segment)
        self.assertTrue(transfer.snapshot().complete)
        session.verify_complete(2003)
        envelope = session.commit_and_activate(2004)
        self.assertEqual(session.state, "ACTIVE")
        receipt = self.authenticator.verify(
            envelope,
            expected_artifact=self.fixture.artifact,
            expected_operation_id="operation-success",
        )
        self.assertEqual(receipt.state, "COMMITTED")
        self.assertTrue(session.payload_store.is_committed(session.identity))
        active = resolve_active_artifact(
            self.persistence,
            self.fixture.artifact.logical_name,
            self.fixture.artifact.policy_epoch,
        )
        self.assertEqual(active.content_digest, self.fixture.artifact.content_digest)
        self.assertEqual(
            [event.to_state for event in
             self.persistence.lifecycle_events("operation-success")
             if event.accepted],
            ["RESERVED", "RECEIVING", "VERIFIED", "COMMITTED", "ACTIVE"],
        )

        destination = self.root / "consumer" / "artifact.bin"
        sink = AtomicArtifactDestination(
            destination, active, "retrieval-success", max_range_bytes=4
        )
        sink.write_range(4, self.fixture.payload[4:])
        self.assertFalse(destination.exists())
        sink.write_range(0, self.fixture.payload[:4])
        self.assertEqual(sink.finalize(), destination)
        self.assertEqual(destination.read_bytes(), self.fixture.payload)

        repeated = session.commit_and_activate(9999)
        self.assertEqual(repeated.to_bytes(), envelope.to_bytes())

    def test_selected_task_queues_without_capacity_reservation_or_lock(self) -> None:
        session = self.session("operation-queued")
        before = self.persistence.capacity_status()
        session.begin_assigned_task(2001)
        after = self.persistence.capacity_status()
        self.assertEqual(session.state, "QUEUED")
        self.assertEqual(after.reserved_bytes, before.reserved_bytes)
        self.assertEqual(
            [event.to_state for event in
             self.persistence.lifecycle_events("operation-queued")
             if event.accepted],
            ["QUEUED"],
        )
        session.receive_chunk(0, self.fixture.payload, now_ms=2002)
        session.verify_complete(2003)
        session.commit_and_activate(2004)
        self.assertEqual(session.state, "ACTIVE")
        self.assertEqual(
            self.persistence.capacity_status().reserved_bytes,
            before.reserved_bytes,
        )

    def test_corruption_never_commits_activates_or_exposes_destination(self) -> None:
        session = self.session("operation-corrupt")
        session.reserve(2101)
        corrupt = self.fixture.payload[:-1] + b"!"
        with self.assertRaisesRegex(Exception, "digest-mismatch"):
            session.receive_chunk(0, corrupt, now_ms=2102)
        session.fail("injected chunk corruption", 2103)
        self.assertEqual(session.state, "FAILED")
        self.assertFalse(session.payload_store.is_committed(session.identity))
        with self.assertRaisesRegex(LookupError, "repo-artifact-not-active"):
            resolve_active_artifact(
                self.persistence,
                self.fixture.artifact.logical_name,
                self.fixture.artifact.policy_epoch,
            )

        destination = self.root / "consumer-corrupt.bin"
        sink = AtomicArtifactDestination(
            destination,
            self.fixture.artifact,
            "retrieval-corrupt",
            max_range_bytes=8,
        )
        sink.write_range(0, corrupt)
        with self.assertRaisesRegex(ValueError, "digest-mismatch"):
            sink.finalize()
        self.assertFalse(destination.exists())
        self.assertFalse(sink.temporary.exists())

    def test_atomic_destination_batches_and_forces_resume_checkpoint(self) -> None:
        destination = self.root / "consumer-batched.bin"
        sink = AtomicArtifactDestination(
            destination,
            self.fixture.artifact,
            "retrieval-batched",
            max_range_bytes=len(self.fixture.payload),
            checkpoint_bytes=len(self.fixture.payload),
        )
        sink.write_range(0, self.fixture.payload[:4])
        # Below the configured checkpoint threshold, the sidecar remains at
        # its initial empty coverage; this is the high-throughput path.
        self.assertEqual(json.loads(sink.sidecar.read_text())['ranges'], [])
        sink.abort(preserve_progress=True)

        resumed = AtomicArtifactDestination(
            destination,
            self.fixture.artifact,
            "retrieval-batched",
            max_range_bytes=len(self.fixture.payload),
            checkpoint_bytes=len(self.fixture.payload),
        )
        self.assertEqual(resumed.missing_ranges(), ((4, len(self.fixture.payload) - 4),))
        resumed.write_range(4, self.fixture.payload[4:])
        self.assertEqual(resumed.finalize(), destination)
        self.assertEqual(destination.read_bytes(), self.fixture.payload)

    def test_receipt_tamper_and_wrong_operation_fail_authentication(self) -> None:
        session = self.session()
        session.reserve(2201)
        session.receive_chunk(0, self.fixture.payload, now_ms=2202)
        session.verify_complete(2203)
        envelope = session.commit_and_activate(2204)
        value = envelope.to_dict()
        value["signatureHex"] = "00" * 32
        tampered = AuthenticatedReplicaReceipt.from_dict(value)
        with self.assertRaisesRegex(ValueError, "signature mismatch"):
            self.authenticator.verify(tampered)
        with self.assertRaisesRegex(ValueError, "operation mismatch"):
            self.authenticator.verify(
                envelope, expected_operation_id="different-operation"
            )

    def test_verified_journal_recovers_commit_after_payload_finalize(self) -> None:
        session = self.session("operation-recover")
        session.reserve(2301)
        session.receive_chunk(0, self.fixture.payload, now_ms=2302)
        session.verify_complete(2303)
        session.payload_store.finalize(session.identity)
        self.assertEqual(session.state, "VERIFIED")
        envelope = session.commit_and_activate(2304)
        self.authenticator.verify(
            envelope, expected_operation_id="operation-recover"
        )
        self.assertEqual(session.state, "ACTIVE")

    def test_session_revalidates_selected_lease_at_data_plane_entry(self) -> None:
        fixture = self.fixture
        lease = artifact_upload_lease_from_dict({
            "leaseId": "lease-expiring",
            "operationId": "operation-expiring",
            "repoNode": "/repo/1",
            "artifact": fixture.artifact_dict,
            "reservedBytes": len(fixture.payload),
            "issuedAtMs": 1000,
            "expiresAtMs": 3000,
            "replayId": "replay-expiring",
        }, 2000)
        with self.assertRaisesRegex(Exception, "artifact-invalid-lease"):
            ArtifactReplicaSession(
                persistence=self.persistence,
                operation_id="operation-expiring",
                repo_node="/repo/1",
                generation=1,
                upload_lease=lease,
                lease_validation_time_ms=3000,
                artifact=fixture.artifact,
                signed_root=fixture.signed_root,
                pages=[fixture.page],
                chunks=[fixture.chunk],
                capability=fixture.capability,
                trust_policy=fixture.policy,
                receipt_authenticator=self.authenticator,
                limits=fixture.limits,
            )


if __name__ == "__main__":
    unittest.main()
