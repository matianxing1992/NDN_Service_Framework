#!/usr/bin/env python3
"""Spec 107 local release upgrade and rollback behavior tests."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "ndnsf-di"))

from run_spec107_operations import LocalReleaseOperations  # noqa: E402


class Spec107UpgradeTest(unittest.TestCase):
    def test_upgrade_discards_incompatible_cache_preserves_repo_and_rolls_back(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            releases = root / "releases"
            for release_id, plan in (("release-n", "1"), ("release-n1", "2")):
                release = releases / release_id
                release.mkdir(parents=True)
                (release / "release.json").write_text(json.dumps({
                    "releaseId": release_id,
                    "planDigest": "sha256:" + plan * 64,
                    "candidateId": (
                        "spec107-c1-111111111111-222222222222-333333333333-"
                        "444444444444-555555555555-666666666666"),
                }), encoding="utf-8")
            repo = root / "var/lib/ndnsf-repo"
            repo.mkdir(parents=True)
            (repo / "catalog.db").write_bytes(b"authoritative-repo")
            cache = root / "var/cache/ndnsf-di"
            cache.mkdir(parents=True)
            (cache / "binding.json").write_text(json.dumps({
                "planDigest": "sha256:" + "1" * 64}), encoding="utf-8")
            (cache / "kv.bin").write_bytes(b"disposable")

            drill = LocalReleaseOperations(root=root)
            drill.activate(releases / "release-n")
            upgraded = drill.activate(releases / "release-n1")
            self.assertEqual(upgraded["cacheDecision"], "DISCARDED_INCOMPATIBLE")
            self.assertFalse((cache / "kv.bin").exists())
            self.assertEqual((repo / "catalog.db").read_bytes(), b"authoritative-repo")
            rolled_back = drill.rollback()
            self.assertEqual(rolled_back["activeReleaseId"], "release-n")
            self.assertEqual((repo / "catalog.db").read_bytes(), b"authoritative-repo")
            self.assertTrue(rolled_back["repoPreserved"])


if __name__ == "__main__":
    unittest.main()
