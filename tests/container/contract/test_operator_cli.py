from __future__ import annotations

import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from contract._support import FIXTURES, REPO, valid_slurm_evidence


CLI = REPO / "packaging" / "ndnsf-di-container" / "bin" / "ndnsf-di-deploy"


class OperatorCliTest(unittest.TestCase):
    def run_cli(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([str(CLI), "--json", *args], text=True, capture_output=True, cwd=REPO, check=False)

    def test_validate_profile_json_contract(self) -> None:
        result = self.run_cli("validate-profile", "--profile", str(FIXTURES / "profiles" / "cloud-cpu-valid.yaml"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "PASS")

    def test_schema_failure_uses_exit_two_and_stderr(self) -> None:
        result = self.run_cli("validate-profile", "--profile", str(FIXTURES / "profiles" / "invalid" / "mixed-adapters.yaml"))
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertIn("PROFILE_INVALID", result.stderr)

    def test_verify_release_json_contract(self) -> None:
        result = self.run_cli("verify-release", "--manifest", str(FIXTURES / "releases" / "release-valid.json"))
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["releaseId"], "spec108-r1")
        self.assertRegex(payload["manifestDigest"], r"^sha256:[a-f0-9]{64}$")

    def test_verify_evidence_json_contract(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "evidence.json"
            path.write_text(json.dumps(valid_slurm_evidence()), encoding="utf-8")
            result = self.run_cli("verify-evidence", "--evidence", str(path))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["runId"], "itiger-rtx5000-001")

    def test_slurm_lifecycle_is_wired_and_missing_release_fails(self) -> None:
        result = self.run_cli("submit", "--profile", str(FIXTURES / "profiles" / "itiger-rtx5000-valid.yaml"))
        self.assertEqual(result.returncode, 2)
        self.assertIn("INPUT_READ_FAILED", result.stderr)

    def test_compose_preflight_is_wired_and_fails_before_daemon_on_missing_mount(self) -> None:
        result = self.run_cli("preflight", "--profile", str(FIXTURES / "profiles" / "cloud-cpu-valid.yaml"))
        self.assertEqual(result.returncode, 3)
        self.assertIn("COMPOSE_IDENTITY_MISSING", result.stderr)


if __name__ == "__main__":
    unittest.main()
