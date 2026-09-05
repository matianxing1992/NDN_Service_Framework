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


class MvcnnOnnxSecurityTest(unittest.TestCase):
    def test_duplicate_exact_data_is_counted_once(self) -> None:
        source = json.loads(MANIFEST.read_text(encoding="utf-8"))
        views = list(source["views"][:2])
        views.append(dict(views[0]))
        for view in views:
            view["file"] = str((MANIFEST.parent / view["file"]).resolve())
        source["views"] = views
        with tempfile.TemporaryDirectory(prefix="uav-mvcnn-duplicate-") as directory:
            root = Path(directory)
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps(source), encoding="utf-8")
            completed = subprocess.run(
                [sys.executable, str(RUNNER), "--manifest", str(manifest),
                 "--mode", "real", "--views", "3", "--output", str(root / "out")],
                cwd=ROOT, text=True, capture_output=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            result = json.loads((root / "out/result.json").read_text(encoding="utf-8"))
            self.assertEqual(result["model"]["acceptedViewCount"], 2)
            self.assertEqual(result["model"]["duplicateViewCount"], 1)
            self.assertEqual(len(result["annotatedViews"]), 2)

    def test_altered_view_digest_fails_before_inference(self) -> None:
        source = json.loads(MANIFEST.read_text(encoding="utf-8"))
        source["views"][0]["sha256"] = "0" * 64
        for view in source["views"]:
            view["file"] = str((MANIFEST.parent / view["file"]).resolve())
        with tempfile.TemporaryDirectory(prefix="uav-mvcnn-view-digest-") as directory:
            root = Path(directory)
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps(source), encoding="utf-8")
            completed = subprocess.run(
                [sys.executable, str(RUNNER), "--manifest", str(manifest),
                 "--mode", "real", "--views", "2", "--output", str(root / "out")],
                cwd=ROOT, text=True, capture_output=True, check=False)
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("digest", completed.stderr.lower())

    def test_annotations_are_provider_owned_and_real_path_has_no_fallback(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uav-mvcnn-owner-") as directory:
            completed = subprocess.run(
                [sys.executable, str(RUNNER), "--manifest", str(MANIFEST),
                 "--mode", "real", "--views", "2", "--provider", "/provider/cpu",
                 "--output", directory], cwd=ROOT, text=True,
                capture_output=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            result = json.loads((Path(directory) / "result.json").read_text(encoding="utf-8"))
            self.assertFalse(result["model"]["fallbackUsed"])
            self.assertTrue(all(item["signerIdentity"] == "/provider/cpu"
                                for item in result["annotatedViews"]))


if __name__ == "__main__":
    unittest.main()
