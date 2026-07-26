#!/usr/bin/env python3

import json
from pathlib import Path
import subprocess
import sys
import unittest


REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "Experiments/NDNSF_LiveStream_Minindn.py"


class LiveStreamMiniNdnContractTest(unittest.TestCase):
    def test_harness_preflight_is_non_privileged_and_deterministic(self):
        completed = subprocess.run(
            [sys.executable, str(SCRIPT), "--check-only", "--loss", "5",
             "--count", "12", "--consumers", "2", "--fec"],
            cwd=REPO, text=True, capture_output=True, check=True)
        self.assertTrue(json.loads(completed.stdout)["passed"])

    def test_examples_keep_crypto_and_manual_face_logic_out(self):
        provider = (REPO / "examples/python/live_stream/provider.py").read_text()
        consumer = (REPO / "examples/python/live_stream/consumer.py").read_text()
        self.assertNotIn("encrypt", provider.lower())
        self.assertNotIn("decrypt", consumer.lower())
        self.assertNotIn("express_interest", consumer)
        self.assertIn("reserve_many_ahead", provider)
        self.assertIn("reserve_group", provider)
        self.assertIn("publish_group", provider)
        self.assertIn("latest_descriptor", provider)
        self.assertIn("open_live_stream", consumer)
        self.assertIn("--minimum-first-cursor", consumer)


if __name__ == "__main__":
    unittest.main()
