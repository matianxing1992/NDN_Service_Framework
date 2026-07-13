from __future__ import annotations

from pathlib import Path
import subprocess
import tempfile
import unittest

from contract._support import REPO


SCAN = REPO / "packaging" / "ndnsf-di-container" / "oci" / "scripts" / "scan-release.sh"


class OciBuildContextTest(unittest.TestCase):
    def test_root_dockerignore_excludes_secrets_and_bulk_artifacts(self) -> None:
        patterns = set((REPO / ".dockerignore").read_text(encoding="utf-8").splitlines())
        for required in (".git", ".env", "*.key", "*.pem", "*.sif", "*.onnx", "**/identities", "**/secrets"):
            self.assertIn(required, patterns)

    def test_oci_source_contains_no_secret_marker(self) -> None:
        result = subprocess.run([str(SCAN), "--path", str(REPO / "packaging" / "ndnsf-di-container" / "oci")],
                                text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("SECRET_SCAN_PASS", result.stdout)

    def test_private_key_marker_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "private.pem"
            path.write_text("-----BEGIN PRIVATE KEY-----\nsynthetic\n", encoding="utf-8")
            result = subprocess.run([str(SCAN), "--path", directory], text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 4)
        self.assertIn("SECRET_SCAN_FAIL", result.stderr)


if __name__ == "__main__":
    unittest.main()
