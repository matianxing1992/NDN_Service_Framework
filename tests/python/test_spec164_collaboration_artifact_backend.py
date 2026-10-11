import unittest

from py_repoclient import RepoCompatibilityError
from py_repoclient.network_artifact_backend import (
    CollaborationArtifactApiBackend,
    install_artifact_collaboration_service,
)


class RetiredCollaborationArtifactBackendTest(unittest.TestCase):
    def test_backend_and_server_registration_report_native_replacements(self):
        for operation, callback in (
            (
                "CollaborationArtifactApiBackend",
                lambda: CollaborationArtifactApiBackend(None, None, "/repo/store"),
            ),
            (
                "install_artifact_collaboration_service",
                install_artifact_collaboration_service,
            ),
        ):
            with self.subTest(operation=operation):
                with self.assertRaises(RepoCompatibilityError) as caught:
                    callback()
                self.assertEqual(caught.exception.operation, operation)
                self.assertEqual(caught.exception.code, "SPEC192_COMPATIBILITY_V1")
                self.assertTrue(caught.exception.replacement)


if __name__ == "__main__":
    unittest.main()
