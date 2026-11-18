"""Regression for the CD-05 tracked-entry collector.

CD-05 requires coverage of the **git index** — dotfiles, Waf helpers, templates, locks, workflows,
gitlinks and symlinks — with text and binary assets separated and private/bulk content left unread. These
cases pin the parts that are easy to regress: index-driven enumeration, symlink/gitlink handling, and the
refusal to read private material.
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COLLECTOR = ROOT / "scripts/collect-tracked-entry-inventory.py"

spec = importlib.util.spec_from_file_location("collect_inventory", COLLECTOR)
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)


class TrackedEntryInventoryTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        self.git("init", "-q")
        self.git("config", "user.email", "test@example.com")
        self.git("config", "user.name", "test")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def git(self, *args: str) -> None:
        subprocess.run(["git", *args], cwd=str(self.repo), check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def write(self, name: str, data: bytes = b"x\n") -> None:
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def commit(self) -> None:
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "snapshot")

    def inventory(self) -> dict:
        self.commit()
        return collector.build_inventory(self.repo)

    def test_untracked_paths_are_excluded(self) -> None:
        self.write(".github/workflows/ci.yml")
        self.commit()
        self.write("untracked.txt")
        inventory = collector.build_inventory(self.repo)
        paths = [entry["path"] for entry in inventory["entries"]]
        self.assertIn(".github/workflows/ci.yml", paths)
        self.assertNotIn("untracked.txt", paths)

    def test_categories_cover_the_cd05_shapes(self) -> None:
        for name in (".github/workflows/ci.yml", "waflib/x.py", "pkg.pc.in", "runtime.def.in",
                     "app.conf", "cargo.lock", "schema.json", "script.sh", "core.cpp"):
            self.write(name)
        inventory = self.inventory()
        seen = {category for entry in inventory["entries"] for category in entry["categories"]}
        for expected in ("workflow", "waf-helper", "pkgconfig-template", "container-definition",
                         "config", "lock", "script", "source"):
            self.assertIn(expected, seen)

    def test_symlink_and_gitlink_are_described_without_following(self) -> None:
        self.write("target.txt")
        (self.repo / "link.txt").symlink_to("target.txt")
        (self.repo / "dangling.txt").symlink_to("absent.txt")
        self.commit()
        inventory = collector.build_inventory(self.repo)
        by_path = {entry["path"]: entry for entry in inventory["entries"]}
        self.assertEqual(by_path["link.txt"]["kind"], "symlink")
        self.assertEqual(by_path["link.txt"]["size"], 0)
        self.assertFalse(by_path["link.txt"]["dangling"])
        self.assertTrue(by_path["dangling.txt"]["dangling"])

    def test_gitlink_mode_is_described_without_a_working_tree(self) -> None:
        # A submodule is present in the index as mode 160000 and may not be checked out; describing it
        # must not touch the filesystem.
        record = collector.describe(self.repo, "vendor/sub", 0o160000)
        self.assertEqual(record["kind"], "gitlink")
        self.assertEqual(record["size"], 0)

    def test_private_and_bulk_content_is_not_read(self) -> None:
        self.write("secret.ndnkey", b"\x00private")
        self.write("model.onnx", b"\x00weights")
        inventory = self.inventory()
        by_path = {entry["path"]: entry for entry in inventory["entries"]}
        self.assertEqual(by_path["secret.ndnkey"]["content"], "private-not-read")
        self.assertEqual(by_path["model.onnx"]["content"], "bulk-not-read")
        self.assertNotIn("sha256", by_path["secret.ndnkey"])
        self.assertNotIn("sha256", by_path["model.onnx"])

    def test_binary_detection_uses_the_leading_window(self) -> None:
        self.write("text.txt", b"plain text\n")
        self.write("blob.dat", b"abc\x00def")
        inventory = self.inventory()
        by_path = {entry["path"]: entry for entry in inventory["entries"]}
        self.assertEqual(by_path["text.txt"]["content"], "text")
        self.assertEqual(by_path["blob.dat"]["content"], "binary")

    def test_output_is_deterministic(self) -> None:
        self.write("a.txt")
        self.write("b/c.txt")
        self.commit()
        first = json.dumps(collector.build_inventory(self.repo), sort_keys=True)
        second = json.dumps(collector.build_inventory(self.repo), sort_keys=True)
        self.assertEqual(first, second)


if __name__ == "__main__":
    unittest.main()
