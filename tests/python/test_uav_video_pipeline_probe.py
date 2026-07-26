#!/usr/bin/env python3
"""Spec 122 timestamp-backend capability gate."""

import json
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
PROBE = ROOT / "NDNSF-UAV-APP/tools/run_uav_video_pipeline_probe.sh"


class UavVideoPipelineProbeTest(unittest.TestCase):
    def test_gstreamer_preserves_pts_and_lifecycle(self) -> None:
        completed = subprocess.run(
            [str(PROBE), "--json"], cwd=ROOT, check=True, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
        result = json.loads(completed.stdout)
        self.assertEqual(result["backend"], "gstreamer")
        self.assertTrue(result["available"])
        self.assertTrue(result["preservesPts"])
        self.assertTrue(result["accessUnitAligned"])
        self.assertTrue(result["headlessAppsink"])
        self.assertTrue(result["gtkGlPluginAvailable"])
        self.assertEqual(result["startStopCycles"], 5)
        self.assertLess(result["maxStopMs"], 1000)


if __name__ == "__main__":
    unittest.main()
