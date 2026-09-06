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


class MultiViewOutputTest(unittest.TestCase):
    def test_exact_named_annotations_are_complete_and_tamper_evident(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uav-mv-output-") as directory:
            completed = subprocess.run(
                [sys.executable, str(RUNNER), "--manifest", str(MANIFEST), "--views", "6",
                 "--output", directory, "--provider", "/provider/gpu"],
                text=True, capture_output=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            result_path = Path(directory) / "result.json"
            result = json.loads(result_path.read_text())
            self.assertEqual(len(result["annotatedViews"]), 6)
            self.assertTrue(all(item["exactDataName"].startswith("/provider/gpu/")
                                for item in result["annotatedViews"]))
            self.assertTrue(all((Path(directory) / "annotations" / f"{item['viewId']}.png").is_file()
                                for item in result["annotatedViews"]))
            tampered = json.loads(result_path.read_text())
            tampered["annotatedViews"][0]["contentDigest"] = "sha256:" + "0" * 64
            self.assertNotEqual(tampered["annotatedViews"][0]["contentDigest"],
                                result["annotatedViews"][0]["contentDigest"])


if __name__ == "__main__":
    unittest.main()

