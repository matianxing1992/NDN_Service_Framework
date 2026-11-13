#!/usr/bin/env python3
"""Spec 240: the typed helpers translate native dictionaries without guessing."""

from __future__ import annotations

import unittest

from py_repoclient import unified_client


class _FakeNative:
    def __init__(self) -> None:
        self.calls: list[tuple] = []

    def unified_publish_file(self, root, owner, source, reference, timeout_ms):
        self.calls.append(("publish", root, owner, source, dict(reference), timeout_ms))
        return {
            "state": "COMMITTED",
            "reference": dict(reference),
            "receipt_ids": ["r1"],
            "deduplicated": True,
            "resumed": False,
        }

    def unified_fetch_file(self, root, owner, reference, target, timeout_ms):
        self.calls.append(("fetch", root, owner, dict(reference), target, timeout_ms))
        return {"bytes_written": 7, "digest_verified": True,
                "content_digest": "abc", "attempted_sources": ["/repo/a"]}

    def unified_inspect_object(self, root, owner, reference, timeout_ms):
        self.calls.append(("inspect", root, owner, dict(reference), timeout_ms))
        return {"availability": "AVAILABLE", "stored_bytes": 7,
                "lifecycle_state": "COMMITTED"}


class UnifiedClientHelperTest(unittest.TestCase):
    def setUp(self) -> None:
        self.fake = _FakeNative()
        self._original = unified_client._native
        unified_client._native = lambda: self.fake
        self.addCleanup(setattr, unified_client, "_native", self._original)
        self.reference = unified_client.RepoReference(
            logical_name="/owner/object", content_digest="d", size_bytes=7,
            publisher_identity="/owner")

    def test_publish_returns_a_typed_result_and_passes_the_reference_through(self):
        result = unified_client.publish_file("/root", "/owner", "/src", self.reference, 1234)
        self.assertTrue(result.committed)
        self.assertEqual(result.receipt_ids, ("r1",))
        self.assertTrue(result.deduplicated)
        self.assertEqual(result.reference.size_bytes, 7)
        kind, root, owner, source, reference, timeout = self.fake.calls[0]
        self.assertEqual((kind, root, owner, source, timeout), ("publish", "/root", "/owner", "/src", 1234))
        self.assertEqual(reference["logical_name"], "/owner/object")

    def test_fetch_and_inspect_expose_the_native_decisions_unchanged(self):
        fetched = unified_client.fetch_file("/root", "/owner", self.reference, "/dst", 9)
        self.assertEqual(fetched.bytes_written, 7)
        self.assertTrue(fetched.digest_verified)
        self.assertEqual(tuple(fetched.attempted_sources), ("/repo/a",))
        inspected = unified_client.inspect_object("/root", "/owner", self.reference, 9)
        self.assertTrue(inspected.available)
        self.assertEqual(inspected.stored_bytes, 7)


if __name__ == "__main__":
    unittest.main()
