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
                "GS_VIDEO_ADAPTIVE_STATE reason=active VideoAdaptive drone=A "
                "rtt_ms=40 pending_chunks=2 pending_bytes=7200 "
                "fec_recovered_chunks=1 decoded_frame_gap=0 timeouts=1 "
                "nacks=0 duplicates=0\n"
                "GS_VIDEO_FEC_RECOVERED stream=A session=1 frame_seq=2 packet_seq=3\n"
                "GS_DECODED_FRAMES count=30\n"
                "GS_GUI_EXIT rc=0\n",
                encoding="utf-8",
            )
            summary = campaign.parse_run(run_dir, 0, ["launcher"])

        self.assertTrue(summary["completion"])
        self.assertEqual(summary["fecRecoveryLogCount"], 1)
        self.assertEqual(summary["fecRecoveredChunks"], 1)
        self.assertEqual(summary["maxPendingChunks"], 2)
        self.assertEqual(summary["maxPendingBytes"], 7200)
        self.assertEqual(summary["rttP50Ms"], 40.0)


if __name__ == "__main__":
    unittest.main()
