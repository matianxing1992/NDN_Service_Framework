#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = (
    ROOT
    / "packaging/ndnsf-di-container/oci/layered/scripts/prepare-layer-seals.py"
)


def load_module():
    spec = importlib.util.spec_from_file_location("prepare_layer_seals", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class LayerSealTests(unittest.TestCase):
    def setUp(self):
        self.module = load_module()
        self.temp = Path(tempfile.mkdtemp(prefix="spec158-seal-test."))

    def tearDown(self):
        shutil.rmtree(self.temp)

    def make_repo(self, name: str) -> Path:
        repo = self.temp / name
        repo.mkdir()
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        subprocess.run(["git", "-C", str(repo), "config", "user.email", "test@example.invalid"], check=True)
        subprocess.run(["git", "-C", str(repo), "config", "user.name", "Spec 158"], check=True)
        (repo / "source.cpp").write_text("one\n")
        subprocess.run(["git", "-C", str(repo), "add", "source.cpp"], check=True)
        subprocess.run(["git", "-C", str(repo), "commit", "-qm", "fixture"], check=True)
        return repo

    def test_workspace_snapshot_records_dirty_tracked_content(self):
        repo = self.make_repo("repo")
        (repo / "source.cpp").write_text("two\n")
        archive = self.temp / "repo.tar"
        row = self.module.snapshot_git_tree(repo, archive, [])
        self.assertTrue(row["dirty"])
        self.assertIn("source.cpp", row["dirtyPaths"])
        with tarfile.open(archive) as value:
            self.assertEqual(value.extractfile("source.cpp").read(), b"two\n")

    def test_workspace_snapshot_includes_allowlisted_untracked_source(self):
        repo = self.make_repo("repo")
        (repo / "owned").mkdir()
        (repo / "owned/new.cpp").write_text("new\n")
        (repo / "outside.txt").write_text("not owned\n")
        archive = self.temp / "repo.tar"
        row = self.module.snapshot_git_tree(repo, archive, [], ["owned"])
        self.assertTrue(row["dirty"])
        self.assertEqual(row["dirtyPaths"], ["owned/new.cpp"])
        self.assertEqual(row["trackedPathCount"], 0)
        self.assertEqual(row["untrackedPathCount"], 1)
        with tarfile.open(archive) as value:
            self.assertEqual(value.getnames(), ["owned/new.cpp"])
            self.assertEqual(value.extractfile("owned/new.cpp").read(), b"new\n")

    def test_workspace_snapshot_excludes_untracked_build_and_secret_files(self):
        repo = self.make_repo("repo")
        (repo / "owned").mkdir()
        (repo / "owned/new.py").write_text("safe = True\n")
        (repo / "owned/secret.key").write_text("secret\n")
        (repo / "owned/build").mkdir()
        (repo / "owned/build/cache.py").write_text("cache\n")
        archive = self.temp / "repo.tar"
        row = self.module.snapshot_git_tree(
            repo, archive, ["build", "*.key"], ["owned"])
        self.assertEqual(row["dirtyPaths"], ["owned/new.py"])
        self.assertEqual(row["untrackedPathCount"], 1)
        with tarfile.open(archive) as value:
            self.assertEqual(value.getnames(), ["owned/new.py"])

    def test_verify_rejects_lock_drift_and_archive_tamper(self):
        repo = self.make_repo("repo")
        lock = self.temp / "lock.json"
        lock.write_text('{"schemaVersion":"ndnsf-di-layer-lock-v1"}\n')
        output = self.temp / "seal"
        (output / "archives").mkdir(parents=True)
        row = self.module.snapshot_git_tree(repo, output / "archives/repo.tar", [])
        manifest = {
            "schemaVersion": self.module.SCHEMA,
            "layer": "app-runtime",
            "lockDigest": self.module.sha256(lock),
            "sources": {"repo": row},
        }
        manifest["sealDigest"] = self.module.body_digest(manifest)
        (output / "seal.json").write_text(json.dumps(manifest))
        self.assertEqual(self.module.verify(lock, output), manifest["sealDigest"])
        lock.write_text('{"changed":true}\n')
        with self.assertRaisesRegex(self.module.SealError, "LOCK_MISMATCH"):
            self.module.verify(lock, output)

    def test_archive_path_traversal_is_rejected(self):
        archive = self.temp / "unsafe.tar"
        with tarfile.open(archive, "w") as value:
            info = tarfile.TarInfo("../escape")
            info.size = 0
            value.addfile(info)
        with self.assertRaisesRegex(self.module.SealError, "ARCHIVE_UNSAFE"):
            self.module.validate_archive(archive)

    def test_workspace_allowlist_excludes_unowned_tracked_content(self):
        repo = self.make_repo("repo")
        (repo / "owned").mkdir()
        (repo / "owned/source.cpp").write_text("owned\n")
        (repo / "large.bin").write_bytes(b"x" * 1024)
        subprocess.run(
            ["git", "-C", str(repo), "add", "owned/source.cpp", "large.bin"],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(repo), "commit", "-qm", "more fixtures"], check=True
        )
        archive = self.temp / "repo.tar"
        self.module.snapshot_git_tree(repo, archive, [], ["owned"])
        with tarfile.open(archive) as value:
            self.assertEqual(value.getnames(), ["owned/source.cpp"])

    def test_workspace_allowlist_excludes_unowned_dirty_paths_from_identity(self):
        repo = self.make_repo("repo")
        (repo / "owned").mkdir()
        (repo / "owned/source.cpp").write_text("owned\n")
        (repo / "outside.txt").write_text("before\n")
        subprocess.run(
            [
                "git", "-C", str(repo), "add",
                "owned/source.cpp", "outside.txt",
            ],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(repo), "commit", "-qm", "owned fixture"],
            check=True,
        )
        (repo / "outside.txt").write_text("after\n")
        archive = self.temp / "repo.tar"
        row = self.module.snapshot_git_tree(repo, archive, [], ["owned"])
        self.assertFalse(row["dirty"])
        self.assertEqual(row["dirtyPaths"], [])

    def test_seal_digest_ignores_creation_timestamp(self):
        first = {"schemaVersion": self.module.SCHEMA, "createdAt": "one", "sources": {"x": {}}}
        second = {"schemaVersion": self.module.SCHEMA, "createdAt": "two", "sources": {"x": {}}}
        self.assertEqual(
            self.module.body_digest(first), self.module.body_digest(second)
        )
        self.assertEqual(
            self.module.NORMALIZED_CREATED_AT,
            "1970-01-01T00:00:00+00:00",
        )


if __name__ == "__main__":
    unittest.main()
