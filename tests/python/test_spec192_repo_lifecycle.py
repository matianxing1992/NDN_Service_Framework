from __future__ import annotations

import unittest

from py_repoclient import (
    ArtifactRepositoryApi,
    RepoCompatibilityError,
)


class Spec192PythonCompatibilityTest(unittest.TestCase):
    def assert_retired(self, operation: str, callback) -> None:
        with self.assertRaises(RepoCompatibilityError) as caught:
            callback()
        self.assertEqual(caught.exception.code, "SPEC192_COMPATIBILITY_V1")
        self.assertEqual(caught.exception.operation, operation)
        self.assertTrue(caught.exception.replacement)

    def test_python_owned_backend_construction_is_retired(self):
        self.assert_retired(
            "ArtifactRepositoryApi(backend=...)",
            lambda: ArtifactRepositoryApi(object()),
        )


if __name__ == "__main__":
    unittest.main()
