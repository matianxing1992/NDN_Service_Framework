#!/usr/bin/env python3
"""Focused non-network checks for the NSC three-provider failover client."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import unittest


REPO = Path(__file__).resolve().parents[2]
NSC_DIR = REPO / "Experiments" / "NDN_NSC"
CONSUMER = NSC_DIR / "consumer"


class NscThreeProviderFailoverClientTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        env = os.environ.copy()
        env["PATH"] = "/usr/bin:/bin"
        subprocess.run(
            ["make", "consumer"],
            cwd=NSC_DIR,
            env=env,
            check=True,
            text=True,
            capture_output=True,
        )

    def test_logic_self_test_covers_provider_rotation_deadline_and_summary(self) -> None:
        result = subprocess.run(
            [str(CONSUMER), "--logic-self-test"],
            cwd=NSC_DIR,
            check=True,
            text=True,
            capture_output=True,
        )
        fields = dict(
            token.split("=", 1)
            for token in result.stdout.strip().split()[1:]
        )
        self.assertTrue(result.stdout.startswith("NSC_LOGIC_SELF_TEST_OK "))
        self.assertEqual(fields["providers"], "3")
        self.assertEqual(fields["rotation"], "0,1,2|1,2,0")
        self.assertEqual(fields["attempt_lifetime_full"], "200")
        self.assertEqual(fields["attempt_lifetime_remaining"], "75")
        self.assertEqual(fields["legacy_timeout"], "2")
        self.assertIn("excludes_late_and_wire_retransmissions",
                      fields["message_definition"])

    def test_legacy_single_provider_cli_remains_documented(self) -> None:
        result = subprocess.run(
            [str(CONSUMER)],
            cwd=NSC_DIR,
            check=False,
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("<provider[,provider...]>", result.stderr)
        self.assertIn("[attempt_timeout_ms]", result.stderr)
        self.assertIn("[measurement_start_monotonic_ms]", result.stderr)

    def test_empty_provider_list_is_rejected_before_network_execution(self) -> None:
        result = subprocess.run(
            [
                str(CONSUMER),
                "/muas/user",
                ",",
                "/FlightControl",
                "/ManualControl",
                "100",
                "1",
            ],
            cwd=NSC_DIR,
            check=False,
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("at least one provider is required", result.stderr)


if __name__ == "__main__":
    unittest.main()
