#!/usr/bin/env python3
"""Spec 240: the unified RepoClient facade is usable from Python."""

from __future__ import annotations

import hashlib
import os
import shutil
import tempfile
import unittest

import _py_repoclient as native

PAYLOAD = b"spec240 unified client facade v1"
OWNER = "/example/spec240/owner"
OBJECT = "/example/spec240/owner/object"


def reference() -> dict:
    return {
        "logical_name": OBJECT,
        "content_digest": hashlib.sha256(PAYLOAD).hexdigest(),
        "size_bytes": len(PAYLOAD),
        "format_version": "raw-file-v1",
        "publisher_identity": OWNER,
        "policy_epoch": "1",
    }


class UnifiedRepoClientTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = tempfile.mkdtemp(prefix="spec240-py-")
        self.addCleanup(shutil.rmtree, self.root, True)
        self.source = os.path.join(self.root, "source.bin")
        with open(self.source, "wb") as handle:
            handle.write(PAYLOAD)
        self.repo_root = os.path.join(self.root, "repo")

    def test_publish_fetch_and_inspect_round_trip(self) -> None:
        published = native.unified_publish_file(
            self.repo_root, OWNER, self.source, reference(), 30000)
        self.assertEqual(published["state"], "COMMITTED")

        destination = os.path.join(self.root, "fetched.bin")
        fetched = native.unified_fetch_file(
            self.repo_root, OWNER, reference(), destination, 30000)
        self.assertEqual(fetched["bytes_written"], len(PAYLOAD))
        self.assertTrue(fetched["digest_verified"])
        with open(destination, "rb") as handle:
            self.assertEqual(handle.read(), PAYLOAD)

        inspected = native.unified_inspect_object(
            self.repo_root, OWNER, reference(), 30000)
        self.assertEqual(inspected["availability"], "AVAILABLE")
        self.assertEqual(inspected["stored_bytes"], len(PAYLOAD))

    def test_publish_rejects_an_empty_reference_before_side_effects(self) -> None:
        # pybind maps the native invalid_argument rejection to ValueError.
        with self.assertRaises(ValueError):
            native.unified_publish_file(
                self.repo_root, OWNER, self.source, {}, 30000)


if __name__ == "__main__":
    unittest.main()
