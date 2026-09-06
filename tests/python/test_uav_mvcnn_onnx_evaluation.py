from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PREPARE = ROOT / "NDNSF-UAV-APP/tools/prepare_coperception_uav.py"
EVALUATE = ROOT / "NDNSF-UAV-APP/tools/evaluate_multiview_recognition.py"


class MvcnnOnnxEvaluationTest(unittest.TestCase):
    def test_paired_report_contains_model_costs_and_mcnemar(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uav-mvcnn-eval-") as directory:
            root = Path(directory)
            for index in range(6):
                (root / f"view-{index}.ppm").write_text(
                    "P3\n2 2\n255\n255 0 0 255 0 0 0 0 0 0 0 0\n",
                    encoding="utf-8")
            (root / "samples.json").write_text(json.dumps({
                "source": "controlled-fixture", "synchronized": True,
                "calibration": {"intrinsics": "known", "poses": "known"},
                "ground_truth": {"type": "class-label"},
                "scientific_accuracy_claim_allowed": False,
                "samples": [{"sample_id": "s1", "target_id": "car-1",
                              "ground_truth_label": "car",
                              "views": [{"view_id": f"v{i}", "file": f"view-{i}.ppm",
                                         "capture_time_ms": i} for i in range(6)]}],
            }), encoding="utf-8")
            registration = root / "registration.json"
            prepared = subprocess.run(
                [sys.executable, str(PREPARE), "--source", str(root),
                 "--output", str(registration), "--license", "internal-test"],
                cwd=ROOT, text=True, capture_output=True, check=False)
            self.assertEqual(prepared.returncode, 0, prepared.stderr)
            output = root / "evaluation.json"
            evaluated = subprocess.run(
                [sys.executable, str(EVALUATE), "--registration", str(registration),
                 "--output", str(output), "--bootstrap-replicates", "100"],
                cwd=ROOT, text=True, capture_output=True, check=False)
            self.assertEqual(evaluated.returncode, 0, evaluated.stderr)
            report = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(set(report["metrics"]), {"1", "2", "4", "6"})
            for metrics in report["metrics"].values():
                self.assertIn("meanModelLatencyMs", metrics)
                self.assertIn("meanE2ELatencyMs", metrics)
                self.assertIn("meanPeakRssBytes", metrics)
                self.assertIn("meanConfidence", metrics)
                self.assertIn("macroF1", metrics)
                self.assertIn("failureCount", metrics)
            for uncertainty in report["pairedUncertainty"].values():
                self.assertIn("mcnemar", uncertainty)
                self.assertIn("latencyDeltaMsMultiMinusOneView", uncertainty)
            self.assertEqual(report["uncertainty"]["status"], "insufficient-samples")
            self.assertFalse(report["scientificAccuracyClaimAllowed"])


if __name__ == "__main__":
    unittest.main()
