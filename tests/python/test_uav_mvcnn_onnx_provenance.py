from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PREPARE = ROOT / "NDNSF-UAV-APP/tools/prepare_mvcnn_artifact.py"


class MvcnnArtifactProvenanceTest(unittest.TestCase):
    def test_reproducible_manifest_binds_checkpoint_and_onnx(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uav-mvcnn-artifact-") as directory:
            completed = subprocess.run(
                [sys.executable, str(PREPARE), "--output-dir", directory],
                cwd=ROOT, text=True, capture_output=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            root = Path(directory)
            manifest = json.loads((root / "mvcnn_vehicle_cpu.manifest.json").read_text())
            self.assertEqual(manifest["license"], "MIT")
            self.assertEqual(manifest["expectedExecutionProvider"], "CPUExecutionProvider")
            self.assertEqual(manifest["onnxChecker"], "passed")
            self.assertEqual(manifest["nativeQualification"]["nativeAccuracy"], 1.0)
            for key, filename in (("checkpoint", "mvcnn_vehicle_cpu.pt"),
                                  ("artifact", "mvcnn_vehicle_cpu.onnx")):
                digest = hashlib.sha256((root / filename).read_bytes()).hexdigest()
                self.assertEqual(manifest[key]["digest"], "sha256:" + digest)


if __name__ == "__main__":
    unittest.main()

