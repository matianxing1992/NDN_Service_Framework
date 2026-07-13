from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[4]
CLI = REPO / "tools" / "ndnsf-di" / "ndnsf-di-itiger-qwen"


def prepare_build_source(source: Path) -> None:
    target = source / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts"
    target.mkdir(parents=True)
    owner = REPO / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts"
    for name in ("rootless-build.sh", "inspect-oci-archive.py"):
        shutil.copy2(owner / name, target / name)


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

    def test_release_build_render_is_review_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            project = root / "project"
            output = project / "campaigns/spec110/rendered/build.sbatch"
            prepare_build_source(source)
            env = dict(os.environ, NDNSF_SPEC110_ALLOW_TEST_ROOT="1")
            result = subprocess.run(
                [str(CLI), "release", "build-render", "--source", str(source),
                 "--project", str(project), "--release-id", "probe-001",
                 "--output", str(output)],
                text=True, capture_output=True, check=False, env=env,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            value = json.loads(result.stdout)
            self.assertEqual(value["status"], "RENDERED_NOT_SUBMITTED")
            self.assertTrue(output.is_file())
            self.assertNotIn("sbatch ", output.read_text())

    def test_release_build_render_does_not_require_evidence_dependencies(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            project = root / "project"
            output = project / "campaigns/spec110/rendered/build.sbatch"
            prepare_build_source(source)
            env = dict(os.environ, NDNSF_SPEC110_ALLOW_TEST_ROOT="1")
            result = subprocess.run(
                ["python3", "-S", str(CLI), "release", "build-render",
                 "--source", str(source), "--project", str(project),
                 "--release-id", "no-site-probe", "--output", str(output)],
                text=True, capture_output=True, check=False, env=env,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(json.loads(result.stdout)["status"], "RENDERED_NOT_SUBMITTED")

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
