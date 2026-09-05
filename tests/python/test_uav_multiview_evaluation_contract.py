from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PREPARE = ROOT / "NDNSF-UAV-APP" / "tools" / "prepare_coperception_uav.py"
EVALUATE = ROOT / "NDNSF-UAV-APP" / "tools" / "evaluate_multiview_recognition.py"


class MultiViewEvaluationContractTest(unittest.TestCase):
    def test_registration_requires_control_metadata_and_evaluator_is_paired(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uav-mv-dataset-") as directory:
            root = Path(directory)
            # PPM is text and deterministic, keeping this contract test free of
            # binary fixtures while still exercising real image decoding.
            for index in range(6):
                (root / f"view-{index}.ppm").write_text("P3\n2 2\n255\n255 0 0 255 0 0 0 0 0 0 0 0\n")
            (root / "samples.json").write_text(json.dumps({
                "source": "controlled-synthetic-render-v1",
                "synchronized": True,
                "calibration": {"intrinsics": "known", "poses": "known"},
                "ground_truth": {"type": "class-label"},
                "scientific_accuracy_claim_allowed": False,
                "samples": [{"sample_id": "s1", "target_id": "car-1",
                              "ground_truth_label": "car",
                              "views": [{"view_id": f"v{i}", "file": f"view-{i}.ppm",
                                         "capture_time_ms": i} for i in range(6)]}],
            }))
            registration = root / "registration.json"
            completed = subprocess.run(
                [sys.executable, str(PREPARE), "--source", str(root),
                 "--output", str(registration), "--license", "internal-test"],
                text=True, capture_output=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            value = json.loads(registration.read_text())
            self.assertEqual(value["schema"], "ndnsf-uav-multiview-dataset-registration/v1")
            output = root / "evaluation.json"
            completed = subprocess.run(
                [sys.executable, str(EVALUATE), "--registration", str(registration),
                 "--output", str(output)], text=True, capture_output=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(output.read_text())
            self.assertEqual(set(report["metrics"]), {"1", "2", "4", "6"})
            self.assertFalse(report["scientificAccuracyClaimAllowed"])


if __name__ == "__main__":
    unittest.main()

