from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[4]
PATH = REPO / "packaging/ndnsf-di-container/oci/scripts/scan-secrets.py"
SPEC = importlib.util.spec_from_file_location("spec110_secret_scanner", PATH)
scanner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(scanner)


class SecretScannerTests(unittest.TestCase):
    def test_clean_artifact_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "release.txt"
            path.write_text("NDNSF runtime release metadata")
            report = scanner.scan([path], scope="artifact")
            self.assertEqual(report["status"], "PASS")

    def test_text_finding_is_redacted(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "evidence.log"
            path.write_text("Authorization: Bearer synthetic-value")
            report = scanner.scan([path], scope="log")
            self.assertEqual(report["status"], "FAIL")
            self.assertEqual(report["findings"][0]["kind"], "bearer-token")
            self.assertNotIn("synthetic-value", str(report))

    def test_binary_sif_marker_is_detected_streaming(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "candidate.sif"
            path.write_bytes(b"\x00" * 5000 + b"-----BEGIN PRIVATE KEY-----" + b"\x00" * 5000)
            report = scanner.scan([path], scope="artifact")
            self.assertEqual(report["status"], "FAIL")
            self.assertEqual(report["findings"][0]["kind"], "private-key")


if __name__ == "__main__":
    unittest.main()
