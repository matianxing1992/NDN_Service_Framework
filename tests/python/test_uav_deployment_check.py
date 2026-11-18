"""Regression for the NFD RIB-authorization probe in the UAV deployment check.

A reachable forwarder is not enough to deploy: without a matching management
authorization the first actor fails with "authorization rejected" after its
service container has started. The checker must report that before an operator
starts anything, and must not invent a trust grant to make itself pass.
"""

from __future__ import annotations

import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "NDNSF-UAV-APP/tools/uav_deployment_check.py"

spec = importlib.util.spec_from_file_location("uav_deployment_check", CHECKER)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class RecordingReporter:
    """Mirror only what the checker reads: the failure and warning sinks."""

    def __init__(self) -> None:
        self.ok_messages: list[str] = []
        self.warnings: list[str] = []
        self.failures: list[str] = []

    def ok(self, message: str) -> None:
        self.ok_messages.append(message)

    def warn(self, message: str) -> None:
        self.warnings.append(message)

    def fail(self, message: str) -> None:
        self.failures.append(message)


class NfdRibAuthorizationTest(unittest.TestCase):
    def probe(self, result: subprocess.CompletedProcess) -> RecordingReporter:
        reporter = RecordingReporter()
        with mock.patch.object(checker.shutil, "which", return_value="/usr/bin/nfdc"), \
                mock.patch.object(checker, "run", return_value=result):
            checker.check_nfd_rib_authorization(reporter, "/example/uav")
        return reporter

    def test_rejected_registration_is_a_deployment_failure(self) -> None:
        reporter = self.probe(subprocess.CompletedProcess(
            [], 1, stdout="Error 403 when adding route: authorization rejected", stderr=None))
        self.assertEqual(reporter.ok_messages, [])
        self.assertEqual(len(reporter.failures), 1)
        self.assertIn("grant the deployment identity rib/route management", reporter.failures[0])

    def test_admitted_registration_removes_the_probe_route(self) -> None:
        reporter = RecordingReporter()
        calls: list[list[str]] = []

        def fake_run(cmd, timeout=10):
            calls.append(list(cmd))
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr=None)

        with mock.patch.object(checker.shutil, "which", return_value="/usr/bin/nfdc"), \
                mock.patch.object(checker, "run", side_effect=fake_run):
            checker.check_nfd_rib_authorization(reporter, "/example/uav")
        self.assertEqual(len(reporter.ok_messages), 1)
        self.assertEqual(reporter.failures, [])
        self.assertEqual([call[1:3] for call in calls],
                         [["route", "add"], ["route", "remove"]])
        probe = calls[0][3]
        self.assertTrue(probe.startswith("/example/uav/deployment-check/"))
        self.assertEqual(calls[1][3], probe)

    def test_missing_nfdc_warns_instead_of_passing(self) -> None:
        reporter = RecordingReporter()
        with mock.patch.object(checker.shutil, "which", return_value=None):
            checker.check_nfd_rib_authorization(reporter, "/example/uav")
        self.assertEqual(reporter.ok_messages, [])
        self.assertEqual(reporter.failures, [])
        self.assertEqual(len(reporter.warnings), 1)
        self.assertIn("nfdc is unavailable", reporter.warnings[0])


class TrustAnchorLocationTest(unittest.TestCase):
    """A file anchor resolves beside the schema, not at the caller's --root-cert path."""

    def check(self, directory: Path, name: str = "schema.conf"):
        reporter = RecordingReporter()
        checker.check_trust_schema(reporter, directory / name)
        return reporter

    def test_anchor_beside_the_schema_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "schema.conf").write_text(
                'trust-anchor\n{\n  type file\n  file-name "root.cert"\n}\n', encoding="utf-8")
            (root / "root.cert").write_bytes(b"certificate")
            reporter = self.check(root)
        self.assertEqual(reporter.failures, [])
        self.assertIn("trust schema uses a file trust anchor", reporter.ok_messages)
        self.assertTrue(any("trust anchor resolves" in item for item in reporter.ok_messages))

    def test_anchor_elsewhere_is_a_failure_with_the_reason(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "schema.conf").write_text(
                'trust-anchor\n{\n  type file\n  file-name "root.cert"\n}\n', encoding="utf-8")
            reporter = self.check(root)
        self.assertEqual(len(reporter.failures), 1)
        self.assertIn("resolved relative to the schema directory", reporter.failures[0])
        self.assertIn("--root-cert", reporter.failures[0])

    def test_type_any_warns(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "schema.conf").write_text(
                "trust-anchor\n{\n  type any\n}\n", encoding="utf-8")
            reporter = self.check(root)
        self.assertEqual(reporter.failures, [])
        self.assertEqual(len(reporter.warnings), 1)
        self.assertIn("type any", reporter.warnings[0])

    def test_file_anchor_without_file_name_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "schema.conf").write_text(
                "trust-anchor\n{\n  type file\n}\n", encoding="utf-8")
            reporter = self.check(root)
        self.assertEqual(len(reporter.failures), 1)
        self.assertIn("without a usable file-name", reporter.failures[0])


if __name__ == "__main__":
    unittest.main()
