"""Python authority acquisition must reject without opening or changing a database."""
import unittest
from py_repoclient import MetadataStore, PayloadStore, SqliteRepositoryPersistence
from retired_repo_fixture import LegacySourceFixture


class PersistenceAuthorityTests(LegacySourceFixture):
    def test_distinct_python_writers_cannot_acquire_legacy_database_authority(self):
        for writer in ("writer-a", "writer-b"):
            with self.subTest(writer=writer):
                self.assert_rejected(
                    "SqliteRepositoryPersistence", "use the native C++ Repo server",
                    lambda: SqliteRepositoryPersistence(self.database, writer))

    def test_python_store_facades_cannot_own_metadata_or_payload_transactions(self):
        self.assert_rejected(
            "MetadataStore", "use the native Repo server and artifact-v2 catalog",
            lambda: MetadataStore(self.database))
        self.assert_rejected(
            "PayloadStore", "use the native Repo server or ArtifactRepositoryApi",
            lambda: PayloadStore(self.root / "destination"))


if __name__ == "__main__":
    unittest.main()
