#!/usr/bin/env python3
"""Spec 109 source snapshot and campaign binding tests."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "ndnsf-di"))

from spec109_source import (  # noqa: E402
    SourceSnapshotError,
    campaign_id,
    capture_source_snapshot,
    validate_source_snapshot,
)


class Spec109SourceTest(unittest.TestCase):
    def _repo(self, root: Path) -> None:
        subprocess.run(["git", "init", "-q"], cwd=root, check=True)
        subprocess.run(["git", "config", "user.email", "spec109@example.invalid"], cwd=root, check=True)
        subprocess.run(["git", "config", "user.name", "Spec 109"], cwd=root, check=True)
        (root / "tracked.txt").write_text("original\n", encoding="utf-8")
        subprocess.run(["git", "add", "tracked.txt"], cwd=root, check=True)
        subprocess.run(["git", "commit", "-qm", "baseline"], cwd=root, check=True)

    def test_clean_snapshot_is_self_validating(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._repo(root)
            value = capture_source_snapshot(root, captured_at="2026-07-13T00:00:00Z")
            self.assertEqual(value["worktreeState"], "CLEAN")
            self.assertIsNone(value["binaryDiffDigest"])
            self.assertEqual(validate_source_snapshot(value), value)
            changed = dict(value)
            changed["treeDigest"] = "sha256:" + "0" * 64
            with self.assertRaisesRegex(SourceSnapshotError, "SOURCE_SNAPSHOT_DIGEST_MISMATCH"):
                validate_source_snapshot(changed)

    def test_dirty_snapshot_requires_and_seals_archive(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._repo(root)
            (root / "tracked.txt").write_text("changed\n", encoding="utf-8")
            (root / "new.txt").write_text("untracked\n", encoding="utf-8")
            with self.assertRaisesRegex(SourceSnapshotError, "SOURCE_DIRTY_ARCHIVE_REQUIRED"):
                capture_source_snapshot(root)
            archive = root / "seal" / "untracked.tar"
            value = capture_source_snapshot(
                root, captured_at="2026-07-13T00:00:00Z", untracked_archive=archive)
            self.assertEqual(value["worktreeState"], "SEALED_DIRTY")
            self.assertTrue(archive.is_file())
            self.assertEqual(validate_source_snapshot(value), value)

    def test_campaign_id_changes_for_each_binding(self) -> None:
        digests = ["sha256:" + str(i) * 64 for i in range(1, 5)]
        first = campaign_id(
            source_digest=digests[0], predecessor_digest=digests[1],
            deployment_digest=digests[2], matrix_digest=digests[3])
        self.assertTrue(first.startswith("spec109-"))
        changed = campaign_id(
            source_digest="sha256:" + "9" * 64, predecessor_digest=digests[1],
            deployment_digest=digests[2], matrix_digest=digests[3])
        self.assertNotEqual(first, changed)
        with self.assertRaisesRegex(SourceSnapshotError, "CAMPAIGN_BINDING_DIGEST_INVALID"):
            campaign_id(source_digest="bad", predecessor_digest=digests[1],
                        deployment_digest=digests[2], matrix_digest=digests[3])


if __name__ == "__main__":
    unittest.main()
