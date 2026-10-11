from __future__ import annotations

import unittest

from py_repoclient import (
    ArtifactRepositoryApi,
    FilesystemArtifactApiBackend,
    NetworkDistributedRepoClient,
    RepoCompatibilityError,
    RepoNodeApp,
)
from py_repoclient.artifact_api import ArtifactUploadSession
from py_repoclient.network_artifact_backend import (
    CollaborationArtifactApiBackend,
    install_artifact_collaboration_service,
)
from py_repoclient.orchestration import (
    NetworkDistributedRepoClient as LegacyNetworkClient,
    RepoNodeApp as LegacyRepoNode,
    encode_repo_request,
)


class Spec192PythonCompatibilityTest(unittest.TestCase):
    def assert_retired(self, operation: str, callback) -> None:
        with self.assertRaises(RepoCompatibilityError) as caught:
            callback()
        self.assertEqual(caught.exception.code, "SPEC192_COMPATIBILITY_V1")
        self.assertEqual(caught.exception.operation, operation)
        self.assertTrue(caught.exception.replacement)

    def test_python_owned_backends_sessions_and_servers_are_retired(self):
        self.assert_retired(
            "FilesystemArtifactApiBackend",
            lambda: FilesystemArtifactApiBackend("/tmp/spec192-retired"),
        )
        self.assert_retired(
            "CollaborationArtifactApiBackend",
            lambda: CollaborationArtifactApiBackend(None, None, "/repo/store"),
        )
        self.assert_retired("ArtifactUploadSession", ArtifactUploadSession)
        self.assert_retired(
            "ArtifactRepositoryApi(backend=...)",
            lambda: ArtifactRepositoryApi(object()),
        )
        self.assert_retired("RepoNodeApp", RepoNodeApp)
        self.assert_retired("NetworkDistributedRepoClient", NetworkDistributedRepoClient)
        self.assert_retired(
            "install_artifact_collaboration_service",
            install_artifact_collaboration_service,
        )

    def test_orchestration_imports_fail_with_versioned_replacements(self):
        self.assertIs(LegacyNetworkClient, NetworkDistributedRepoClient)
        self.assertIs(LegacyRepoNode, RepoNodeApp)
        self.assert_retired("encode_repo_request", encode_repo_request)


if __name__ == "__main__":
    unittest.main()
