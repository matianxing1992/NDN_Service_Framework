from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PREPARE = ROOT / "NDNSF-UAV-APP/tools/prepare_coperception_uav.py"


class MvcnnDatasetRegistrationTest(unittest.TestCase):
    def test_registration_freezes_pairing_provenance_and_claim_boundary(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uav-mvcnn-registration-") as directory:
            root = Path(directory)
            for index in range(6):
                (root / f"view-{index}.ppm").write_text(
                    "P3\n2 2\n255\n255 0 0 255 0 0 0 0 0 0 0 0\n",
                    encoding="utf-8")
            (root / "samples.json").write_text(json.dumps({
                "source": "controlled-fixture",
                "synchronized": True,
                "calibration": {"intrinsics": "known", "poses": "known"},
                "ground_truth": {"type": "class-label"},
                "scientific_accuracy_claim_allowed": False,
                "preprocessing_policy": "fixed RGB crop",
                "sample_size_rationale": "qualification-only single target",
                "samples": [{"sample_id": "s1", "target_id": "car-1",
                              "ground_truth_label": "car",
                              "views": [{"view_id": f"v{i}", "file": f"view-{i}.ppm",
                                         "capture_time_ms": i} for i in range(6)]}],
            }), encoding="utf-8")
            output = root / "registration.json"
            completed = subprocess.run(
                [sys.executable, str(PREPARE), "--source", str(root),
                 "--output", str(output), "--license", "internal-test",
                 "--model-profile-id", "vehicle-mvcnn-v1"],
                cwd=ROOT, text=True, capture_output=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            registration = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(registration["modelProfileId"], "vehicle-mvcnn-v1")
            self.assertEqual(registration["viewCounts"], [1, 2, 4, 6])
            self.assertTrue(registration["sourceManifestDigest"].startswith("sha256:"))
            self.assertEqual(registration["sampleSizeRationale"], "qualification-only single target")
            self.assertFalse(registration["scientificAccuracyClaimAllowed"])
            self.assertEqual(len(registration["samples"][0]["views"]), 6)


if __name__ == "__main__":
    unittest.main()
