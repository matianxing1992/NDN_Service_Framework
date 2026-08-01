#!/usr/bin/env python3
"""Spec 164 T010 finalization, capacity, and GC failure injection."""

from __future__ import annotations

from pathlib import Path
import os
import tempfile
import unittest

from py_repoclient import (
    ArtifactReplicaSession,
    HmacReceiptAuthenticator,
    LifecycleTransitionError,
    SqliteRepositoryPersistence,
    artifact_upload_lease_from_dict,
)
from test_spec164_artifact_manifest import ManifestFixture


class SimulatedCrash(RuntimeError):
    pass


class CrashRecoveryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = ManifestFixture()
        self.authenticator = HmacReceiptAuthenticator(
            "/repo/1", "/repo/1/KEY/crash", b"T" * 32
        )

    def tearDown(self) -> None:
        self.fixture.close()

    def _lease(
        self,
        operation_id: str,
        *,
        expires_at_ms: int = 10000,
        now_ms: int = 2000,
    ):
        return artifact_upload_lease_from_dict({
            "leaseId": f"lease-{operation_id}",
            "operationId": operation_id,
            "repoNode": "/repo/1",
            "artifact": self.fixture.artifact_dict,
            "reservedBytes": len(self.fixture.payload),
            "issuedAtMs": 1000,
            "expiresAtMs": expires_at_ms,
            "replayId": f"replay-{operation_id}",
        }, now_ms)

    def _session(self, persistence, operation_id, lease, now_ms=2000):
        fixture = self.fixture
        return ArtifactReplicaSession(
            persistence=persistence,
            operation_id=operation_id,
            repo_node="/repo/1",
            generation=1,
            upload_lease=lease,
            lease_validation_time_ms=now_ms,
            artifact=fixture.artifact,
            signed_root=fixture.signed_root,
            pages=[fixture.page],
            chunks=[fixture.chunk],
            capability=fixture.capability,
            trust_policy=fixture.policy,
            receipt_authenticator=self.authenticator,
            limits=fixture.limits,
        )

    def _verified_session(self, persistence, operation_id):
        session = self._session(
            persistence, operation_id, self._lease(operation_id)
        )
        session.reserve(2001)
        session.receive_chunk(0, self.fixture.payload, now_ms=2002)
        session.verify_complete(2003)
        return session

    def test_every_finalization_crash_point_reconciles_to_active(self):
        for point in (
            "after-intent",
            "after-payload-rename",
            "after-payload-finalize",
            "after-metadata-commit",
            "after-activation",
        ):
            with self.subTest(point=point), tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                database = root / "repo.sqlite3"
                persistence = SqliteRepositoryPersistence(
                    database, f"owner-{point}"
                )
                session = self._verified_session(persistence, f"op-{point}")

                def inject(observed):
                    if observed == point:
                        raise SimulatedCrash(point)

                with self.assertRaisesRegex(SimulatedCrash, point):
                    session.commit_and_activate(
                        2004, crash_injector=inject
                    )
                visible_before = persistence.active_artifact(
                    self.fixture.artifact.logical_name,
                    self.fixture.artifact.policy_epoch,
                )
                self.assertEqual(
                    visible_before is not None, point == "after-activation"
                )
                persistence.close()

                recovered = SqliteRepositoryPersistence(
                    database, f"recovered-{point}"
                )
                try:
                    record = recovered.finalization_record(f"op-{point}")
                    self.assertEqual(record.phase, "ACTIVE")
                    self.assertEqual(record.error, "")
                    self.assertIsNotNone(recovered.active_artifact(
                        self.fixture.artifact.logical_name,
                        self.fixture.artifact.policy_epoch,
                    ))
                    self.assertIsNotNone(recovered.authenticated_receipt(
                        f"op-{point}"
                    ))
                    states = [
                        event.to_state
                        for event in recovered.lifecycle_events(f"op-{point}")
                        if event.accepted
                    ]
                    self.assertEqual(states, [
                        "RESERVED", "RECEIVING", "VERIFIED",
                        "COMMITTED", "ACTIVE",
                    ])
                finally:
                    recovered.close()

    def test_corrupt_recovery_stays_hidden_and_can_roll_back(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            database = root / "repo.sqlite3"
            persistence = SqliteRepositoryPersistence(database, "corrupt")
            session = self._verified_session(persistence, "op-corrupt")

            def inject(point):
                if point == "after-intent":
                    raise SimulatedCrash(point)

            with self.assertRaises(SimulatedCrash):
                session.commit_and_activate(2100, crash_injector=inject)
            path = session.payload_store.staging_path(session.identity)
            with path.open("r+b") as stream:
                stream.write(b"!")
                stream.flush()
            persistence.close()

            recovered = SqliteRepositoryPersistence(database, "recover-corrupt")
            try:
                record = recovered.finalization_record("op-corrupt")
                self.assertEqual(record.phase, "INTENT_RECORDED")
                self.assertTrue(record.error)
                self.assertIsNone(recovered.active_artifact(
                    self.fixture.artifact.logical_name,
                    self.fixture.artifact.policy_epoch,
                ))
                rolled = recovered.rollback_finalization(
                    "op-corrupt", reason="digest mismatch", now_ms=2200
                )
                self.assertEqual(rolled.phase, "ROLLED_BACK")
                self.assertFalse(path.exists())
                self.assertEqual(
                    recovered.lifecycle_events("op-corrupt")[-1].to_state,
                    "FAILED",
                )
            finally:
                recovered.close()

    def test_recovery_cleans_payload_sidecars_left_after_atomic_rename(self):
        with tempfile.TemporaryDirectory() as raw:
            database = Path(raw) / "repo.sqlite3"
            persistence = SqliteRepositoryPersistence(database, "rename-gap")
            session = self._verified_session(persistence, "op-rename-gap")

            def inject(point):
                if point == "after-intent":
                    raise SimulatedCrash(point)

            with self.assertRaises(SimulatedCrash):
                session.commit_and_activate(2250, crash_injector=inject)
            staging = session.payload_store.staging_path(session.identity)
            committed = session.payload_store.committed_path(session.identity)
            range_path = Path(str(staging) + ".ranges")
            intent_path = Path(str(staging) + ".finalize-intent")
            intent_path.write_text("simulated durable intent")
            committed.parent.mkdir(parents=True, exist_ok=True)
            os.replace(staging, committed)
            self.assertTrue(range_path.exists())
            self.assertTrue(intent_path.exists())
            persistence.close()

            recovered = SqliteRepositoryPersistence(database, "rename-recover")
            try:
                self.assertEqual(
                    recovered.finalization_record("op-rename-gap").phase,
                    "ACTIVE",
                )
                self.assertFalse(range_path.exists())
                self.assertFalse(intent_path.exists())
            finally:
                recovered.close()

    def test_capacity_is_reserved_released_and_committed_without_overclaim(self):
        with tempfile.TemporaryDirectory() as raw:
            persistence = SqliteRepositoryPersistence(
                Path(raw) / "repo.sqlite3",
                "capacity",
                capacity_bytes=len(self.fixture.payload),
                reservation_overhead_bytes=0,
            )
            try:
                first = self._verified_session(persistence, "op-capacity-1")
                first.commit_and_activate(2300)
                status = persistence.capacity_status()
                self.assertEqual(
                    status.committed_bytes, len(self.fixture.payload)
                )
                self.assertEqual(status.reserved_bytes, 0)

                with self.assertRaisesRegex(
                    LifecycleTransitionError, "capacity-insufficient"
                ):
                    persistence.reserve_artifact_capacity(
                        operation_id="op-capacity-2",
                        artifact_digest="f" * 64,
                        generation=1,
                        lease_id="lease-capacity-2",
                        reserved_bytes=len(self.fixture.payload),
                        expires_at_ms=10000,
                        now_ms=2301,
                    )
            finally:
                persistence.close()

    def test_exact_live_reservation_is_adopted_after_process_restart(self):
        with tempfile.TemporaryDirectory() as raw:
            database = Path(raw) / "repo.sqlite3"
            digest = "e" * 64
            first = SqliteRepositoryPersistence(
                database,
                "owner-before-crash",
                capacity_bytes=4096,
                reservation_overhead_bytes=0,
            )
            first.reserve_artifact_capacity(
                operation_id="op-restart",
                artifact_digest=digest,
                generation=1,
                lease_id="lease-restart",
                reserved_bytes=1024,
                expires_at_ms=10000,
                now_ms=2000,
            )
            first.close()

            restarted = SqliteRepositoryPersistence(
                database,
                "owner-after-crash",
                capacity_bytes=4096,
                reservation_overhead_bytes=0,
            )
            try:
                status = restarted.reserve_artifact_capacity(
                    operation_id="op-restart",
                    artifact_digest=digest,
                    generation=1,
                    lease_id="lease-restart",
                    reserved_bytes=1024,
                    expires_at_ms=10000,
                    now_ms=2100,
                )
                self.assertEqual(status.reserved_bytes, 1024)
            finally:
                restarted.close()

    def test_gc_requires_expiry_and_exclusive_owner_then_reconciles_crash(self):
        with tempfile.TemporaryDirectory() as raw:
            database = Path(raw) / "repo.sqlite3"
            persistence = SqliteRepositoryPersistence(database, "gc")
            operation_id = "op-gc"
            session = self._session(
                persistence,
                operation_id,
                self._lease(operation_id, expires_at_ms=3000),
            )
            session.reserve(2001)
            session.receive_chunk(0, self.fixture.payload, now_ms=2002)
            with self.assertRaisesRegex(
                LifecycleTransitionError, "repo-gc-protected"
            ):
                persistence.claim_garbage_collection(
                    operation_id=operation_id,
                    artifact_digest=self.fixture.artifact.content_digest,
                    generation=1,
                    gc_owner="collector-a",
                    now_ms=2500,
                    deadline_ms=2600,
                )
            session.expire(3000)
            persistence.claim_garbage_collection(
                operation_id=operation_id,
                artifact_digest=self.fixture.artifact.content_digest,
                generation=1,
                gc_owner="collector-a",
                now_ms=3001,
                deadline_ms=4000,
            )
            with self.assertRaisesRegex(
                LifecycleTransitionError, "ownership-conflict"
            ):
                persistence.claim_garbage_collection(
                    operation_id=operation_id,
                    artifact_digest=self.fixture.artifact.content_digest,
                    generation=1,
                    gc_owner="collector-b",
                    now_ms=3002,
                    deadline_ms=4000,
                )

            def inject(point):
                if point == "after-payload-reclaim":
                    raise SimulatedCrash(point)

            with self.assertRaises(SimulatedCrash):
                persistence.reclaim_temporary_generation(
                    session.identity,
                    operation_id=operation_id,
                    gc_owner="collector-a",
                    now_ms=3003,
                    crash_injector=inject,
                )
            self.assertFalse(
                session.payload_store.staging_path(session.identity).exists()
            )
            persistence.close()

            recovered = SqliteRepositoryPersistence(database, "gc-recovered")
            try:
                record = recovered.transfer_session(operation_id)
                self.assertEqual(record.state, "FAILED")
                states = [
                    event.to_state
                    for event in recovered.lifecycle_events(operation_id)
                    if event.accepted
                ]
                self.assertEqual(states[-1], "EXPIRED")
                claim = recovered.connection.execute("""
                    SELECT state FROM artifact_gc_claims
                    WHERE artifact_digest=? AND generation=1
                """, (self.fixture.artifact.content_digest,)).fetchone()
                self.assertEqual(claim[0], "RECLAIMED")
            finally:
                recovered.close()

    def test_gc_never_claims_active_or_receipted_payload(self):
        with tempfile.TemporaryDirectory() as raw:
            persistence = SqliteRepositoryPersistence(
                Path(raw) / "repo.sqlite3", "gc-active"
            )
            try:
                session = self._verified_session(persistence, "op-active")
                session.commit_and_activate(2400)
                with self.assertRaisesRegex(
                    LifecycleTransitionError, "repo-gc-protected"
                ):
                    persistence.claim_garbage_collection(
                        operation_id="op-active",
                        artifact_digest=self.fixture.artifact.content_digest,
                        generation=1,
                        gc_owner="collector",
                        now_ms=5000,
                        deadline_ms=6000,
                    )
                self.assertTrue(
                    session.payload_store.verify_committed(session.identity)
                )
            finally:
                persistence.close()

    def test_gc_collects_abandoned_open_session_after_lease_expiry(self):
        with tempfile.TemporaryDirectory() as raw:
            persistence = SqliteRepositoryPersistence(
                Path(raw) / "repo.sqlite3", "gc-abandoned"
            )
            try:
                operation_id = "op-abandoned"
                session = self._session(
                    persistence,
                    operation_id,
                    self._lease(operation_id, expires_at_ms=3000),
                )
                session.reserve(2001)
                session.receive_chunk(
                    0, self.fixture.payload, now_ms=2002
                )
                self.assertEqual(
                    persistence.collect_expired_temporaries(
                        gc_owner="startup-gc",
                        now_ms=3001,
                    ),
                    (operation_id,),
                )
                self.assertFalse(
                    session.payload_store.staging_path(
                        session.identity
                    ).exists()
                )
                self.assertEqual(
                    persistence.transfer_session(operation_id).state,
                    "FAILED",
                )
                self.assertEqual(
                    persistence.lifecycle_events(operation_id)[-1].to_state,
                    "EXPIRED",
                )
            finally:
                persistence.close()


if __name__ == "__main__":
    unittest.main()
