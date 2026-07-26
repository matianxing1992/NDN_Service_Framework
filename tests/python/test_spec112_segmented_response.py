#!/usr/bin/env python3
"""Real compiled-binding acceptance for Spec 112 segmented responses.

The heavy test is opt-in because it owns global MiniNDN state. T020 invokes it
with one candidate manifest after the rebuilt ndn-svs library and NDNSF Python
extension have been installed. No fake native object is accepted here.
"""

from __future__ import annotations

import importlib
import json
import os
import subprocess
import sys
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
SIZES = (64, 4000, 5000, 6500, 8000, 16000)


class Spec112CompiledBindingSegmentedResponseTest(unittest.TestCase):
    def test_loaded_ndnsf_module_is_compiled_extension(self) -> None:
        sys.path.insert(0, str(REPO / "pythonWrapper"))
        try:
            native = importlib.import_module("ndnsf._ndnsf")
        finally:
            sys.path.pop(0)
        module_path = Path(native.__file__).resolve()
        self.assertEqual(module_path.suffix, ".so")
        self.assertTrue(module_path.is_file())
        self.assertGreater(module_path.stat().st_size, 0)

    @unittest.skipUnless(
        os.environ.get("SPEC112_RUN_MININDN") == "1",
        "set SPEC112_RUN_MININDN=1 for the exclusive MiniNDN acceptance",
    )
    def test_normal_and_targeted_are_byte_exact_under_both_publish_modes(self) -> None:
        manifest_value = os.environ.get("SPEC112_CANDIDATE_MANIFEST", "")
        self.assertTrue(manifest_value, "SPEC112_CANDIDATE_MANIFEST is required")
        manifest = Path(manifest_value).resolve()
        self.assertTrue(manifest.is_file())
        candidate_dir = manifest.parent
        candidate = json.loads(manifest.read_text(encoding="utf-8"))
        candidate_id = candidate["candidateId"]
        self.assertEqual(candidate_dir.name, candidate_id)

        for publish_mode in ("async", "sync"):
            for invocation_mode in ("normal", "targeted"):
                cell = candidate_dir / f"focused-{publish_mode}-{invocation_mode}"
                command = [
                    sys.executable,
                    str(REPO / "Experiments/NDNSF_Segmented_Response_Minindn.py"),
                    "--candidate-manifest", str(manifest),
                    "--output-dir", str(cell),
                    "--sizes", ",".join(map(str, SIZES)),
                    "--mode", invocation_mode,
                    "--wall-timeout-s", "180",
                ]
                if publish_mode == "sync":
                    command.append("--svs-sync-publish")
                completed = subprocess.run(
                    command,
                    cwd=str(REPO),
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    check=False,
                    timeout=240,
                )
                self.assertEqual(
                    completed.returncode,
                    0,
                    msg=f"{cell.name} failed:\n{completed.stdout}",
                )
                summary = json.loads((cell / "cell-summary.json").read_text(encoding="utf-8"))
                self.assertEqual(summary["candidateId"], candidate_id)
                self.assertEqual(summary["status"], "SUCCESS")
                self.assertEqual(summary["requestedSizes"], list(SIZES))
                self.assertEqual(summary["passed"], len(SIZES))
                self.assertTrue(summary["noReferenceProof"]["verified"])
                self.assertTrue(summary["providerAlive"])


if __name__ == "__main__":
    unittest.main()
