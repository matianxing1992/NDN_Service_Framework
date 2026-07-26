from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "Experiments/spec112_candidate_manifest.py"


def run(*args: str, cwd: Path | None = None, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [*args],
        cwd=cwd,
        check=check,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


class Spec112CandidateManifestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.framework = self.root / "ndn-service-framework"
        self.svs = self.root / "ndn-svs"
        self.nac = self.root / "NAC-ABE"
        for repository in (self.framework, self.svs, self.nac):
            repository.mkdir()
            run("git", "init", "-q", cwd=repository)
            run("git", "config", "user.email", "spec112@example.invalid", cwd=repository)
            run("git", "config", "user.name", "Spec 112", cwd=repository)
            (repository / "tracked.txt").write_text("baseline\n", encoding="utf-8")
            run("git", "add", "tracked.txt", cwd=repository)
            run("git", "commit", "-qm", "baseline", cwd=repository)

        # Spec 113 binds the clean review head and permanent recovery snapshot
        # without moving the final local labels before MiniNDN validation.
        run("git", "branch", "Experimental", cwd=self.svs)
        run("git", "branch", "review/ndn-svs-convergence", cwd=self.svs)
        run("git", "branch", "backup/experimental-before-convergence-20260715", cwd=self.svs)
        run("git", "update-ref", "refs/remotes/origin/master", "HEAD", cwd=self.svs)
        run("git", "remote", "add", "origin", "https://example.invalid/ndn-svs.git", cwd=self.svs)

        (self.framework / "build").mkdir()
        (self.framework / "build/unit-tests").write_bytes(b"framework-binary")
        (self.svs / "build").mkdir()
        (self.svs / "build/unit-tests").write_bytes(b"svs-binary")
        (self.nac / "build").mkdir()
        (self.nac / "build/unit-tests").write_bytes(b"nac-binary")
        (self.nac / "build/libnac-abe.so").write_bytes(b"nac-library")
        self.results = self.root / "results"

    def create(self, *, check: bool = True) -> subprocess.CompletedProcess[str]:
        return run(
            sys.executable,
            str(SCRIPT),
            "create",
            "--result-root",
            str(self.results),
            "--repo-root",
            str(self.framework),
            check=check,
        )

    def test_create_writes_one_atomic_manifest_and_prints_only_candidate_id(self) -> None:
        completed = self.create()
        candidate_id = completed.stdout.strip()
        self.assertRegex(candidate_id, r"^spec112-[0-9a-f]{20}$")
        self.assertEqual(completed.stdout, candidate_id + "\n")
        self.assertEqual(completed.stderr, "")

        manifest_path = self.results / candidate_id / "candidate-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(manifest["candidateId"], candidate_id)
        self.assertTrue(manifest["identitySha256"].startswith(candidate_id[len("spec112-"):]))
        self.assertEqual([item["name"] for item in manifest["identity"]["repositories"]], [
            "ndn-service-framework",
            "ndn-svs",
            "NAC-ABE",
        ])
        self.assertTrue(manifest["identity"]["binaries"])
        topology = manifest["identity"]["ndnSvsTopology"]
        self.assertEqual(topology["expectedFinal"]["masterOid"], topology["originMaster"]["oid"])
        self.assertEqual(
            topology["expectedFinal"]["experimentalOid"], topology["validatedReviewHead"]["oid"])
        self.assertEqual(topology["expectedFinal"]["backupOid"], topology["permanentBackup"]["oid"])
        self.assertEqual(topology["creationLabels"]["master"]["state"], "present")
        self.assertEqual(topology["creationLabels"]["Experimental"]["state"], "present")
        self.assertEqual(len(manifest["identity"]["declaredMiniNdnCells"]), 8)
        self.assertEqual(
            {cell["cellId"] for cell in manifest["identity"]["declaredMiniNdnCells"]},
            {
                "boundary-async-normal",
                "boundary-async-targeted",
                "boundary-sync-normal",
                "boundary-sync-targeted",
                "burst-async-normal",
                "targeted-degraded-timeout",
                "targeted-degraded-timeout-async",
                "rollback-v2-boundary",
            },
        )
        self.assertTrue(manifest["identity"]["campaignInputs"])
        self.assertIn("dependencyInstallation", manifest["identity"])
        self.assertIn("ndnSvs", manifest["identity"]["dependencyInstallation"])

        repeated = self.create(check=False)
        self.assertNotEqual(repeated.returncode, 0)
        self.assertEqual(repeated.stdout, "")
        self.assertIn("already exists", repeated.stderr)

    def test_tracked_dirty_diff_changes_candidate(self) -> None:
        first = self.create().stdout.strip()
        (self.svs / "tracked.txt").write_text("changed\n", encoding="utf-8")
        second = self.create().stdout.strip()
        self.assertNotEqual(first, second)

    def test_relevant_untracked_content_changes_candidate(self) -> None:
        (self.framework / "diagnostic.py").write_text("VALUE = 1\n", encoding="utf-8")
        first = self.create().stdout.strip()
        (self.framework / "diagnostic.py").write_text("VALUE = 2\n", encoding="utf-8")
        second = self.create().stdout.strip()
        self.assertNotEqual(first, second)

    def test_generated_local_configuration_does_not_change_candidate(self) -> None:
        first = self.create().stdout.strip()
        private = self.framework / ".planning"
        private.mkdir()
        (private / "settings.local.json").write_text('{"local":true}\n', encoding="utf-8")
        repeated = self.create(check=False)
        self.assertNotEqual(repeated.returncode, 0)
        self.assertIn("already exists", repeated.stderr)
        self.assertTrue((self.results / first / "candidate-manifest.json").is_file())

    def test_built_binary_change_changes_candidate(self) -> None:
        first = self.create().stdout.strip()
        (self.svs / "build/unit-tests").write_bytes(b"different-svs-binary")
        second = self.create().stdout.strip()
        self.assertNotEqual(first, second)

    def test_existing_candidate_directory_with_wrong_manifest_is_never_overwritten(self) -> None:
        candidate_id = self.create().stdout.strip()
        manifest_path = self.results / candidate_id / "candidate-manifest.json"
        manifest_path.write_text('{"candidateId":"tampered"}\n', encoding="utf-8")
        failed = self.create(check=False)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("collision", failed.stderr.lower())
        self.assertEqual(manifest_path.read_text(encoding="utf-8"), '{"candidateId":"tampered"}\n')


if __name__ == "__main__":
    unittest.main()
