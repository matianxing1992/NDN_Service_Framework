"""Retired Python recovery cannot reinterpret or clean existing crash material."""
import unittest
from py_repoclient import ArtifactReplicaSession, SqliteRepositoryPersistence
from retired_repo_fixture import LegacySourceFixture


class CrashRecoveryTests(LegacySourceFixture):
    def test_recovery_constructor_preserves_payload_and_journal_sidecars(self):
        (self.root / "legacy.payload.partial").write_bytes(b"partial recovery material")
        (self.root / "legacy.payload.resume.json").write_text('{"operationId":"old-operation"}')
        self.assert_rejected(
            "SqliteRepositoryPersistence", "use the native C++ Repo server",
            lambda: SqliteRepositoryPersistence(self.database, "recovery"))

    def test_expired_python_session_cannot_gc_or_activate_legacy_bytes(self):
        self.assert_rejected(
            "ArtifactReplicaSession", "use native ArtifactRepositoryApi and ArtifactFetchResult",
            lambda: ArtifactReplicaSession(persistence=self.database,
                                           operation_id="expired", lease_validation_time_ms=10000))


if __name__ == "__main__":
    unittest.main()
