#!/usr/bin/env python3
"""Python-owned Repo coordination is retired in favor of the C++ owner."""

from __future__ import annotations

import unittest

import py_repoclient.orchestration as legacy_repo
from py_repoclient import RepoCompatibilityError


class RetiredRepoCoordinationTest(unittest.TestCase):
    def test_python_owned_coordination_and_retry_state_are_versioned_retired(self):
        for name in (
            "NetworkDistributedRepoClient",
            "RepoNodeApp",
            "RepoObjectManifest",
            "RepoWriteIntent",
            "RepoWriteReceipt",
            "RepoIncompleteWriteError",
            "StorageCapability",
            "PlacementPolicy",
            "select_replicas",
            "normalize_repo_operation_state",
            "required_write_acks",
            "validate_write_receipts",
            "encode_repo_request",
            "decode_repo_request",
            "REPO_OPERATION_STATES",
            "REPO_REJECTION_REASONS",
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
