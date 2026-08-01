#!/usr/bin/env python3
"""Spec 164 T003 tests for one persistence authority and lifecycle journal."""

from __future__ import annotations

import sqlite3
import tempfile
from pathlib import Path
import unittest

from py_repoclient.persistence import (
    LifecycleTransitionError,
    PersistenceOwnershipError,
    SqliteRepositoryPersistence,
)


DIGEST = "ab" * 32


class PersistenceAuthorityTests(unittest.TestCase):
    def test_one_backend_has_exactly_one_live_authority(self):
        with tempfile.TemporaryDirectory() as temporary:
            database = Path(temporary) / "repo.sqlite3"
            first = SqliteRepositoryPersistence(database, "python-owner-1")
            try:
                with self.assertRaisesRegex(
                        PersistenceOwnershipError, "^repo-persistence-owned:"):
                    SqliteRepositoryPersistence(database, "python-owner-2")
            finally:
                first.close()

            second = SqliteRepositoryPersistence(database, "python-owner-2")
            self.assertEqual(second.owner_id, "python-owner-2")
            second.close()

    def test_connection_transaction_control_returns_to_facade(self):
        with tempfile.TemporaryDirectory() as temporary:
            database = Path(temporary) / "repo.sqlite3"
            persistence = SqliteRepositoryPersistence(database, "python-owner")
            connection = persistence.connection
            connection.execute("CREATE TABLE owned_write(value TEXT NOT NULL)")
            connection.execute("INSERT INTO owned_write(value) VALUES('committed')")
            connection.commit()
            connection.close()

            with sqlite3.connect(database) as reader:
                row = reader.execute("SELECT value FROM owned_write").fetchone()
            self.assertEqual(row, ("committed",))

    def test_lifecycle_is_strict_durable_and_idempotent(self):
        with tempfile.TemporaryDirectory() as temporary:
            database = Path(temporary) / "repo.sqlite3"
            persistence = SqliteRepositoryPersistence(database, "python-owner")
            try:
                reserved = persistence.metadata_store.transition(
                    event_id="event-1",
                    operation_id="operation-1",
                    artifact_digest=DIGEST,
                    generation=1,
                    from_state="ABSENT",
                    to_state="RESERVED",
                    event_time_ms=1000,
                    detail={"lease": "lease-1"},
                )
                replay = persistence.metadata_store.transition(
                    event_id="event-1",
                    operation_id="operation-1",
                    artifact_digest=DIGEST,
                    generation=1,
                    from_state="ABSENT",
                    to_state="RESERVED",
                    event_time_ms=1000,
                    detail={"lease": "lease-1"},
                )
                self.assertTrue(reserved.accepted)
                self.assertEqual(replay.sequence, reserved.sequence)

                with self.assertRaisesRegex(
                        LifecycleTransitionError,
                        "^repo-lifecycle-event-conflict:"):
                    persistence.metadata_store.transition(
                        event_id="event-1",
                        operation_id="operation-1",
                        artifact_digest=DIGEST,
                        generation=2,
                        from_state="ABSENT",
                        to_state="RESERVED",
                        event_time_ms=1000,
                        detail={"lease": "lease-1"},
                    )
            finally:
                persistence.close()

            reopened = SqliteRepositoryPersistence(database, "recovery-owner")
            try:
                events = reopened.lifecycle_events("operation-1")
                self.assertEqual(len(events), 1)
                self.assertEqual(events[0].to_state, "RESERVED")
            finally:
                reopened.close()

    def test_illegal_transition_is_recorded_but_does_not_advance_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            persistence = SqliteRepositoryPersistence(
                Path(temporary) / "repo.sqlite3", "python-owner"
            )
            try:
                persistence.transition(
                    event_id="event-1",
                    operation_id="operation-1",
                    artifact_digest=DIGEST,
                    generation=1,
                    from_state="ABSENT",
                    to_state="RESERVED",
                    event_time_ms=1000,
                )
                with self.assertRaisesRegex(
                        LifecycleTransitionError,
                        "^repo-lifecycle-illegal-transition:"):
                    persistence.transition(
                        event_id="event-2",
                        operation_id="operation-1",
                        artifact_digest=DIGEST,
                        generation=1,
                        from_state="RESERVED",
                        to_state="COMMITTED",
                        event_time_ms=1100,
                        detail={"reason": "attempted verification bypass"},
                    )

                events = persistence.lifecycle_events("operation-1")
                self.assertEqual(len(events), 2)
                self.assertTrue(events[0].accepted)
                self.assertFalse(events[1].accepted)

                receiving = persistence.transition(
                    event_id="event-3",
                    operation_id="operation-1",
                    artifact_digest=DIGEST,
                    generation=1,
                    from_state="RESERVED",
                    to_state="RECEIVING",
                    event_time_ms=1200,
                )
                self.assertTrue(receiving.accepted)
            finally:
                persistence.close()

    def test_operation_identity_cannot_change_digest_or_generation(self):
        with tempfile.TemporaryDirectory() as temporary:
            persistence = SqliteRepositoryPersistence(
                Path(temporary) / "repo.sqlite3", "python-owner"
            )
            try:
                persistence.transition(
                    event_id="event-1",
                    operation_id="operation-1",
                    artifact_digest=DIGEST,
                    generation=1,
                    from_state="ABSENT",
                    to_state="RESERVED",
                    event_time_ms=1000,
                )
                with self.assertRaisesRegex(
                        LifecycleTransitionError,
                        "^repo-lifecycle-identity-conflict:"):
                    persistence.transition(
                        event_id="event-2",
                        operation_id="operation-1",
                        artifact_digest="cd" * 32,
                        generation=2,
                        from_state="RESERVED",
                        to_state="RECEIVING",
                        event_time_ms=1100,
                    )
                events = persistence.lifecycle_events("operation-1")
                self.assertEqual(len(events), 2)
                self.assertFalse(events[-1].accepted)
            finally:
                persistence.close()

    def test_lifecycle_event_metadata_is_bounded_before_insert(self):
        with tempfile.TemporaryDirectory() as temporary:
            persistence = SqliteRepositoryPersistence(
                Path(temporary) / "repo.sqlite3", "python-owner"
            )
            try:
                with self.assertRaisesRegex(
                        LifecycleTransitionError,
                        "^repo-lifecycle-invalid-event:"):
                    persistence.transition(
                        event_id="event-1",
                        operation_id="operation-1",
                        artifact_digest=DIGEST,
                        generation=1,
                        from_state="ABSENT",
                        to_state="RESERVED",
                        event_time_ms=1000,
                        detail={"oversized": "x" * (16 * 1024)},
                    )
                self.assertEqual(
                    persistence.lifecycle_events("operation-1"), ()
                )
            finally:
                persistence.close()


if __name__ == "__main__":
    unittest.main()
