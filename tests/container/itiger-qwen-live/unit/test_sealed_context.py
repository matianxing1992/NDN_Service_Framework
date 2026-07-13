from __future__ import annotations

import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[4]
SCRIPT = REPO / "packaging/ndnsf-di-container/oci/scripts/prepare-sealed-context.py"


def git(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args], text=True, capture_output=True, check=False
    )
    if result.returncode:
        raise AssertionError(result.stderr)
    return result.stdout.strip()


def make_repo(path: Path, marker: str) -> str:
    path.mkdir(parents=True)
    git(path, "init", "-q")
    git(path, "config", "user.email", "test@example.invalid")
    git(path, "config", "user.name", "test")
    (path / "source.txt").write_text(marker + "\n", encoding="utf-8")
    git(path, "add", "source.txt")
    git(path, "commit", "-qm", "initial")
    return git(path, "rev-parse", "HEAD")


class SealedContextTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.workspace = self.root / "workspace"
        self.dependencies = self.root / "dependencies"
        make_repo(self.workspace, "workspace")
        revisions = {
            name: make_repo(self.dependencies / name, name)
            for name in ("NFD", "ndn-svs")
        }
        self.lock = self.workspace / "gpu.lock"
        self.lock.write_text(
            json.dumps(
                {
                    "schemaVersion": "ndnsf-di-gpu-lock-v1",
                    "sourceRepositories": {
                        name: {"url": str(self.dependencies / name), "revision": revision}
                        for name, revision in revisions.items()
                    },
                }
            ),
            encoding="utf-8",
        )
        git(self.workspace, "add", "gpu.lock")
        git(self.workspace, "commit", "-qm", "lock")
        self.output = self.root / "sealed"

    def run_tool(self, action: str, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                "python3", str(SCRIPT), action,
                "--workspace", str(self.workspace),
                "--lock", str(self.lock),
                "--output", str(self.output),
                *extra,
            ],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_create_and_verify_exact_dependency_archives(self) -> None:
        created = self.run_tool(
            "create", "--dependency-root", str(self.dependencies),
            "--work-root", str(self.root / "fetch"),
        )
        self.assertEqual(created.returncode, 0, created.stderr)
        value = json.loads((self.output / "source-seal.json").read_text())
        self.assertEqual(value["schemaVersion"], "spec110-oci-source-seal-v1")
        self.assertEqual(set(value["dependencies"]), {"NFD", "ndn-svs"})
        for name, row in value["dependencies"].items():
            archive = self.output / "archives" / f"{name}.tar"
            self.assertEqual(row["archiveBytes"], archive.stat().st_size)
            self.assertEqual(
                row["archiveDigest"],
                "sha256:" + hashlib.sha256(archive.read_bytes()).hexdigest(),
            )
        verified = self.run_tool("verify")
        self.assertEqual(verified.returncode, 0, verified.stderr)
        self.assertEqual(verified.stdout.strip(), value["sealDigest"])

    def test_unreachable_locked_revision_fails_closed(self) -> None:
        value = json.loads(self.lock.read_text())
        value["sourceRepositories"]["NFD"]["revision"] = "0" * 40
        self.lock.write_text(json.dumps(value), encoding="utf-8")
        result = self.run_tool("create", "--dependency-root", str(self.dependencies))
        self.assertEqual(result.returncode, 4)
        self.assertIn("SOURCE_SEAL_REVISION_MISMATCH:NFD", result.stderr)

    def test_archive_and_lock_tamper_fail_closed(self) -> None:
        self.assertEqual(
            self.run_tool("create", "--dependency-root", str(self.dependencies)).returncode,
            0,
        )
        archive = self.output / "archives/NFD.tar"
        archive.write_bytes(archive.read_bytes() + b"tamper")
        result = self.run_tool("verify")
        self.assertEqual(result.returncode, 4)
        self.assertIn("SOURCE_SEAL_ARCHIVE_MISMATCH:NFD", result.stderr)

        archive.write_bytes(archive.read_bytes()[:-6])
        self.lock.write_text(self.lock.read_text() + "\n", encoding="utf-8")
        result = self.run_tool("verify")
        self.assertEqual(result.returncode, 4)
        self.assertIn("SOURCE_SEAL_LOCK_MISMATCH", result.stderr)

    def test_manifest_shape_tamper_fails_closed(self) -> None:
        self.assertEqual(
            self.run_tool("create", "--dependency-root", str(self.dependencies)).returncode,
            0,
        )
        manifest = self.output / "source-seal.json"
        value = json.loads(manifest.read_text())
        value["dependencies"]["NFD"] = "invalid"
        body = dict(value)
        body.pop("sealDigest")
        value["sealDigest"] = "sha256:" + hashlib.sha256(
            json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
        ).hexdigest()
        manifest.write_text(json.dumps(value), encoding="utf-8")
        result = self.run_tool("verify")
        self.assertEqual(result.returncode, 4)
        self.assertIn("SOURCE_SEAL_DEPENDENCY_INVALID:NFD", result.stderr)

    def test_dependency_archive_does_not_hydrate_lfs_content(self) -> None:
        dependency = self.dependencies / "NFD"
        (dependency / ".gitattributes").write_text("*.bin filter=lfs\n", encoding="utf-8")
        (dependency / "artifact.bin").write_text("pointer-content\n", encoding="utf-8")
        git(dependency, "add", ".gitattributes", "artifact.bin")
        git(dependency, "commit", "-qm", "add lfs pointer")
        git(dependency, "config", "filter.lfs.smudge", "sed s/pointer/hydrated/")
        value = json.loads(self.lock.read_text())
        value["sourceRepositories"]["NFD"]["revision"] = git(
            dependency, "rev-parse", "HEAD"
        )
        self.lock.write_text(json.dumps(value), encoding="utf-8")
        git(self.workspace, "add", "gpu.lock")
        git(self.workspace, "commit", "-qm", "update lock")
        result = self.run_tool("create", "--dependency-root", str(self.dependencies))
        self.assertEqual(result.returncode, 0, result.stderr)
        committed = subprocess.check_output(
            ["git", "-C", str(dependency), "show", "HEAD:artifact.bin"]
        )
        with tarfile.open(self.output / "archives/NFD.tar", "r:") as archive:
            stream = archive.extractfile("artifact.bin")
            self.assertIsNotNone(stream)
            archived = stream.read()
            self.assertEqual(archived, committed)
            self.assertNotIn(b"hydrated-content", archived)

    def test_unsafe_tar_member_is_rejected(self) -> None:
        spec = importlib.util.spec_from_file_location("prepare_sealed_context", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        archive = self.root / "unsafe.tar"
        with tarfile.open(archive, "w") as stream:
            info = tarfile.TarInfo("../escape")
            payload = b"bad"
            info.size = len(payload)
            stream.addfile(info, io.BytesIO(payload))
        with self.assertRaisesRegex(module.SealError, "SOURCE_SEAL_ARCHIVE_UNSAFE"):
            module.validate_archive(archive)


if __name__ == "__main__":
    unittest.main()
