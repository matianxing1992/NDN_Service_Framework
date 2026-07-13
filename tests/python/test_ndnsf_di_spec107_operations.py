#!/usr/bin/env python3
"""Spec 107 local packaged-process supervision behavior tests."""

from __future__ import annotations

import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "ndnsf-di"))

from spec107_local_supervisor import LocalSupervisor, LocalSupervisorError  # noqa: E402


class Spec107OperationsTest(unittest.TestCase):
    def test_packaged_child_reaches_ready_restarts_and_cleans_up(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            release = root / "release-n"
            binary = release / "bin/provider"
            binary.parent.mkdir(parents=True)
            binary.write_text(
                "#!/bin/sh\necho PROVIDER_READY\nwhile :; do sleep 1; done\n",
                encoding="utf-8")
            binary.chmod(binary.stat().st_mode | stat.S_IXUSR)
            supervisor = LocalSupervisor(
                staging_root=root / "staging",
                release_root=release,
                candidate_id=("spec107-c1-111111111111-222222222222-333333333333-"
                              "444444444444-555555555555-666666666666"),
                plan_digest="sha256:" + "1" * 64,
            )
            first = supervisor.start(
                "provider-0", ["bin/provider"], ready_marker="PROVIDER_READY",
                timeout_seconds=2)
            status = supervisor.status()
            self.assertEqual(status["supervisionClass"], "local-process-supervision")
            self.assertTrue(status["physicalProductionDeferred"])
            self.assertEqual(status["processes"][0]["state"], "READY")
            second = supervisor.restart("provider-0", timeout_seconds=2)
            self.assertNotEqual(first["pid"], second["pid"])
            stopped = supervisor.stop_all()
            self.assertTrue(stopped["cleanupProven"])
            self.assertEqual(supervisor.status()["processes"], [])

    def test_operator_script_emits_structured_canary_and_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            release = root / "release"
            binary = release / "bin/provider"
            binary.parent.mkdir(parents=True)
            binary.write_text(
                "#!/bin/sh\necho READY\nwhile :; do sleep 1; done\n",
                encoding="utf-8")
            binary.chmod(0o755)
            config = root / "supervisor.json"
            config.write_text(json.dumps({
                "schema": "ndnsf-di-spec107-local-supervisor-config-v1",
                "candidateId": (
                    "spec107-c1-111111111111-222222222222-333333333333-"
                    "444444444444-555555555555-666666666666"),
                "planDigest": "sha256:" + "1" * 64,
                "releaseRoot": str(release),
                "processes": [{
                    "name": "provider-0", "command": ["bin/provider"],
                    "readyMarker": "READY",
                }],
            }), encoding="utf-8")
            output = root / "canary.json"
            result = subprocess.run([
                str(REPO / "packaging/ndnsf-di-systemd/run-local-supervised.sh"),
                "canary", "--config", str(config),
                "--staging-root", str(root / "staging"),
                "--output", str(output), "--restart",
            ], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            record = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(record["verdict"], "PASS")
            self.assertTrue(record["cleanup"]["cleanupProven"])
            self.assertEqual(record["status"]["supervisionClass"],
                             "local-process-supervision")


if __name__ == "__main__":
    unittest.main()
