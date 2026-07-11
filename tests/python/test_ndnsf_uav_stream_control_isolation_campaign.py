#!/usr/bin/env python3
"""Tests for UAV stream/control isolation campaign."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
CAMPAIGN = REPO / "Experiments/NDNSF_UAV_Stream_Control_Isolation_Campaign.py"


def load_campaign():
    spec = importlib.util.spec_from_file_location("uav_isolation_campaign", CAMPAIGN)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


class UavStreamControlIsolationCampaignTest(unittest.TestCase):
    def test_primary_matrix_has_five_cells_and_fifteen_runs(self) -> None:
        campaign = load_campaign()
        modes = campaign.parse_workload_modes(campaign.DEFAULT_WORKLOADS)
        cells = campaign.campaign_cells(modes, 3)
        self.assertEqual(len(modes), 5)
        self.assertEqual(len(cells), 15)
        self.assertEqual(cells[0], ("control-only", 1))
        self.assertEqual(cells[-1], ("combined-fec1", 3))

    def test_mode_commands_select_only_required_automation(self) -> None:
        campaign = load_campaign()
        common = {
            "run_dir": Path("/tmp/run"),
            "topology": Path("/tmp/topology.conf"),
            "duration_seconds": 60,
        }
        control = campaign.build_mode_command(mode="control-only", **common)
        video = campaign.build_mode_command(mode="video-only-fec0", **common)
        combined = campaign.build_mode_command(mode="combined-fec1", **common)
        self.assertIn("--auto-mavlink-test", control)
        self.assertNotIn("--auto-video-test", control)
        self.assertNotIn("--video-fec-parity-shards", control)
        self.assertIn("--auto-video-test", video)
        self.assertNotIn("--auto-mavlink-test", video)
        self.assertIn("--auto-video-test", combined)
        self.assertIn("--auto-mavlink-test", combined)

    def test_control_only_accepts_without_stream_metrics(self) -> None:
        campaign = load_campaign()
        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            (run_dir / "ground-station.log").write_text(
                "MAVLink arm drone=A accepted=true\n"
                "MAVLink takeoff drone=A accepted=true\n"
                "MAVLink land drone=A accepted=true\n"
                "GS_GUI_EXIT rc=0\n",
                encoding="utf-8",
            )
            result = campaign.parse_mode_run(
                run_dir,
                0,
                ["launcher"],
                mode="control-only",
                repetition=1,
                loss_percent=5,
                duration_seconds=60,
                elapsed_seconds=8.0,
            )
        self.assertTrue(result["accepted"])
        self.assertIsNone(result["videoCompletion"])
        self.assertTrue(result["controlCompletion"])
        self.assertTrue(result["metricsValid"])

    def test_video_mode_reuses_usable_frame_gate(self) -> None:
        campaign = load_campaign()
        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            (run_dir / "ground-station.log").write_text(
                "100.0 INFO GS_RESPONSE status=streaming fec_parity_shards=0\n"
                "GS_VIDEO_ADAPTIVE_STATE reason=active VideoAdaptive "
                "rtt_ms=20 pending_chunks=0 pending_bytes=0 fec_recovered_chunks=0 "
                "decoded_frame_gap=0 timeouts=0 nacks=0 duplicates=0\n"
                "GS_DECODED_FRAMES count=90\n"
                "161.0 INFO GS_VIDEO_ADAPTIVE_STATE reason=stop-ack VideoAdaptive "
                "rtt_ms=20 pending_chunks=0 pending_bytes=0 fec_recovered_chunks=0 "
                "decoded_frame_gap=0 timeouts=0 nacks=0 duplicates=0\n"
                "GS_GUI_EXIT rc=0\n",
                encoding="utf-8",
            )
            result = campaign.parse_mode_run(
                run_dir,
                0,
                ["launcher"],
                mode="video-only-fec0",
                repetition=1,
                loss_percent=5,
                duration_seconds=60,
                elapsed_seconds=70.0,
            )
        self.assertFalse(result["accepted"])
        self.assertFalse(result["videoCompletion"])
        self.assertIsNone(result["controlCompletion"])

    def test_aggregate_separates_required_components(self) -> None:
        campaign = load_campaign()
        rows = [{
            "workloadMode": "control-only",
            "videoRequired": False,
            "controlRequired": True,
            "accepted": True,
            "videoCompletion": None,
            "controlCompletion": True,
            "decodedFrames": 0,
            "fecRecoveredChunks": 0,
            "rttP95Ms": 0.0,
            "maxTimeouts": 0,
            "elapsedSeconds": 8.0,
        }]
        aggregate = campaign.aggregate_cells(rows)[0]
        self.assertEqual(aggregate["videoRunCount"], 0)
        self.assertEqual(aggregate["controlRunCount"], 1)
        self.assertEqual(aggregate["controlCompletedRuns"], 1)
        self.assertIsNone(aggregate["meanDecodedFrames"])

    def test_unknown_mode_is_rejected(self) -> None:
        campaign = load_campaign()
        with self.assertRaises(ValueError):
            campaign.parse_workload_modes("video-only-fec0,unknown")


if __name__ == "__main__":
    unittest.main()
