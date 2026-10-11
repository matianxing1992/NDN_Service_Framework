#!/usr/bin/env python3
"""The former Python catalog and repair sidecar is explicitly retired."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / "examples/python/NDNSF-DistributedRepo/generic_object_store"


class RetiredPythonRepoEntrypointsTest(unittest.TestCase):
    def test_python_service_campaign_and_sidecar_exit_with_versioned_errors(self):
        expected = {
            "repo_node.py": "DistributedRepoServer",
            "repo_campaign.py": "C++ Repo integration and MiniNDN gates",
            "catalog_sync.py": "C++ Persistent Repo service",
        }
        for script, replacement in expected.items():
            with self.subTest(script=script):
                result = subprocess.run(
                    [sys.executable, str(EXAMPLES / script), "--help"],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 2)
                self.assertIn("SPEC192_COMPATIBILITY_V1", result.stderr)
                self.assertIn(replacement, result.stderr)


if __name__ == "__main__":
    unittest.main()
