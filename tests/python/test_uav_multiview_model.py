from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "NDNSF-UAV-APP" / "tools" / "run_multiview_fixture.py"
MANIFEST = ROOT / "NDNSF-UAV-APP" / "testdata" / "multiview-car" / "manifest.json"


class MultiViewModelTest(unittest.TestCase):
    def run_views(self, count: int, minimum: int = 2) -> dict:
        with tempfile.TemporaryDirectory(prefix="uav-mv-test-") as directory:
            command = [sys.executable, str(RUNNER), "--manifest", str(MANIFEST),
                       "--views", str(count), "--minimum-views", str(minimum),
                       "--output", directory]
            completed = subprocess.run(command, text=True, capture_output=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            return json.loads((Path(directory) / "result.json").read_text())

    def test_joint_pooling_and_annotations_for_2_4_6_views(self) -> None:
        results = {count: self.run_views(count) for count in (2, 4, 6)}
        for count, result in results.items():
            self.assertEqual(result["status"], "completed")
            self.assertEqual(result["fusionEvidence"]["consumedViewCount"], count)
            self.assertEqual(len(result["annotatedViews"]), count)
            self.assertFalse(result["scientificAccuracyClaimAllowed"])
        self.assertNotEqual(results[2]["fusionEvidence"]["pooledFeatureDigest"],
                            results[4]["fusionEvidence"]["pooledFeatureDigest"])
        self.assertNotEqual(results[4]["fusionEvidence"]["pooledFeatureDigest"],
                            results[6]["fusionEvidence"]["pooledFeatureDigest"])

    def test_one_view_is_explicit_baseline_not_multiview_success(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uav-mv-test-") as directory:
            completed = subprocess.run(
                [sys.executable, str(RUNNER), "--manifest", str(MANIFEST), "--views", "1",
                 "--output", directory], text=True, capture_output=True, check=False)
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("insufficient-views", completed.stdout)


if __name__ == "__main__":
    unittest.main()

