from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
ANALYZER_PATH = ROOT / "Experiments/analyze_uav_video_stage_timeline.py"
SPEC = importlib.util.spec_from_file_location("stage_attribution", ANALYZER_PATH)
assert SPEC is not None and SPEC.loader is not None
ANALYZER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ANALYZER
SPEC.loader.exec_module(ANALYZER)


def event(wall: float, role: str, name: str, steady_us: int,
          request_id: str, **fields: object) -> str:
    suffix = " ".join(f"{key}={value}" for key, value in fields.items())
    return (
        f"{wall:.6f} DEBUG NDNSF_TIMELINE role={role} event={name} "
        f"steady_us={steady_us} timestamp_us={int(wall * 1_000_000)} "
        f"requestId={request_id} {suffix}\n"
    )


def make_cell(root: Path) -> Path:
    cell = root / "fps-30"
    cell.mkdir(parents=True)
    (cell / "cell-summary.json").write_text(json.dumps({
        "measurement": {
            "startTimestamp": 0.0,
            "endTimestamp": 20.0,
            "seconds": 20.0,
            "warmupSeconds": 0,
        },
        "profile": {"fps": 30},
    }), encoding="utf-8")
    frame = "/NDNSF/UAV/VIDEO/FRAME/stream/%01/%01"
    cursor_a = "/NDNSF/STREAM/TIMELINE/stream/%01/%0A"
    cursor_b = "/NDNSF/STREAM/TIMELINE/stream/%01/%0B"
    provider = [
        event(1.0, "provider", "source-acquired", 2100, frame,
              clock_domain="host-steady", frame_correlation="exact",
              source_id=1, capture_origin_ns=1_000_000),
        event(2.0, "provider", "encoded-output-ready", 2000, frame,
              clock_domain="host-steady", frame_correlation="exact",
              source_id=1),
        event(2.1, "provider", "source-acquired", 2100, cursor_a,
              source_frame_id=1, capture_origin_ns=1_000_000),
        event(2.2, "provider", "source-acquired", 2200, cursor_b,
              source_frame_id=1, capture_origin_ns=1_000_000),
        event(3.0, "provider", "signed-and-materialized", 3000, cursor_a),
        event(4.0, "provider", "signed-and-materialized", 4000, cursor_b),
    ]
    consumer = [
        event(7.0, "consumer", "decoder-input", 7000, cursor_b,
              source_frame_id=1, frame_correlation="exact-pts"),
        event(9.0, "consumer", "decoder-output", 9000, frame,
              source_id=1, frame_correlation="exact"),
        event(25.0, "consumer", "decoder-output", 25000,
              "/NDNSF/UAV/VIDEO/FRAME/stream/%01/%02",
              source_id=2, frame_correlation="exact"),
    ]
    (cell / "drone.log").write_text("".join(provider), encoding="utf-8")
    (cell / "ground-station.log").write_text(
        "".join(consumer), encoding="utf-8")
    return cell


class StageAttributionTest(unittest.TestCase):
    def test_exact_five_stage_join_uses_last_source_cursor(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cell = make_cell(Path(directory) / "campaign")
            summary, rows = ANALYZER.analyze_cell(cell)
            self.assertEqual(summary["candidateFrames"], 1)
            self.assertEqual(summary["completeFrames"], 1)
            self.assertEqual(summary["coverageRatio"], 1.0)
            self.assertEqual(summary["exclusions"], {})
            self.assertEqual(rows[0]["last_source_cursor"], 11)
            self.assertEqual(rows[0]["source_segment_count"], 2)
            self.assertEqual(summary["sourceSegmentsPerFrame"]["mean"], 2.0)
            self.assertEqual(rows[0]["capture_to_encoded_ms"], 1.0)
            self.assertEqual(rows[0]["encoded_to_materialized_ms"], 2.0)
            self.assertEqual(
                rows[0]["materialized_to_decoder_input_ms"], 3.0)
            self.assertEqual(rows[0]["decoder_input_to_output_ms"], 2.0)
            self.assertEqual(rows[0]["capture_to_output_ms"], 8.0)
            self.assertEqual(summary["stages"]["capture_to_output_ms"], {
                "count": 1, "mean": 8.0, "p50": 8.0,
                "p95": 8.0, "p99": 8.0})

    def test_missing_last_materialization_is_visible(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cell = make_cell(Path(directory) / "campaign")
            text = (cell / "drone.log").read_text(encoding="utf-8")
            text = "\n".join(
                line for line in text.splitlines()
                if not ("signed-and-materialized" in line and "%0B" in line)
            ) + "\n"
            (cell / "drone.log").write_text(text, encoding="utf-8")
            summary, rows = ANALYZER.analyze_cell(cell)
            self.assertEqual(rows, [])
            self.assertEqual(summary["completeFrames"], 0)
            self.assertEqual(
                summary["exclusions"],
                {"missing_last_segment_materialized": 1})

    def test_campaign_output_cannot_modify_frozen_input(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            campaign = Path(directory) / "campaign"
            make_cell(campaign)
            with self.assertRaisesRegex(
                    ValueError, "outside the immutable campaign"):
                ANALYZER.analyze_campaign(
                    campaign, campaign / "diagnostic")

    def test_campaign_writes_separate_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            campaign = Path(directory) / "campaign"
            make_cell(campaign)
            output = Path(directory) / "diagnostic"
            summaries = ANALYZER.analyze_campaign(campaign, output)
            self.assertEqual(len(summaries), 1)
            self.assertTrue(
                (output / "campaign-stage-summary.json").is_file())
            self.assertTrue(
                (output / "campaign-stage-summary.csv").is_file())
            self.assertTrue((output / "stage-attribution.md").is_file())
            self.assertTrue(
                (output / "fps-30/per-frame-stages.csv").is_file())


if __name__ == "__main__":
    unittest.main()
