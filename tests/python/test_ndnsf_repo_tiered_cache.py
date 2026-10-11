#!/usr/bin/env python3
"""Repo cache and availability policy have one native C++ owner."""

from __future__ import annotations

import unittest

import py_repoclient.orchestration as legacy_repo
from py_repoclient import RepoCompatibilityError


class RetiredPythonRepoCacheTest(unittest.TestCase):
    def test_python_cache_and_network_clients_are_versioned_retired(self):
        for name in (
            "NetworkDistributedRepoClient",
            "RepoNodeApp",
            "RepoObjectManifest",
        ):
            with self.subTest(name=name):
                with self.assertRaises(RepoCompatibilityError) as captured:
                    getattr(legacy_repo, name)()
                self.assertEqual(
                    captured.exception.code, "SPEC192_COMPATIBILITY_V1"
                )


if __name__ == "__main__":
    unittest.main()
