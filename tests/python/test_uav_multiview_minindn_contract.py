from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "NDNSF-UAV-APP" / "tools" / "run_uav_multiview_minindn.py"
FIXTURE = ROOT / "NDNSF-UAV-APP" / "testdata" / "multiview-car" / "manifest.json"


class MultiViewMiniNdnContractTest(unittest.TestCase):
    def test_reusable_topology_has_multiple_producers_one_terminal_owner(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uav-mv-minindn-") as directory:
            output = Path(directory) / "trace.json"
            completed = subprocess.run([sys.executable, str(RUNNER), "--fixture", str(FIXTURE),
                                        "--output", str(output)], text=True,
                                       capture_output=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            value = json.loads(output.read_text())
            self.assertGreaterEqual(len(value["topology"]["uavProducers"]), 2)
            self.assertFalse(value["imageBytesInServicePayload"])
            terminals = [event for event in value["trace"] if event["stage"] == "TERMINAL_ACCEPTED"]
            self.assertEqual(len(terminals), 1)
            self.assertEqual(terminals[0]["terminalOwner"], value["topology"]["selectedProvider"])
            self.assertEqual(set(value["scenarios"]), {"nominal", "provider-selection",
                                                         "unavailable-view", "late-view",
                                                         "publication-failure", "model-missing",
                                                         "model-digest-failure"})


if __name__ == "__main__":
    unittest.main()
