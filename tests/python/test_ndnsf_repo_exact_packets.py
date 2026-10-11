#!/usr/bin/env python3
"""Legacy Python exact-packet implementation is explicitly retired."""

from __future__ import annotations

import unittest

import py_repoclient.orchestration as legacy_repo
from py_repoclient import RepoCompatibilityError


class RetiredExactPacketRepoTest(unittest.TestCase):
    def test_python_owned_exact_packet_runtime_is_versioned_retired(self):
        for name in (
            "LocalDistributedRepo",
            "NetworkDistributedRepoClient",
            "RepoNodeApp",
            "RepoObjectManifest",
            "StorageCapability",
            "ExactPacketRepositoryApi",
        ):
            with self.subTest(name=name):
                try:
                    value = getattr(legacy_repo, name)
                except RepoCompatibilityError as error:
                    self.assertEqual(error.code, "SPEC192_COMPATIBILITY_V1")
                    continue
                with self.assertRaises(RepoCompatibilityError) as captured:
                    value()
                self.assertEqual(
                    captured.exception.code, "SPEC192_COMPATIBILITY_V1"
                )


if __name__ == "__main__":
    unittest.main()
