from __future__ import annotations

import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from contract._support import REPO


BUILD = REPO / "packaging" / "ndnsf-di-container" / "oci" / "scripts" / "build-release.sh"


class OciReleaseTest(unittest.TestCase):
    def generate(self, output: Path) -> dict:
        root = output.parent
        sbom = root / "sbom.spdx.json"
        provenance = root / "provenance.json"
        sbom.write_text('{"spdxVersion":"SPDX-2.3"}\n', encoding="utf-8")
        provenance.write_text('{"builder":"unit"}\n', encoding="utf-8")
        sha = "a" * 64
        result = subprocess.run([
            str(BUILD), "--output", str(output), "--release-id", "spec108-r1",
            "--candidate-id", "spec107-c1-test", "--source-revision", "0123456789abcdef",
            "--created-at", "2026-07-12T12:00:00Z",
            "--image-reference", f"registry.example/ndnsf-di@sha256:{sha}",
            "--image-digest", f"sha256:{sha}", "--sbom", str(sbom),
            "--provenance", str(provenance),
        ], text=True, capture_output=True, check=False, cwd=REPO)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads((output / "release-manifest.json").read_text(encoding="utf-8"))

    def test_manifest_is_reproducible_and_digest_bound(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            left = self.generate(root / "left")
            right = self.generate(root / "right")
        self.assertEqual(left, right)
        image = left["images"]["linux-amd64-cpu"]
        self.assertTrue(image["reference"].endswith(image["digest"]))
        self.assertRegex(left["sbom"]["digest"], r"^sha256:[a-f0-9]{64}$")
        self.assertRegex(left["provenance"]["digest"], r"^sha256:[a-f0-9]{64}$")

    def test_mismatched_reference_and_digest_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sbom = root / "sbom"
            provenance = root / "provenance"
            sbom.write_text("sbom", encoding="utf-8")
            provenance.write_text("provenance", encoding="utf-8")
            result = subprocess.run([
                str(BUILD), "--output", str(root / "out"), "--release-id", "r",
                "--candidate-id", "c", "--source-revision", "0123456",
                "--created-at", "2026-07-12T12:00:00Z",
                "--image-reference", "registry/x@sha256:" + "a" * 64,
                "--image-digest", "sha256:" + "b" * 64,
                "--sbom", str(sbom), "--provenance", str(provenance),
            ], text=True, capture_output=True, check=False, cwd=REPO)
        self.assertEqual(result.returncode, 4)
        self.assertIn("OCI_DIGEST_MISMATCH", result.stderr)


if __name__ == "__main__":
    unittest.main()
