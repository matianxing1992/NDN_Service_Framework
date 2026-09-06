from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from uav_mvcnn_inputs import prepared_mvcnn_arguments


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "NDNSF-UAV-APP/tools/run_multiview_fixture.py"
MANIFEST = ROOT / "NDNSF-UAV-APP/testdata/multiview-car/manifest.json"


class MvcnnOnnxInferenceTest(unittest.TestCase):
    def run_real(self, count: int, *extra: str) -> dict:
        with tempfile.TemporaryDirectory(prefix="uav-mvcnn-real-") as directory:
            result = subprocess.run(
                [sys.executable, str(RUNNER), *prepared_mvcnn_arguments(directory), "--manifest", str(MANIFEST),
                 "--mode", "real", "--views", str(count), "--output", directory, *extra],
                cwd=ROOT, text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads((Path(directory) / "result.json").read_text())

    def test_joint_cpu_inference_for_2_4_6_views(self) -> None:
        for count in (2, 4, 6):
            value = self.run_real(count)
            self.assertEqual(value["schema"], "ndnsf-uav-multiview-result/v2")
            self.assertEqual(value["status"], "completed")
            self.assertEqual(value["model"]["executionProviders"], ["CPUExecutionProvider"])
            self.assertEqual(value["model"]["acceptedViewCount"], count)
            self.assertEqual(value["fusionEvidence"]["consumedViewCount"], count)
            self.assertEqual(len(value["annotatedViews"]), count)
            self.assertFalse(value["scientificAccuracyClaimAllowed"])
            self.assertEqual(value["fusedDecision"]["label"], "car")

    def test_one_view_requires_explicit_paired_baseline(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uav-mvcnn-one-") as directory:
            failed = subprocess.run(
                [sys.executable, str(RUNNER), *prepared_mvcnn_arguments(directory), "--manifest", str(MANIFEST),
                 "--mode", "real", "--views", "1", "--output", directory],
                cwd=ROOT, text=True, capture_output=True, check=False)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("insufficient-views", failed.stdout)

    def test_model_digest_mismatch_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uav-mvcnn-digest-") as directory:
            failed = subprocess.run(
                [sys.executable, str(RUNNER), *prepared_mvcnn_arguments(directory), "--manifest", str(MANIFEST),
                 "--mode", "real", "--views", "2", "--output", directory,
                 "--model-digest", "sha256:" + "0" * 64],
                cwd=ROOT, text=True, capture_output=True, check=False)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("digest", failed.stderr.lower())


if __name__ == "__main__":
    unittest.main()
