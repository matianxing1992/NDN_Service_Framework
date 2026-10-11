"""Retired Python streaming stores cannot create ranges or mutate committed bytes."""
import unittest
from py_repoclient import ArtifactStorageIdentity, FilesystemCasPayloadStore
from retired_repo_fixture import LegacySourceFixture


class StreamingArtifactStoreTests(LegacySourceFixture):
    def test_python_storage_identity_cannot_reinterpret_native_artifact_identity(self):
        self.assert_rejected(
            "ArtifactStorageIdentity", "use native ArtifactReference and ArtifactHandle",
            lambda: ArtifactStorageIdentity("/legacy/artifact", "ab" * 32, 1))

    def test_python_streaming_store_cannot_create_payload_or_range_sidecars(self):
        self.assert_rejected(
            "FilesystemCasPayloadStore", "use ArtifactRepositoryApi.make_in_app",
            lambda: FilesystemCasPayloadStore(self.root / "destination"))


if __name__ == "__main__":
    unittest.main()
