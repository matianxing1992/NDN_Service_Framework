#!/usr/bin/env python3
"""Regression checks for the self-contained Spec174 exact-SIF bundle."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PREPARE = ROOT / "tools/spec174_prepare_exact_bundle.py"


class Spec174ExactBundleTest(unittest.TestCase):
    def test_bundle_has_runtime_root_contract_and_current_wire_bounds(self) -> None:
        with tempfile.TemporaryDirectory(prefix="spec174-bundle-test-") as temp:
            output = Path(temp) / "bundle"
            result = subprocess.run(
                [sys.executable, str(PREPARE), "--output", str(output)],
                cwd=ROOT,
                check=True,
                text=True,
                capture_output=True,
            )
            self.assertIn('"status": "PASS"', result.stdout)
            manifest = json.loads(
                (output / "bundle-manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest["schema"], "spec174-exact-sif-bundle-v1")
            self.assertEqual(
                [(item["scope"], item["expectedBytes"])
                 for item in manifest["dependencyBounds"]],
                [
                    ("backbone-to-head0", 120),
                    ("backbone-to-head1", 120),
                    ("head0-to-merge", 32),
                    ("head1-to-merge", 32),
                ],
            )
            for relative in (
                "controller.policies",
                "trust-schema.conf",
                "native-execution-plan.json",
                "service-manifest.json",
                "user_driver.py",
                "spec170-d0-current-sif-workload.sh",
                "spec170-d1-current-sif-workload.sh",
                "artifacts/qwen-native-tracer-backbone.onnx",
                "artifacts/qwen-native-tracer-head0.onnx",
                "artifacts/qwen-native-tracer-head1.onnx",
                "artifacts/qwen-native-tracer-merge.onnx",
            ):
                self.assertTrue((output / relative).is_file(), relative)

    def test_bundle_refuses_overwrite(self) -> None:
        with tempfile.TemporaryDirectory(prefix="spec174-bundle-test-") as temp:
            output = Path(temp) / "bundle"
            output.mkdir()
            result = subprocess.run(
                [sys.executable, str(PREPARE), "--output", str(output)],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("refusing to overwrite", result.stderr)


if __name__ == "__main__":
    unittest.main()
