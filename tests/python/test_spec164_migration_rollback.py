#!/usr/bin/env python3
"""Spec 164 T015 non-destructive migration and rollback contracts."""

from __future__ import annotations

from dataclasses import replace
import hashlib
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

from py_repoclient import (
    ArtifactReplicaSession,
    HmacReceiptAuthenticator,
    LifecycleTransitionError,
    SqliteRepositoryPersistence,
    artifact_upload_lease_from_dict,
)
from py_repoclient.orchestration import encode_repo_request
from test_ndnsf_repo_ha import make_repo
from test_spec164_artifact_manifest import ManifestFixture


def _downgrade_artifact_metadata_to_v11(database: Path) -> None:
    """Construct the exact additive-migration predecessor without touching bytes."""

    connection = sqlite3.connect(database)
    try:
        connection.execute("DROP TABLE artifact_schema_state")
        connection.execute(
            "ALTER TABLE artifact_active_catalog RENAME TO active_v12"
        )
        connection.execute("""
            CREATE TABLE artifact_active_catalog (
                logical_name TEXT NOT NULL,
                policy_epoch TEXT NOT NULL,
                artifact_digest TEXT NOT NULL,
                generation INTEGER NOT NULL,
                operation_id TEXT NOT NULL,
                artifact_json TEXT NOT NULL,
                activated_at_ms INTEGER NOT NULL,
                PRIMARY KEY (logical_name, policy_epoch)
            )
        """)
        connection.execute("""
            INSERT INTO artifact_active_catalog
            SELECT logical_name, policy_epoch, artifact_digest, generation,
                   operation_id, artifact_json, activated_at_ms
            FROM active_v12
        """)
        connection.execute("DROP TABLE active_v12")

        connection.execute(
            "ALTER TABLE artifact_replica_receipts RENAME TO receipts_v12"
        )
        connection.execute("""
            CREATE TABLE artifact_replica_receipts (
                receipt_id TEXT PRIMARY KEY,
                operation_id TEXT NOT NULL UNIQUE,
                artifact_digest TEXT NOT NULL,
                repo_node TEXT NOT NULL,
                generation INTEGER NOT NULL,
                receipt_json TEXT NOT NULL,
                signer_key_id TEXT NOT NULL,
                authentication_algorithm TEXT NOT NULL,
                signature_hex TEXT NOT NULL,
                committed_at_ms INTEGER NOT NULL
            )
        """)
        connection.execute("""
            INSERT INTO artifact_replica_receipts
            SELECT receipt_id, operation_id, artifact_digest, repo_node,
                   generation, receipt_json, signer_key_id,
                   authentication_algorithm, signature_hex, committed_at_ms
            FROM receipts_v12
        """)
        connection.execute("DROP TABLE receipts_v12")

        connection.execute("ALTER TABLE artifact_gc_claims RENAME TO gc_v12")
        connection.execute("""
            CREATE TABLE artifact_gc_claims (
                artifact_digest TEXT NOT NULL,
                generation INTEGER NOT NULL,
                operation_id TEXT NOT NULL,
                gc_owner TEXT NOT NULL,
                claimed_at_ms INTEGER NOT NULL,
                deadline_ms INTEGER NOT NULL,
                state TEXT NOT NULL,
                PRIMARY KEY (artifact_digest, generation)
            )
        """)
        connection.execute("""
            INSERT INTO artifact_gc_claims
            SELECT artifact_digest, generation, operation_id, gc_owner,
                   claimed_at_ms, deadline_ms, state
            FROM gc_v12
        """)
        connection.execute("DROP TABLE gc_v12")
        connection.commit()
    finally:
        connection.close()


class MigrationRollbackTest(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = ManifestFixture()
        self.authenticator = HmacReceiptAuthenticator(
            "/repo/1", "/repo/1/KEY/migration", b"M" * 32
        )

    def tearDown(self) -> None:
        self.fixture.close()

    def _commit(self, database: Path, operation_id: str = "op-migration"):
        persistence = SqliteRepositoryPersistence(database, "writer")
        lease = artifact_upload_lease_from_dict({
            "leaseId": f"lease-{operation_id}",
            "operationId": operation_id,
            "repoNode": "/repo/1",
            "artifact": self.fixture.artifact_dict,
            "reservedBytes": len(self.fixture.payload),
            "issuedAtMs": 1000,
            "expiresAtMs": 10000,
            "replayId": f"replay-{operation_id}",
        }, 2000)
        session = ArtifactReplicaSession(
            persistence=persistence,
            operation_id=operation_id,
            repo_node="/repo/1",
            generation=1,
            upload_lease=lease,
            lease_validation_time_ms=2000,
            artifact=self.fixture.artifact,
            signed_root=self.fixture.signed_root,
            pages=[self.fixture.page],
            chunks=[self.fixture.chunk],
            capability=self.fixture.capability,
            trust_policy=self.fixture.policy,
            receipt_authenticator=self.authenticator,
            limits=self.fixture.limits,
        )
        session.reserve(2001)
        session.receive_chunk(0, self.fixture.payload, now_ms=2002)
        session.verify_complete(2003)
        session.commit_and_activate(2004)
        payload_path = session.payload_store.committed_path(session.identity)
        return persistence, payload_path

    def test_v11_roll_forward_is_additive_and_preserves_payload(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            database = Path(raw) / "repo.sqlite3"
            persistence, payload_path = self._commit(database)
            before = hashlib.sha256(payload_path.read_bytes()).hexdigest()
            persistence.close()
            _downgrade_artifact_metadata_to_v11(database)

            migrated = SqliteRepositoryPersistence(database, "migrated")
            try:
                diagnostics = migrated.migration_diagnostics()
                self.assertEqual(diagnostics["previousSchemaGeneration"], 11)
                self.assertEqual(diagnostics["action"], "roll-forward")
                self.assertTrue(diagnostics["writesEnabled"])
                self.assertFalse(diagnostics["destructiveChanges"])
                self.assertEqual(
                    hashlib.sha256(payload_path.read_bytes()).hexdigest(),
                    before,
                )
                active = migrated.active_artifact(
                    self.fixture.artifact.logical_name,
                    self.fixture.artifact.policy_epoch,
                )
                self.assertEqual(active, self.fixture.artifact_dict)
                self.assertIsNotNone(
                    migrated.authenticated_receipt("op-migration")
                )
                connection = sqlite3.connect(database)
                try:
                    catalog = connection.execute(
                        "SELECT format_version, digest_algorithm "
                        "FROM artifact_active_catalog"
                    ).fetchone()
                    receipt = connection.execute(
                        "SELECT format_version, digest_algorithm "
                        "FROM artifact_replica_receipts"
                    ).fetchone()
                    gc_columns = {
                        row[1] for row in connection.execute(
                            "PRAGMA table_info(artifact_gc_claims)"
                        )
                    }
                finally:
                    connection.close()
                self.assertEqual(
                    catalog, ("artifact-manifest-v2", "sha256")
                )
                self.assertEqual(
                    receipt, ("artifact-manifest-v2", "sha256")
                )
                self.assertTrue(
                    {"format_version", "digest_algorithm"}.issubset(gc_columns)
                )
            finally:
                migrated.close()

    def test_rollback_runtime_reads_committed_bytes_but_rejects_v2_writes(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as raw:
            database = Path(raw) / "repo.sqlite3"
            persistence, payload_path = self._commit(database)
            committed_bytes = payload_path.read_bytes()
            with persistence.lock:
                persistence.connection.execute("""
                    CREATE TABLE IF NOT EXISTS data_packets (
                        data_name TEXT PRIMARY KEY,
                        wire BLOB NOT NULL
                    )
                """)
                persistence.connection.execute(
                    "INSERT INTO data_packets(data_name, wire) VALUES(?, ?)",
                    ("/legacy/packet", sqlite3.Binary(b"exact-wire")),
                )
                persistence.connection.commit()
            persistence.close()

            rollback = SqliteRepositoryPersistence(
                database,
                "rollback-runtime",
                max_write_schema_generation=11,
            )
            try:
                diagnostics = rollback.migration_diagnostics()
                self.assertFalse(diagnostics["writesEnabled"])
                self.assertEqual(diagnostics["action"], "read-only-rollback")
                self.assertEqual(payload_path.read_bytes(), committed_bytes)
                self.assertIsNotNone(rollback.active_artifact(
                    self.fixture.artifact.logical_name,
                    self.fixture.artifact.policy_epoch,
                ))
                self.assertIsNotNone(
                    rollback.authenticated_receipt("op-migration")
                )
                wire = rollback.connection.execute(
                    "SELECT wire FROM data_packets WHERE data_name=?",
                    ("/legacy/packet",),
                ).fetchone()
                self.assertEqual(bytes(wire[0]), b"exact-wire")
                with self.assertRaisesRegex(
                    LifecycleTransitionError, "repo-artifact-writes-disabled"
                ):
                    rollback.transition(
                        event_id="blocked",
                        operation_id="blocked",
                        artifact_digest="ab" * 32,
                        generation=2,
                        from_state="ABSENT",
                        to_state="RESERVED",
                    )
            finally:
                rollback.close()

    def test_operator_disabled_v11_database_remains_readable_without_upgrade(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as raw:
            database = Path(raw) / "repo.sqlite3"
            persistence, payload_path = self._commit(database)
            committed_bytes = payload_path.read_bytes()
            persistence.close()
            _downgrade_artifact_metadata_to_v11(database)

            disabled = SqliteRepositoryPersistence(
                database,
                "disabled-v11",
                artifact_writes_enabled=False,
            )
            try:
                diagnostics = disabled.migration_diagnostics()
                self.assertEqual(diagnostics["databaseSchemaGeneration"], 11)
                self.assertEqual(diagnostics["action"], "read-only-rollback")
                self.assertEqual(
                    diagnostics["reason"], "operator-disabled"
                )
                self.assertEqual(payload_path.read_bytes(), committed_bytes)
                self.assertIsNotNone(disabled.active_artifact(
                    self.fixture.artifact.logical_name,
                    self.fixture.artifact.policy_epoch,
                ))
                self.assertIsNotNone(
                    disabled.authenticated_receipt("op-migration")
                )
                columns = {
                    row[1] for row in disabled.connection.execute(
                        "PRAGMA table_info(artifact_active_catalog)"
                    )
                }
                self.assertNotIn("format_version", columns)
            finally:
                disabled.close()

    def test_newer_unknown_schema_fails_closed_without_reinterpretation(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as raw:
            database = Path(raw) / "repo.sqlite3"
            persistence, payload_path = self._commit(database)
            committed_bytes = payload_path.read_bytes()
            persistence.connection.execute(
                "UPDATE artifact_schema_state SET schema_generation=13 "
                "WHERE singleton=1"
            )
            persistence.connection.commit()
            persistence.close()

            older = SqliteRepositoryPersistence(database, "older-runtime")
            try:
                diagnostics = older.migration_diagnostics()
                self.assertFalse(diagnostics["writesEnabled"])
                self.assertEqual(
                    diagnostics["reason"],
                    "database-schema-newer-than-write-runtime",
                )
                self.assertEqual(payload_path.read_bytes(), committed_bytes)
                self.assertIsNotNone(older.active_artifact(
                    self.fixture.artifact.logical_name,
                    self.fixture.artifact.policy_epoch,
                ))
            finally:
                older.close()

    def test_capability_reports_migration_and_withdraws_v2_when_read_only(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as raw:
            database = Path(raw) / "repo.sqlite3"
            repo = make_repo(database)
            repo._persistence.close()
            repo._persistence = SqliteRepositoryPersistence(
                database,
                "rollback-capability",
                max_write_schema_generation=11,
            )
            repo._db = repo._persistence.connection
            repo._db_lock = repo._persistence.lock
            repo.capability = replace(
                repo.capability,
                artifact_format_versions=("exact-packet-v1",),
                artifact_supports_resume=False,
                artifact_supports_replica_receipts=False,
            )
            try:
                response = repo._handle(encode_repo_request("CAPABILITY"))
                self.assertTrue(response.status)
                payload = json.loads(response.payload)
                self.assertFalse(
                    payload["artifactMigration"]["writesEnabled"]
                )
                self.assertEqual(
                    payload["artifactCapability"]["formatVersions"],
                    ["exact-packet-v1"],
                )
                service_payload = payload["providerCapabilityHint"][
                    "service_payload"
                ]
                self.assertEqual(
                    service_payload["artifactMigration"],
                    payload["artifactMigration"],
                )
            finally:
                repo._persistence.close()


if __name__ == "__main__":
    unittest.main()
