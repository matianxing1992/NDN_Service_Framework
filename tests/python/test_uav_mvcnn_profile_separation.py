from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "NDNSF-UAV-APP/tools/run_multiview_fixture.py"
MANIFEST = ROOT / "NDNSF-UAV-APP/testdata/multiview-car/manifest.json"


class MvcnnProfileSeparationTest(unittest.TestCase):
    def _run(self, mode: str, profile: str = "") -> dict:
        with tempfile.TemporaryDirectory(prefix="uav-mvcnn-profile-") as directory:
            command = [sys.executable, str(RUNNER), "--manifest", str(MANIFEST),
                       "--mode", mode, "--views", "2", "--output", directory]
            if profile:
                command += ["--profile-id", profile]
            completed = subprocess.run(command, cwd=ROOT, text=True,
                                       capture_output=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            return json.loads((Path(directory) / "result.json").read_text(encoding="utf-8"))

    def test_real_and_functional_profiles_are_explicit(self) -> None:
        real = self._run("real")
        functional = self._run("functional")
        self.assertEqual(real["model"]["profileId"], "vehicle-mvcnn-v1")
        self.assertEqual(real["model"]["algorithmId"], "mvcnn-onnx-maxpool/v1")
        self.assertFalse(real["model"]["fallbackUsed"])
        self.assertEqual(functional["model"]["profileId"], "functional-adapter-v1")
        self.assertNotEqual(functional["model"]["algorithmId"], real["model"]["algorithmId"])
        self.assertFalse(functional["scientificAccuracyClaimAllowed"])

    def test_real_model_failure_does_not_switch_to_functional_adapter(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uav-mvcnn-no-fallback-") as directory:
            completed = subprocess.run(
                [sys.executable, str(RUNNER), "--manifest", str(MANIFEST),
                 "--mode", "real", "--views", "2", "--model", "/tmp/does-not-exist.onnx",
                 "--output", directory], cwd=ROOT, text=True,
                capture_output=True, check=False)
            self.assertNotEqual(completed.returncode, 0)
            self.assertNotIn("functional-adapter", completed.stderr)


if __name__ == "__main__":
    unittest.main()
