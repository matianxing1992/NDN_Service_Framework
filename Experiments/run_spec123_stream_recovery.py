#!/usr/bin/env python3
"""Run one immutable Spec 123 UAV live-recovery acceptance cell."""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
RUNNER_PATH = ROOT / "Experiments/run_spec122_video_latency_matrix.py"
SPEC = importlib.util.spec_from_file_location("spec122_runner", RUNNER_PATH)
assert SPEC and SPEC.loader
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

DECODER_OUTPUT_RE = re.compile(r"event=decoder-output steady_us=(\d+)")
SOURCE_RE = re.compile(r"role=provider event=source-acquired steady_us=(\d+)")
GS_STATUS_RE = re.compile(
    r"GS_VIDEO_CORE_STATUS .*delivered=(\d+).*mapping_interests=(\d+) "
    r"payload_interests=(\d+).*in_flight=(\d+)")
PROVIDER_STATUS_RE = re.compile(
    r"VIDEO_LIVE_STREAM_CORE_STATUS .*pending_interests=(\d+).*"
    r"provider_future_interests=(\d+).*provider_future_hits=(\d+).*"
    r"latest_produced_cursor=(\d+).*mapping_committed_through_cursor=(\d+)")

# Original-load acceptance is 12 source Data plus one repair Data per decoded
# frame. Mapping Interests are control overhead, so the gate permits at most
# 15% work above those 13 necessary items. The old 12.65 constant was below the
# payload contract itself and could reject a correct run.
CONFIGURED_ITEMS_PER_FRAME = 13
MAX_INTEREST_WORK_PER_FRAME = CONFIGURED_ITEMS_PER_FRAME * 1.15


def last_match(pattern: re.Pattern[str], text: str):
    matches = list(pattern.finditer(text))
    return matches[-1] if matches else None


def augment_acceptance(summary: dict[str, object], output: Path,
                       duration_seconds: int) -> dict[str, object]:
    drone = (output / "drone.log").read_text(encoding="utf-8", errors="replace")
    ground = (output / "ground-station.log").read_text(
        encoding="utf-8", errors="replace")
    source_times = [int(value) for value in SOURCE_RE.findall(drone)]
    decoded_times = [int(value) for value in DECODER_OUTPUT_RE.findall(ground)]
    start = min(source_times) if source_times else 0
    bucket_count = duration_seconds // 5
    buckets = [0] * bucket_count
    for timestamp in decoded_times:
        if start and timestamp >= start:
            bucket = min(bucket_count - 1, (timestamp - start) // 5_000_000)
            buckets[int(bucket)] += 1

    gs = last_match(GS_STATUS_RE, ground)
    provider = last_match(PROVIDER_STATUS_RE, drone)
    delivered, mapping, payload, in_flight = (
        tuple(map(int, gs.groups())) if gs else (0, 0, 0, 0))
    pending, future, hits, produced, mapped = (
        tuple(map(int, provider.groups())) if provider else (0, 0, 0, 0, 0))
    decoded_frames = int(summary.get("decodedFrames", 0))
    exact = summary["latency"]["exactFrameTimeline"]
    capture_decode = exact["captureToDecodeMs"]
    future_ratio = hits / future if future else 0.0
    work = (mapping + payload) / decoded_frames if decoded_frames else None
    checks = {
        "allFiveSecondBucketsActive": all(value > 0 for value in buckets),
        "finalTenSecondsActive": all(value > 0 for value in buckets[-2:]),
        "terminalFuturePendingEmpty": pending == 0,
        "terminalConsumerInFlightEmpty": in_flight == 0,
        "captureToDecodeP95AtMost250Ms":
            capture_decode.get("p95") is not None and capture_decode["p95"] <= 250.0,
        "captureToDecodeP99AtMost500Ms":
            capture_decode.get("p99") is not None and capture_decode["p99"] <= 500.0,
        "futureHitRatioAtLeast99Percent": future_ratio >= 0.99,
        "interestWorkWithinConfiguredPacketBudget":
            work is not None and work <= MAX_INTEREST_WORK_PER_FRAME,
        "identityCoverageAtLeast99Percent": exact["identityCoverage"] >= 0.99,
    }
    summary["classification"] = "spec123-acceptance"
    summary["spec123Acceptance"] = {
        "fiveSecondDecodedSampleBuckets": buckets,
        "finalConsumer": {
            "delivered": delivered, "mappingInterests": mapping,
            "payloadInterests": payload, "inFlight": in_flight,
        },
        "finalProvider": {
            "pendingInterests": pending, "futureInterests": future,
            "futureHits": hits, "latestProducedCursor": produced,
            "mappingCommittedThroughCursor": mapped,
        },
        "futureHitRatio": future_ratio,
        "interestWorkPerDecodedFrame": work,
        "checks": checks,
        "passed": all(checks.values()),
    }
    summary["terminalStatus"] = "PASS" if all(checks.values()) else "FAILED_GATE"
    summary["rerunAllowed"] = False
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--duration-seconds", type=int, default=60)
    args = parser.parse_args()
    if args.duration_seconds < 60:
        raise SystemExit("Spec 123 acceptance requires at least 60 seconds")
    runner.CELLS["candidate-gstreamer-future-on"]["classification"] = (
        "spec123-acceptance")
    summary = runner.run_cell(
        "candidate-gstreamer-future-on", args.output_root, args.duration_seconds)
    output = (args.output_root / "candidate-gstreamer-future-on").resolve()
    summary = augment_acceptance(summary, output, args.duration_seconds)
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    (args.output_root / "matrix-summary.json").write_text(
        json.dumps({"cells": [summary]}, indent=2) + "\n")
    return 0 if summary["spec123Acceptance"]["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
