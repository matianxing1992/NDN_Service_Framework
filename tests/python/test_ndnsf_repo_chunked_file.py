#!/usr/bin/env python3
"""The old Python chunk store is not a Repo implementation."""

from __future__ import annotations

import unittest

import py_repoclient.orchestration as legacy_repo
from py_repoclient import RepoCompatibilityError


class RetiredPythonChunkStoreTest(unittest.TestCase):
    def test_python_chunk_store_name_fails_with_versioned_replacement(self):
        with self.assertRaises(RepoCompatibilityError) as captured:
            legacy_repo.DistributedRepo()
        self.assertEqual(captured.exception.code, "SPEC192_COMPATIBILITY_V1")
        self.assertIn("DistributedRepoServer", captured.exception.replacement)


if __name__ == "__main__":
    unittest.main()
