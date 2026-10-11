#!/usr/bin/env python3
"""Retired Python migration/session entrypoints leave real legacy inputs intact."""

import unittest

from py_repoclient import (
    ArtifactReplicaSession, AtomicArtifactDestination, HmacReceiptAuthenticator,
    SqliteRepositoryPersistence,
)
from retired_repo_fixture import LegacySourceFixture


class MigrationRollbackTest(LegacySourceFixture):
    def test_migration_rollback_and_disabled_modes_never_open_python_persistence(self):
        for options in ({}, {"artifact_writes_enabled": False},
                        {"max_write_schema_generation": 11},
                        {"max_write_schema_generation": 1}):
            with self.subTest(options=options):
                self.assert_rejected(
                    "SqliteRepositoryPersistence", "use the native C++ Repo server",
                    lambda: SqliteRepositoryPersistence(self.database, "retired", **options))
        self.assert_rejected(
            "SqliteRepositoryPersistence", "use the native C++ Repo server",
            lambda: SqliteRepositoryPersistence(self.root / "new.sqlite3", "retired"))

    def test_retired_sessions_and_destinations_cannot_mutate_legacy_inputs(self):
        self.assert_rejected(
            "ArtifactReplicaSession", "use native ArtifactRepositoryApi and ArtifactFetchResult",
            lambda: ArtifactReplicaSession(persistence=self.database, operation_id="retired",
                                           destination_path=self.root / "session"))
        self.assert_rejected(
            "AtomicArtifactDestination", "use ArtifactRepositoryApi.fetch_file",
            lambda: AtomicArtifactDestination(self.root / "destination"))
        self.assert_rejected(
            "HmacReceiptAuthenticator", "use controller-authenticated native receipts",
            lambda: HmacReceiptAuthenticator("/repo/1", "/repo/1/KEY/legacy", b"M" * 32))


if __name__ == "__main__":
    unittest.main()
