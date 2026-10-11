#!/usr/bin/env python3
"""Public-only source and executable smoke checks for the Spec 164 examples."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
EXAMPLE_DIR = (
    ROOT / "examples/python/NDNSF-DistributedRepo/artifact_api"
)
PUBLISH = EXAMPLE_DIR / "publish_file.py"
FETCH = EXAMPLE_DIR / "fetch_file.py"
PYTHON_WRAPPER = ROOT / "NDNSF-DistributedRepo/pythonWrapper"


class PublicArtifactExamplesTest(unittest.TestCase):
    def test_examples_do_not_use_private_or_packet_level_apis(self):
        forbidden = (
            "._" + "client",
            "_client." + "control_mode",
            "DataPacket",
            "make_segmented_" + "data_packets",
            "put_signed_" + "packets",
            "get_signed_" + "packets",
            "ArtifactReplica" + "Session",
            "receive_" + "chunk",
        )
        for path in (PUBLISH, FETCH):
            source = path.read_text(encoding="utf-8")
            for token in forbidden:
                self.assertNotIn(token, source, f"{path}: forbidden {token}")

    def test_native_in_app_publish_and_fetch(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            source = root / "source.bin"
            payload = bytes(range(251)) * 200
            source.write_bytes(payload)
            digest = hashlib.sha256(payload).hexdigest()
            reference = root / "reference.json"
            destination = root / "destination.bin"
            env = dict(os.environ)
            env["PYTHONPATH"] = str(PYTHON_WRAPPER)

            published = subprocess.run(
                [
                    sys.executable,
                    str(PUBLISH),
                    "--store-dir", str(root / "store"),
                    "--source", str(source),
                    "--name", "/examples/spec164/public",
                    "--reference-out", str(reference),
                ],
                cwd=ROOT,
                env=env,
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            fetched = subprocess.run(
                [
                    sys.executable,
                    str(FETCH),
                    "--store-dir", str(root / "store"),
                    "--reference", str(reference),
                    "--destination", str(destination),
                ],
                cwd=ROOT,
                env=env,
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            self.assertEqual(destination.read_bytes(), payload)
            publication = json.loads(published.stdout)
            retrieval = json.loads(fetched.stdout)
            self.assertEqual(publication["achievedReplicas"], 0)
            self.assertIsNotNone(publication["receipt"])
            self.assertIsNotNone(publication["handle"])
            self.assertEqual(retrieval["sha256"], digest)

    def test_bilingual_readmes_cover_public_contract(self):
        english = (
            ROOT / "NDNSF-DistributedRepo/README.md"
        ).read_text(encoding="utf-8")
        chinese = (
            ROOT / "NDNSF-DistributedRepo/README_ch.md"
        ).read_text(encoding="utf-8")
        required = (
            "ArtifactRepositoryApi",
            "make_in_app",
            "make_persistent",
            "DistributedRepoServer",
            "SPEC192_COMPATIBILITY_V1",
            "ArtifactHandle",
            "ArtifactPublishResult",
            "ArtifactFetchResult",
        )
        for token in required:
            self.assertIn(token, english)
            self.assertIn(token, chinese)


if __name__ == "__main__":
    unittest.main()
