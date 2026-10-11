import unittest

from py_repoclient import (
    ArtifactRepositoryApi,
    FilesystemArtifactApiBackend,
    RepoCompatibilityError,
)


class RetiredLocalArtifactBackendTest(unittest.TestCase):
    def test_python_backend_is_replaced_by_native_in_app_factory(self):
        with self.assertRaises(RepoCompatibilityError) as caught:
            FilesystemArtifactApiBackend("/tmp/spec192-retired-local-store")
        self.assertEqual(caught.exception.code, "SPEC192_COMPATIBILITY_V1")
        self.assertEqual(caught.exception.operation, "FilesystemArtifactApiBackend")
        self.assertIn("ArtifactRepositoryApi.make_in_app", caught.exception.replacement)
        self.assertTrue(callable(ArtifactRepositoryApi.make_in_app))


if __name__ == "__main__":
    unittest.main()
