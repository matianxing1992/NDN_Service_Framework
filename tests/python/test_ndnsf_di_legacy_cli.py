from __future__ import annotations

import contextlib
import io
import unittest

from ndnsf_distributed_inference import policy, runtime_v1


class LegacyCliTest(unittest.TestCase):
    def test_ndnsf_di_help_preserves_current_subcommands(self) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output), self.assertRaises(SystemExit) as raised:
            runtime_v1.main(["--help"])
        self.assertEqual(raised.exception.code, 0)
        for command in ("provider", "plan", "run", "bench", "status", "metrics", "doctor", "contract-smoke", "schema-sample", "inspect"):
            self.assertIn(command, output.getvalue())

    def test_ndnsf_di_policy_required_arguments_remain_fail_closed(self) -> None:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as raised:
            policy.main([])
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("--config", stderr.getvalue())
        self.assertIn("--out-dir", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
