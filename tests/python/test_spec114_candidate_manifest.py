from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "Experiments/spec114_candidate_manifest.py"


class Spec114CandidateManifestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.svs = self.root / "ndn-svs"
        self.svs.mkdir()
        subprocess.run(["git", "init", "-q"], cwd=self.svs, check=True)
        subprocess.run(["git", "config", "user.email", "spec114@example.invalid"], cwd=self.svs, check=True)
        subprocess.run(["git", "config", "user.name", "Spec 114"], cwd=self.svs, check=True)
        (self.svs / "source.cpp").write_text("v3\n", encoding="utf-8")
        subprocess.run(["git", "add", "."], cwd=self.svs, check=True)
        subprocess.run(["git", "commit", "-qm", "baseline"], cwd=self.svs, check=True)
        subprocess.run(["git", "branch", "-M", "Experimental"], cwd=self.svs, check=True)
        self.lock = self.root / "package-lock.json"
        self.lock.write_text('{"lockfileVersion":3}\n', encoding="utf-8")
        self.results = self.root / "results"

    def run_create(self, check: bool = True, attempt: str = "default") -> subprocess.CompletedProcess[str]:
        return subprocess.run([
            sys.executable, str(SCRIPT), "create", "--ndn-svs", str(self.svs),
            "--ndnts-lock", str(self.lock), "--output-root", str(self.results),
            "--campaign-attempt", attempt,
        ], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=check)

    def test_create_is_atomic_content_addressed_and_run_once(self) -> None:
        candidate = self.run_create().stdout.strip()
        self.assertRegex(candidate, r"^spec114-[0-9a-f]{20}$")
        manifest_path = self.results / candidate / "candidate-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(manifest["candidateId"], candidate)
        self.assertEqual(len(manifest["runOnce"]), 6)
        self.assertTrue(all(state == "pending" for state in manifest["runOnce"].values()))
        self.assertEqual(manifest["identity"]["interop"]["owner"], "NDNSF")
        self.assertTrue(manifest["identity"]["interop"]["ndntsSource"].endswith(".ts"))
        self.assertFalse((self.svs / "tests/interop").exists())
        repeated = self.run_create(check=False)
        self.assertNotEqual(repeated.returncode, 0)
        self.assertIn("already exists", repeated.stderr)

        inspected = subprocess.run([
            sys.executable, str(SCRIPT), "inspect", str(manifest_path),
        ], text=True, stdout=subprocess.PIPE, check=True)
        self.assertTrue(json.loads(inspected.stdout)["valid"])

        second = self.run_create(attempt="setup-recovery").stdout.strip()
        self.assertNotEqual(second, candidate)
        self.assertTrue((self.results / second / "candidate-manifest.json").is_file())

    def test_dirty_source_is_rejected(self) -> None:
        (self.svs / "source.cpp").write_text("dirty\n", encoding="utf-8")
        failed = self.run_create(check=False)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("clean", failed.stderr)

    def test_tampered_identity_is_rejected(self) -> None:
        candidate = self.run_create().stdout.strip()
        manifest_path = self.results / candidate / "candidate-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["identity"]["protocol"]["version"] = 2
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        failed = subprocess.run([
            sys.executable, str(SCRIPT), "inspect", str(manifest_path),
        ], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("digest mismatch", failed.stderr)


if __name__ == "__main__":
    unittest.main()
