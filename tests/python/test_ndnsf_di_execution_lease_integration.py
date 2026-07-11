from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
HARNESS = REPO / "Experiments/NDNSF_DI_NativeTracer_Minindn.py"


def load_harness():
    spec = importlib.util.spec_from_file_location("lease_harness", HARNESS)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


class ExecutionLeaseHarnessIntegrationTest(unittest.TestCase):
    def test_dry_run_propagates_execution_lease_flag(self) -> None:
        env = dict(os.environ)
        env["PYTHONPATH"] = ":".join(
            [str(REPO / "NDNSF-DistributedInference"), str(REPO), env.get("PYTHONPATH", "")]
        )
        completed = subprocess.run(
            [
                sys.executable,
                str(HARNESS),
                "--dry-run",
                "--enable-execution-leases",
                "--requests",
                "1",
                "--concurrency",
                "1",
            ],
            cwd=str(REPO),
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )
        payload = json.loads(completed.stdout)
        self.assertIn("--execution-leases", payload["userDriverCommand"])

    def test_provider_command_requires_lease_only_when_enabled(self) -> None:
        harness = load_harness()
        row = {"provider": "/provider/A", "role": "/Backbone"}
        enabled = harness.provider_serve_command(
            row, REPO / "examples", require_execution_lease=True
        )
        disabled = harness.provider_serve_command(row, REPO / "examples")
        self.assertIn("--require-execution-lease", enabled)
        self.assertNotIn("--require-execution-lease", disabled)

    def test_generated_policy_grants_lease_service_without_duplicates(self) -> None:
        harness = load_harness()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "controller.policies"
            path.write_text(
                "ProviderPermissions\n        Permission\n"
                f"            {harness.SERVICE}\n"
                "UserPermissions\n        Permission\n"
                f"            {harness.SERVICE}\n",
                encoding="utf-8",
            )
            harness.add_execution_lease_policies(path)
            harness.add_execution_lease_policies(path)
            text = path.read_text(encoding="utf-8")
        self.assertEqual(text.count(harness.EXECUTION_LEASE_SERVICE), 2)


if __name__ == "__main__":
    unittest.main()
