#!/usr/bin/env python3
"""Tests for the UAV stream parity MiniNDN campaign summarizer."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
CAMPAIGN = REPO / "Experiments/NDNSF_UAV_Stream_Parity_Campaign.py"


def load_campaign():
    spec = importlib.util.spec_from_file_location("uav_stream_parity_campaign", CAMPAIGN)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


class UavStreamParityCampaignTest(unittest.TestCase):
    def test_percentile_interpolates(self) -> None:
        campaign = load_campaign()
        self.assertEqual(campaign.percentile([10.0, 20.0, 30.0], 0.5), 20.0)
        self.assertEqual(campaign.percentile([], 0.95), 0.0)

    def test_parse_run_extracts_bounded_stream_and_fec_evidence(self) -> None:
        campaign = load_campaign()
        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            (run_dir / "ground-station.log").write_text(
                "100.0 INFO GS_RESPONSE status=streaming fec_parity_shards=1\n"
                "GS_VIDEO_ADAPTIVE_STATE reason=active VideoAdaptive drone=A "
                "rtt_ms=40 pending_chunks=2 pending_bytes=7200 "
                "fec_recovered_chunks=1 decoded_frame_gap=0 timeouts=1 "
                "nacks=0 duplicates=0\n"
                "GS_VIDEO_FEC_RECOVERED stream=A session=1 frame_seq=2 packet_seq=3\n"
                "GS_DECODED_FRAMES count=30\n"
                "101.0 INFO GS_VIDEO_ADAPTIVE_STATE reason=stop-ack state=stopped\n"
                "GS_GUI_EXIT rc=0\n",
                encoding="utf-8",
            )
            summary = campaign.parse_run(run_dir, 0, ["launcher"])

        self.assertTrue(summary["completion"])
        self.assertTrue(summary["metricsValid"])
        self.assertTrue(summary["videoCompletion"])
        self.assertTrue(summary["controlCompletion"])
        self.assertEqual(summary["fecRecoveryLogCount"], 1)
        self.assertEqual(summary["fecRecoveredChunks"], 1)
        self.assertEqual(summary["maxPendingChunks"], 2)
        self.assertEqual(summary["maxPendingBytes"], 7200)
        self.assertEqual(summary["rttP50Ms"], 40.0)

    def test_primary_matrix_and_command_are_matched(self) -> None:
        campaign = load_campaign()
        cells = campaign.campaign_cells([0, 5], [0, 1], 3)
        self.assertEqual(len(cells), 12)
        self.assertEqual(cells[0], (0, 0, 1))
        self.assertEqual(cells[-1], (5, 1, 3))

        command = campaign.build_command(
            run_dir=Path("/tmp/run"),
            topology=Path("/tmp/topology.conf"),
            duration_seconds=60,
            fec_parity_shards=0,
            include_mavlink=True,
        )
        self.assertIn("--auto-video-test", command)
        self.assertIn("--auto-mavlink-test", command)
        parity_index = command.index("--video-fec-parity-shards")
        self.assertEqual(command[parity_index + 1], "0")
        duration_index = command.index("--auto-stop-seconds")
        self.assertEqual(command[duration_index + 1], "60")

    def test_sixty_second_run_requires_usable_decoded_frame_rate(self) -> None:
        campaign = load_campaign()
        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            (run_dir / "ground-station.log").write_text(
                "100.0 INFO GS_RESPONSE status=streaming fec_parity_shards=1\n"
                "MAVLink arm drone=A accepted=true\n"
                "MAVLink takeoff drone=A accepted=true\n"
                "MAVLink land drone=A accepted=true\n"
                "GS_DECODED_FRAMES count=90\n"
                "161.0 INFO GS_VIDEO_ADAPTIVE_STATE reason=stop-ack state=stopped\n"
                "GS_GUI_EXIT rc=0\n",
                encoding="utf-8",
            )
            summary = campaign.parse_run(
                run_dir,
                0,
                ["launcher"],
                duration_seconds=60,
                include_mavlink=True,
            )

        self.assertTrue(summary["processCompletion"])
        self.assertTrue(summary["durationAccepted"])
        self.assertTrue(summary["controlCompletion"])
        self.assertFalse(summary["videoCompletion"])
        self.assertFalse(summary["completion"])
        self.assertEqual(summary["minimumDecodedFrames"], 900)
        self.assertAlmostEqual(summary["decodedFrameRate"], 90 / 61)

    def test_treatment_aggregation_keeps_failed_runs(self) -> None:
        campaign = load_campaign()
        runs = [
            {
                "lossPercent": 5, "fecParityShards": 1,
                "completion": True, "videoCompletion": True,
                "controlCompletion": True, "decodedFrames": 60,
                "fecRecoveredChunks": 3, "maxTimeouts": 4,
                "maxDecodedFrameGap": 0, "maxPendingChunks": 1,
                "maxPendingBytes": 3600, "rttP50Ms": 20.0,
                "rttP95Ms": 40.0, "mavlinkArm": True,
                "mavlinkTakeoff": True, "mavlinkLand": True,
            },
            {
                "lossPercent": 5, "fecParityShards": 1,
                "completion": False, "videoCompletion": False,
                "controlCompletion": False, "decodedFrames": 0,
                "fecRecoveredChunks": 0, "maxTimeouts": 9,
                "maxDecodedFrameGap": 2, "maxPendingChunks": 4,
                "maxPendingBytes": 7200, "rttP50Ms": 0.0,
                "rttP95Ms": 0.0, "mavlinkArm": False,
                "mavlinkTakeoff": False, "mavlinkLand": False,
            },
        ]
        aggregate = campaign.aggregate_treatments(runs)[0]
        self.assertEqual(aggregate["runCount"], 2)
        self.assertEqual(aggregate["completedRuns"], 1)
        self.assertEqual(aggregate["completionRate"], 0.5)
        self.assertEqual(aggregate["videoCompletionRate"], 0.5)
        self.assertEqual(aggregate["controlCompletionRate"], 0.5)
        self.assertEqual(aggregate["maxDecodedFrameGap"], 2)

    def test_malformed_required_metric_is_reported(self) -> None:
        campaign = load_campaign()
        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            (run_dir / "ground-station.log").write_text(
                "100.0 INFO GS_RESPONSE status=streaming fec_parity_shards=1\n"
                "GS_VIDEO_ADAPTIVE_STATE reason=active VideoAdaptive "
                "rtt_ms=20 pending_chunks=0 pending_bytes=0 "
                "fec_recovered_chunks=0 decoded_frame_gap=0 timeouts=bad "
                "nacks=0 duplicates=0\n"
                "GS_DECODED_FRAMES count=30\n"
                "101.0 INFO GS_VIDEO_ADAPTIVE_STATE reason=stop-ack state=stopped\n"
                "GS_GUI_EXIT rc=0\n",
                encoding="utf-8",
            )
            summary = campaign.parse_run(run_dir, 0, ["launcher"])

        self.assertTrue(summary["completion"])
        self.assertFalse(summary["metricsValid"])
        self.assertIn("timeouts:invalid", summary["malformedMetrics"])
        self.assertFalse(campaign.is_run_accepted(
            summary,
            max_pending_chunks=48,
            max_pending_bytes=16 * 1024 * 1024,
        ))

    def test_invalid_parity_and_loss_values_fail(self) -> None:
        campaign = load_campaign()
        self.assertEqual(campaign.parse_int_csv("0,1", minimum=0, maximum=1), [0, 1])
        with self.assertRaises(ValueError):
            campaign.parse_int_csv("2", minimum=0, maximum=1)
        with self.assertRaises(ValueError):
            campaign.parse_int_csv("", minimum=0, maximum=100)


if __name__ == "__main__":
    unittest.main()
