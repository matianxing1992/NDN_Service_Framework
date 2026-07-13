from __future__ import annotations

import json
from pathlib import Path
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[4]
CLI = REPO / "tools" / "ndnsf-di" / "ndnsf-di-itiger-qwen"


class OperatorCliTests(unittest.TestCase):
    def invoke(self, *args):
        return subprocess.run([str(CLI), *args], text=True, capture_output=True, check=False)

    def test_help_exposes_contract_command_tree(self):
        result = self.invoke("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        for command in ("discover", "release", "storage", "network", "candidate", "status", "wait", "cancel", "evidence", "aggregate", "cleanup"):
            self.assertIn(command, result.stdout)

    def test_discover_is_truthfully_blocked_not_fake_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "cluster.json"
            result = self.invoke("discover", "--output", str(output))
            self.assertEqual(result.returncode, 3)
            self.assertEqual(json.loads(output.read_text())["reasonCode"], "LIVE_CLUSTER_DISCOVERY_REQUIRED")

    def test_unsafe_release_manifest_returns_authority_exit(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = Path(tmp) / "release.json"
            manifest.write_text(json.dumps({"schemaVersion": "v1", "password": "bad"}))
            result = self.invoke("release", "validate", "--manifest", str(manifest))
            self.assertEqual(result.returncode, 7, result.stdout + result.stderr)

    def test_candidate_freeze_and_misuse_exit_codes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            profile = root / "candidate.json"
            output = root / "frozen.json"
            fields = ("sourceDigest", "runtimeReleaseDigest", "modelArtifactSetDigest", "identitySetDigest", "topologyPlacementDigest", "workloadDigest")
            profile.write_text(json.dumps({"bindingDigests": {name: "sha256:" + str(index) * 64 for index, name in enumerate(fields, 1)}}))
            passed = self.invoke("candidate", "freeze", "--profile", str(profile), "--output", str(output))
            self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)
            self.assertEqual(json.loads(output.read_text())["state"], "FROZEN")
            misuse = self.invoke("status", "--job-id", "1;id")
            self.assertEqual(misuse.returncode, 7)


if __name__ == "__main__":
    unittest.main()
