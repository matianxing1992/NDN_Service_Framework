#!/usr/bin/env python3
"""Spec 121 latency-correlation regression tests."""

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "Experiments/analyze_stream_latency.py"
SPEC = importlib.util.spec_from_file_location("analyze_stream_latency", MODULE_PATH)
assert SPEC and SPEC.loader
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)

SOURCE_MODULE_PATH = ROOT / "Experiments/generate_uav_latency_source.py"
SOURCE_SPEC = importlib.util.spec_from_file_location(
    "generate_uav_latency_source", SOURCE_MODULE_PATH)
assert SOURCE_SPEC and SOURCE_SPEC.loader
source = importlib.util.module_from_spec(SOURCE_SPEC)
SOURCE_SPEC.loader.exec_module(source)


def event(role: str, name: str, cursor: str, steady_us: int, **metadata: str) -> str:
    suffix = " ".join(f"{key}={value}" for key, value in metadata.items())
    return (
        "NDNSF_TIMELINE "
        f"role={role} event={name} steady_us={steady_us} timestamp_us={steady_us} "
        f"requestId=/NDNSF/STREAM/TIMELINE/stream-a/%01/{cursor} {suffix}"
    )


def frame_event(role: str, name: str, frame_id: int, steady_us: int,
                **metadata: str) -> str:
    suffix = " ".join(f"{key}={value}" for key, value in metadata.items())
    return (
        "NDNSF_TIMELINE "
        f"role={role} event={name} steady_us={steady_us} timestamp_us={steady_us} "
        f"requestId=/NDNSF/UAV/VIDEO/FRAME/stream-a/%01/{frame_id} "
        f"frame_correlation=exact source_id={frame_id} output_ordinal=0 {suffix}"
    )


class StreamLatencyAnalyzerTest(unittest.TestCase):
    def test_visual_oracle_survives_h264_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            generated = source.generate_source(
                root, frames=3, width=640, height=360, fps=30,
                capture_origin_ns=7_000_000_000)
            subprocess.run(generated["encodeCommand"], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            subprocess.run(generated["decodeCommand"], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            decoded = Path(generated["decodedRgbPath"]).read_bytes()
            frame_bytes = 640 * 360 * 3
            self.assertEqual(len(decoded), frame_bytes * 3)
            recovered = [
                source.decode_marker_rgb(
                    decoded[index * frame_bytes:(index + 1) * frame_bytes],
                    640, 360)
                for index in range(3)
            ]
            self.assertEqual([value["sourceFrameId"] for value in recovered],
                             [0, 1, 2])
            self.assertEqual(
                [value["captureOriginNs"] for value in recovered],
                [7_000_000_000, 7_033_333_333, 7_066_666_667])
            manifest = json.loads(Path(generated["manifestPath"]).read_text())
            self.assertEqual(manifest["oracleVersion"], 1)

    def test_frame_evidence_requires_oracle_runtime_agreement(self) -> None:
        fixture = json.loads((
            ROOT / "tests/fixtures/uav-video-latency/frame-evidence-v1.json"
        ).read_text())
        expected = fixture["expected"]
        result = analyzer.analyze_frame_observations(fixture["observations"])
        self.assertEqual(result["acceptedFrames"], expected["acceptedFrames"])
        self.assertEqual(result["rejectedFrames"]["oracle-runtime-mismatch"],
                         expected["oracle-runtime-mismatch"])
        self.assertEqual(result["rejectedFrames"]["stale-session"],
                         expected["stale-session"])
        self.assertEqual(result["captureToWidgetMs"]["p95"],
                         expected["captureToWidgetP95Ms"])

    def test_numeric_cursor_collision_cannot_join_different_identities(self) -> None:
        text = "\n".join((
            event("consumer", "data-received", "7", 1000),
            event("consumer", "signature-validated", "8", 1100),
            event("consumer", "decrypted", "7", 1200),
            event("consumer", "reorder-ready", "7", 1300,
                  publication_cursor="7", media_sequence="7"),
        ))
        result = analyzer.analyze_texts([text], warmup_ms=0)
        stages = result["localClockStageMs"]
        self.assertNotIn("data-received->signature-validated", stages)
        self.assertIn("decrypted->reorder-ready", stages)
        self.assertEqual(stages["decrypted->reorder-ready"]["steady"]["samples"], 1)

    def test_one_to_many_fifo_outputs_are_rejected(self) -> None:
        text = "\n".join((
            event("consumer", "decoder-input", "9", 2000,
                  source_id="source-9", clock_domain="consumer-steady"),
            event("consumer", "decoder-output", "9", 2100,
                  frame_correlation="fifo", clock_domain="consumer-steady"),
            event("consumer", "decoder-output", "9", 2200,
                  frame_correlation="fifo", clock_domain="consumer-steady"),
        ))
        result = analyzer.analyze_texts([text], warmup_ms=0)
        self.assertNotIn("decoder-input->decoder-output",
                         result["localClockStageMs"])
        self.assertEqual(
            result["rejectedCorrelations"]["duplicate-or-ambiguous-stage"], 1)

    def test_exact_cardinality_and_phases_are_reported(self) -> None:
        lines = []
        for cursor, start in (("1", 1000), ("2", 3000), ("3", 9000)):
            lines.extend((
                event("consumer", "data-received", cursor, start,
                      clock_domain="consumer-steady"),
                event("consumer", "signature-validated", cursor, start + 100,
                      clock_domain="consumer-steady"),
            ))
        result = analyzer.analyze_texts(["\n".join(lines)], warmup_ms=5)
        phases = result["localClockStageMs"]["data-received->signature-validated"]
        self.assertEqual(phases["startup"]["samples"], 1)
        self.assertEqual(phases["warmup"]["samples"], 1)
        self.assertEqual(phases["steady"]["samples"], 1)

    def test_cross_clock_interval_is_unavailable(self) -> None:
        text = "\n".join((
            event("consumer", "data-received", "4", 1000,
                  clock_domain="consumer-a"),
            event("consumer", "signature-validated", "4", 1100,
                  clock_domain="consumer-b"),
        ))
        result = analyzer.analyze_texts([text])
        self.assertEqual(result["rejectedCorrelations"]["cross-clock-unavailable"], 1)
        self.assertEqual(result["crossClockOneWay"],
                         "unavailable-without-offset-uncertainty")

    def test_minindn_shared_clock_can_measure_data_delivery(self) -> None:
        text = "\n".join((
            event("provider", "data-put", "5", 1000),
            event("consumer", "data-received", "5", 2100),
        ))
        result = analyzer.analyze_texts(
            [text], warmup_ms=0, shared_monotonic_clock=True)
        measured = result["sharedHostClockStageMs"]["data-put->data-received"]
        self.assertEqual(measured["startup"]["p95"], 1.1)
        self.assertEqual(result["crossClockOneWay"],
                         "measured-with-shared-host-monotonic-clock")

    def test_exact_frame_trace_reports_capture_to_widget_without_summing(self) -> None:
        text = "\n".join((
            frame_event("provider", "source-acquired", 7, 1_000_000,
                        clock_domain="host-steady", capture_origin_ns="1000000000"),
            frame_event("provider", "encoded-output-ready", 7, 1_010_000,
                        clock_domain="host-steady"),
            frame_event("consumer", "decoder-output", 7, 1_020_000,
                        clock_domain="host-steady"),
            frame_event("consumer", "gui-delivered", 7, 1_025_000,
                        clock_domain="host-steady"),
        ))
        result = analyzer.analyze_texts([text], shared_monotonic_clock=True)
        frames = result["exactFrameTimeline"]
        self.assertEqual(frames["acceptedFrames"], 1)
        self.assertEqual(frames["captureToWidgetMs"]["mean"], 25.0)
        self.assertEqual(frames["captureToWidgetMs"]["p95"], 25.0)
        self.assertEqual(frames["captureToDecodeMs"]["p95"], 20.0)
        self.assertEqual(frames["decodeToWidgetMs"]["p95"], 5.0)

    def test_exact_coverage_excludes_provider_frames_outside_consumer_window(self) -> None:
        lines = []
        for source_id, capture_ns in ((6, 900_000_000), (7, 1_000_000_000),
                                      (8, 1_010_000_000), (9, 1_020_000_000),
                                      (10, 1_100_000_000)):
            lines.append(frame_event(
                "provider", "source-acquired", source_id, capture_ns // 1000,
                clock_domain="host-steady", capture_origin_ns=str(capture_ns)))
        for source_id, steady_us in ((7, 1_005_000), (9, 1_025_000)):
            lines.extend((
                frame_event("consumer", "decoder-output", source_id, steady_us,
                            clock_domain="host-steady"),
                frame_event("consumer", "gui-delivered", source_id, steady_us + 1000,
                            clock_domain="host-steady"),
            ))
        result = analyzer.analyze_texts(["\n".join(lines)])
        frames = result["exactFrameTimeline"]
        self.assertEqual(frames["acceptedFrames"], 2)
        self.assertEqual(frames["observedIdentities"], 3)
        self.assertAlmostEqual(frames["identityCoverage"], 2 / 3)


if __name__ == "__main__":
    unittest.main()
