#!/usr/bin/env python3
"""Regression coverage for Spec 112 Targeted total-deadline behavior."""

from __future__ import annotations

import importlib
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest


REPO = Path(__file__).resolve().parents[2]


class Spec112TargetedTimeoutTest(unittest.TestCase):
    def test_sync_targeted_adapter_keeps_late_callbacks_off_the_call_stack(self) -> None:
        source = (REPO / "pythonWrapper/src/ndnsf/_ndnsf.cpp").read_text(
            encoding="utf-8"
        )
        method = source.split("PyServiceResponse\n  requestServiceTargeted(", 1)[1]
        method = method.split("\n  PyLargeDataPublishResult", 1)[0]

        self.assertIn("struct TargetedSyncState", method)
        self.assertIn("std::make_shared<TargetedSyncState>()", method)
        self.assertIn("terminalClaimed", method)
        self.assertIn("[this, providerName, serviceName, payload, timeoutMs, state]", method)
        self.assertNotIn("auto submit = [&, payload]", method)
        self.assertNotIn("[&](const ndn::Name& requestId)", method)
        self.assertNotIn("[&](const nsf::ResponseMessage& response)", method)

    def test_compiled_binding_exposes_sync_and_async_targeted_deadlines(self) -> None:
        native = importlib.import_module("ndnsf._ndnsf")
        self.assertEqual(Path(native.__file__).suffix, ".so")
        self.assertTrue(hasattr(native.NativeServiceUser, "request_service_targeted"))
        self.assertTrue(hasattr(native.NativeServiceUser, "request_service_targeted_async"))

    def test_async_targeted_cli_requires_targeted_mode(self) -> None:
        completed = subprocess.run(
            [
                sys.executable,
                str(REPO / "examples/python/segmented_response_user.py"),
                "--run-id", "spec112-timeout-cli",
                "--mode", "normal",
                "--targeted-api", "async",
            ],
            cwd=str(REPO),
            env={**os.environ, "PYTHONPATH": str(REPO / "pythonWrapper")},
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("--targeted-api async requires --mode targeted", completed.stderr)

    @unittest.skipUnless(
        os.environ.get("SPEC112_RUN_TARGETED_TIMEOUT") == "1",
        "set SPEC112_RUN_TARGETED_TIMEOUT=1 for exclusive MiniNDN evidence",
    )
    def test_compiled_sync_and_async_timeout_once_after_provider_degrades(self) -> None:
        manifest_value = os.environ.get("SPEC112_CANDIDATE_MANIFEST", "")
        self.assertTrue(manifest_value, "SPEC112_CANDIDATE_MANIFEST is required")
        manifest = Path(manifest_value).resolve()
        candidate = json.loads(manifest.read_text(encoding="utf-8"))
        candidate_dir = manifest.parent

        for targeted_api in ("sync", "async"):
            cell = candidate_dir / f"targeted-timeout-{targeted_api}"
            command = [
                "sudo", "-n", "-E",
                sys.executable,
                str(REPO / "Experiments/NDNSF_Segmented_Response_Minindn.py"),
                "--candidate-manifest", str(manifest),
                "--output-dir", str(cell),
                "--sizes", "64",
                "--mode", "targeted",
                "--targeted-api", targeted_api,
                "--fault-profile", "degraded-provider-after-targeted-bootstrap",
                "--timeout-ms", "1000",
                "--wall-timeout-s", "90",
                "--ownership-lock", f"/tmp/{candidate['candidateId']}-targeted-timeout.lock",
            ]
            completed = subprocess.run(
                command,
                cwd=str(REPO),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=150,
            )
            self.assertEqual(completed.returncode, 0, completed.stdout)
            summary = json.loads((cell / "cell-summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["candidateId"], candidate["candidateId"])
            self.assertEqual(summary["status"], "SUCCESS")
            results = summary["results"]
            self.assertEqual(len(results), 2)
            self.assertTrue(results[0]["ok"])
            self.assertFalse(results[1]["ok"])
            self.assertTrue(results[1]["deadlineWithinLimit"])
            self.assertEqual(results[1]["timeoutTerminalCount"], 1)
            self.assertEqual(results[1]["responseTerminalCount"], 0)
            self.assertEqual(results[1]["terminalCount"], 1)
            self.assertEqual(results[1]["targetedApi"], targeted_api)


if __name__ == "__main__":
    unittest.main()
