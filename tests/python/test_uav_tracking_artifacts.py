from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Experiments/UAV/tracking_artifacts.py"


class TrackingArtifactTest(unittest.TestCase):
    def test_functional_fixture_is_explicitly_not_real_data(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "passport.json"
            result = subprocess.run([sys.executable, str(SCRIPT), "--functional-fixture",
                                     "--output", str(output)], text=True,
                                    capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            passport = json.loads(output.read_text())
            self.assertEqual(passport["mode"], "functional-test-only")
            self.assertFalse(passport["scientificAccuracyClaimAllowed"])

    def test_real_gate_requires_three_named_videos_and_license(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "model.onnx"
            model.write_bytes(b"model")
            result = subprocess.run([sys.executable, str(SCRIPT), "--source", str(root),
                                     "--model", str(model), "--license", "Apache-2.0"],
                                    text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertIn("missing or empty video", result.stderr)

    def test_functional_passport_digests_all_views(self) -> None:
        import importlib.util
        spec = importlib.util.spec_from_file_location("tracking_artifacts", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        passport = module.functional_passport()
        self.assertTrue(passport["views"])
        self.assertTrue(all(item["digest"].startswith("sha256:") for item in passport["views"]))


if __name__ == "__main__":
    unittest.main()
