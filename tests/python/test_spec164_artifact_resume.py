#!/usr/bin/env python3
"""Spec 164 T009 exact-identity resumable session tests."""

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from py_repoclient import (
    ArtifactReplicaSession,
    AtomicArtifactDestination,
    HmacReceiptAuthenticator,
    LifecycleTransitionError,
    SqliteRepositoryPersistence,
    artifact_chunk_from_dict,
    artifact_manifest_page_from_dict,
    artifact_root_manifest_from_dict,
    artifact_sha256_hex,
    artifact_upload_lease_from_dict,
    canonical_manifest_page_bytes,
)
from test_spec164_artifact_manifest import ManifestFixture


class ArtifactResumeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = ManifestFixture()
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.database = self.root / "repo.sqlite3"
        self.persistence = self._open_persistence()
        self.authenticator = HmacReceiptAuthenticator(
            "/repo/1", "/repo/1/KEY/resume", b"S" * 32
        )
        self.chunks, self.page, self.signed_root = self._two_chunk_manifest()

    def tearDown(self) -> None:
        self.persistence.close()
        self.temporary.cleanup()
        self.fixture.close()

    def _open_persistence(self) -> SqliteRepositoryPersistence:
        return SqliteRepositoryPersistence(
            self.database, "spec164-t009-test"
        )

    def _two_chunk_manifest(self):
        fixture = self.fixture
        chunks = []
        children = []
        for index, offset in enumerate((0, 4)):
            payload = fixture.payload[offset:offset + 4]
            digest = artifact_sha256_hex(payload)
            chunk = artifact_chunk_from_dict({
                "index": index,
                "offsetBytes": offset,
                "lengthBytes": len(payload),
                "digestAlgorithm": "sha256",
                "digest": digest,
                "firstSegment": 0,
                "finalSegment": 0,
            }, fixture.artifact, fixture.limits)
            chunks.append(chunk)
            children.append({
                "kind": "chunk",
                "index": index,
                "offsetBytes": offset,
                "lengthBytes": len(payload),
                "digestAlgorithm": "sha256",
                "digest": digest,
            })
        page_dict = {
            "pageVersion": "artifact-manifest-page-v2",
            "depth": 0,
            "offsetBytes": 0,
            "lengthBytes": len(fixture.payload),
            "pageDigestAlgorithm": "sha256",
            "pageDigest": "0" * 64,
            "children": children,
        }
        placeholder = artifact_manifest_page_from_dict(
            page_dict, 512, fixture.limits
        )
        page_dict["pageDigest"] = artifact_sha256_hex(
            canonical_manifest_page_bytes(placeholder, fixture.limits)
        )
        page = artifact_manifest_page_from_dict(
            page_dict, 512, fixture.limits
        )
        root_dict = dict(fixture.root_dict)
        root_dict["chunkBytes"] = 4
        root_dict["manifestRootDigest"] = page_dict["pageDigest"]
        root = artifact_root_manifest_from_dict(
            root_dict, 512, fixture.limits
        )
        return chunks, page, fixture.sign(root)

    def _lease(
        self,
        operation_id: str,
        suffix: str,
        *,
        issued_at_ms: int = 1000,
        expires_at_ms: int = 10000,
        validation_time_ms: int = 2000,
    ):
        return artifact_upload_lease_from_dict({
            "leaseId": f"lease-{suffix}",
            "operationId": operation_id,
            "repoNode": "/repo/1",
            "artifact": self.fixture.artifact_dict,
            "reservedBytes": len(self.fixture.payload),
            "issuedAtMs": issued_at_ms,
            "expiresAtMs": expires_at_ms,
            "replayId": f"replay-{suffix}",
        }, validation_time_ms)

    def _session(self, operation_id: str, lease, now_ms: int):
        return ArtifactReplicaSession(
            persistence=self.persistence,
            operation_id=operation_id,
            repo_node="/repo/1",
            generation=1,
            upload_lease=lease,
            lease_validation_time_ms=now_ms,
            artifact=self.fixture.artifact,
            signed_root=self.signed_root,
            pages=[self.page],
            chunks=self.chunks,
            capability=self.fixture.capability,
            trust_policy=self.fixture.policy,
            receipt_authenticator=self.authenticator,
            limits=self.fixture.limits,
        )

    def test_restart_transfers_only_missing_chunk_with_monotonic_progress(self):
        operation_id = "resume-after-interrupt"
        lease = self._lease(operation_id, "initial")
        first = self._session(operation_id, lease, 2000)
        first.reserve(2001)
        self.assertTrue(first.receive_chunk(
            0, self.fixture.payload[:4], now_ms=2002
        ))
        before = self.persistence.transfer_session(operation_id)
        self.assertEqual(before.verified_chunks, 1)
        self.persistence.close()
        self.persistence = self._open_persistence()

        resumed = self._session(operation_id, lease, 2500)
        self.assertEqual(resumed.missing_chunks(2501), (1,))
        self.assertFalse(resumed.receive_chunk(
            0, self.fixture.payload[:4], now_ms=2502
        ))
        self.assertTrue(resumed.receive_chunk(
            1, self.fixture.payload[4:], now_ms=2503
        ))
        resumed.verify_complete(2504)
        after = self.persistence.transfer_session(operation_id)
        self.assertEqual(after.state, "COMPLETED")
        self.assertEqual(after.verified_chunks, 2)
        self.assertEqual(after.newly_verified_bytes, len(self.fixture.payload))
        self.assertEqual(after.avoided_retransmission_bytes, 4)
        self.assertGreaterEqual(after.updated_at_ms, before.updated_at_ms)

    def test_expiry_renewal_and_cancellation_are_lease_bound(self):
        operation_id = "lease-resume"
        session = self._session(
            operation_id,
            self._lease(
                operation_id, "short", expires_at_ms=3000
            ),
            2000,
        )
        session.reserve(2001)
        renewed = self._lease(
            operation_id,
            "renewed",
            issued_at_ms=2500,
            expires_at_ms=6000,
            validation_time_ms=2501,
        )
        session.renew_lease(renewed, 2501)
        self.assertFalse(session.expire(3000))
        self.assertTrue(session.expire(6000))
        second = self._lease(
            operation_id,
            "second",
            issued_at_ms=6000,
            expires_at_ms=8000,
            validation_time_ms=6001,
        )
        session.resume(second, 6001)
        self.assertEqual(session.missing_chunks(6002), (0, 1))
        session.cancel(preserve_progress=True, now_ms=6003)
        third = self._lease(
            operation_id,
            "third",
            issued_at_ms=7000,
            expires_at_ms=9000,
            validation_time_ms=7001,
        )
        session.resume(third, 7001)
        session.cancel(preserve_progress=False, now_ms=7002)
        record = self.persistence.transfer_session(operation_id)
        self.assertEqual(record.state, "FAILED")
        self.assertFalse(record.preserves_progress)
        with self.assertRaisesRegex(LifecycleTransitionError, "cannot resume"):
            self._session(
                operation_id,
                self._lease(
                    operation_id,
                    "fourth",
                    issued_at_ms=9000,
                    expires_at_ms=12000,
                    validation_time_ms=9001,
                ),
                9001,
            )

    def test_atomic_destination_restart_uses_exact_identity_and_missing_ranges(self):
        destination = self.root / "consumer" / "artifact.bin"
        sink = AtomicArtifactDestination(
            destination,
            self.fixture.artifact,
            "retrieval-resume",
            max_range_bytes=4,
        )
        sink.write_range(0, self.fixture.payload[:4])
        self.assertEqual(sink.missing_ranges(), ((4, 4),))

        resumed = AtomicArtifactDestination(
            destination,
            self.fixture.artifact,
            "retrieval-resume",
            max_range_bytes=4,
        )
        self.assertEqual(resumed.missing_ranges(), ((4, 4),))
        resumed.write_range(4, self.fixture.payload[4:])
        self.assertEqual(resumed.finalize(), destination)
        self.assertEqual(destination.read_bytes(), self.fixture.payload)
        self.assertFalse(resumed.sidecar.exists())

    def test_atomic_destination_rejects_mixed_resume_and_supports_cancel(self):
        destination = self.root / "mixed.bin"
        sink = AtomicArtifactDestination(
            destination,
            self.fixture.artifact,
            "retrieval-mixed",
            max_range_bytes=4,
        )
        sink.write_range(0, self.fixture.payload[:4])
        sink.write_range(0, self.fixture.payload[:4])
        with self.assertRaisesRegex(ValueError, "verified-range-conflict"):
            sink.write_range(0, b"WXYZ")
        changed = dict(self.fixture.artifact_dict)
        changed["policyEpoch"] = "epoch-other"
        from py_repoclient import artifact_reference_from_dict
        other = artifact_reference_from_dict(changed, self.fixture.limits)
        with self.assertRaisesRegex(ValueError, "resume-identity-conflict"):
            AtomicArtifactDestination(
                destination, other, "retrieval-mixed", max_range_bytes=4
            )
        sink.cancel(preserve_progress=True)
        self.assertTrue(sink.temporary.exists())
        sink.cancel(preserve_progress=False)
        self.assertFalse(sink.temporary.exists())
        self.assertFalse(sink.sidecar.exists())

    def test_persistence_rejects_changed_root_identity(self):
        operation_id = "identity-conflict"
        session = self._session(
            operation_id, self._lease(operation_id, "identity"), 2000
        )
        record = self.persistence.transfer_session(operation_id)
        changed = dict(record.identity)
        changed["manifestRootDigest"] = "f" * 64
        with self.assertRaisesRegex(
            LifecycleTransitionError, "session-conflict"
        ):
            self.persistence.save_transfer_session(
                operation_id=operation_id,
                artifact_digest=record.artifact_digest,
                generation=record.generation,
                identity=changed,
                lease=record.lease,
                state=record.state,
                preserves_progress=record.preserves_progress,
                verified_chunks=record.verified_chunks,
                newly_verified_bytes=record.newly_verified_bytes,
                avoided_retransmission_bytes=(
                    record.avoided_retransmission_bytes
                ),
                updated_at_ms=record.updated_at_ms + 1,
            )
        self.assertEqual(session.missing_chunks(2001), (0, 1))


if __name__ == "__main__":
    unittest.main()
