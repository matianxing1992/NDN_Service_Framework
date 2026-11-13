#!/usr/bin/env python3
"""Spec 240 CA11: the installed typed helpers and the native module agree.

This module imports the package (installed layout) only. It must not be loaded in the
same process as the top-level extension, because both copies register the same pybind
types.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import tempfile
import unittest

from py_repoclient import unified_client

PAYLOAD = b"spec240 installed helper round trip"
OWNER = "/example/spec240/owner"
OBJECT = "/example/spec240/owner/installed-object"


class InstalledHelperTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = tempfile.mkdtemp(prefix="spec240-installed-")
        self.addCleanup(shutil.rmtree, self.root, True)
        self.source = os.path.join(self.root, "source.bin")
        with open(self.source, "wb") as handle:
            handle.write(PAYLOAD)
        self.repo_root = os.path.join(self.root, "repo")

    def reference(self) -> unified_client.RepoReference:
        return unified_client.RepoReference(
            logical_name=OBJECT,
            content_digest=hashlib.sha256(PAYLOAD).hexdigest(),
            size_bytes=len(PAYLOAD),
            format_version="raw-file-v1",
            publisher_identity=OWNER,
            policy_epoch="1",
        )

    def test_publish_fetch_inspect_through_the_typed_helpers(self) -> None:
        published = unified_client.publish_file(
            self.repo_root, OWNER, self.source, self.reference(), 30000)
        self.assertTrue(published.committed)
        self.assertIsNotNone(published.reference)
        # The native side returns a digest_algorithm the dataclass did not have before
        # CA11; conversion must not raise for it.
        self.assertEqual(published.reference.digest_algorithm, "sha256")
        self.assertEqual(published.reference.size_bytes, len(PAYLOAD))

        destination = os.path.join(self.root, "fetched.bin")
        fetched = unified_client.fetch_file(
            self.repo_root, OWNER, published.reference, destination, 30000)
        self.assertTrue(fetched.digest_verified)
        with open(destination, "rb") as handle:
            self.assertEqual(handle.read(), PAYLOAD)

        inspected = unified_client.inspect_object(
            self.repo_root, OWNER, published.reference, 30000)
        self.assertTrue(inspected.available)


if __name__ == "__main__":
    unittest.main()
