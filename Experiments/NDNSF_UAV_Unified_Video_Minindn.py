#!/usr/bin/env python3
"""Run one immutable Spec 120 UAV video acceptance candidate in MiniNDN.

Each invocation owns exactly one output directory.  A failed invocation is
terminal evidence and must be followed by a new candidate identity rather than
reusing or deleting its directory.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
from pathlib import Path
import re
import resource
import sqlite3
import statistics
import subprocess
import sys
import time

from analyze_stream_latency import analyze_texts


REPO = Path(__file__).resolve().parents[1]
LAUNCHER = REPO / "Experiments/NDNSF_UAV_GUI_Minindn.py"
FIELD_RE = re.compile(r"([A-Za-z_]+)=([^\s]+)")
DECODED_RE = re.compile(r"(?:GS_DECODED_FRAMES count=|decoded_frames=)(\d+)")
DECODER_STARTUP_RE = re.compile(r"GS_VIDEO_DECODER_STARTUP.*first_input_to_first_output_ms=(\d+)")
DECODER_CADENCE_RE = re.compile(
    r"GS_VIDEO_OUTPUT_CADENCE.*samples=(\d+).*p50_ms=(\d+)"
    r".*p95_ms=(\d+).*p99_ms=(\d+)")
GUI_DELIVERY_RE = re.compile(r"GS_VIDEO_GUI_DELIVERY.*decoder_callback_to_gui_ms=(\d+)")


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round((len(ordered) - 1) * fraction))))
    return float(ordered[index])


def distribution(values: list[float]) -> dict[str, float | int | None]:
    return {
        "samples": len(values),
        "p50": percentile(values, 0.50),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
    }


def timeline_summary(*texts: str) -> dict[str, object]:
    # All MiniNDN nodes in this launcher are processes on one Linux host, so
    # CLOCK_MONOTONIC has one authority. Real multi-host deployments must not
    # enable this without a bounded clock-offset contract.
    result = analyze_texts(texts, shared_monotonic_clock=True)
    # Preserve the established summary spelling for downstream readers.
    result["sampledCursors"] = result["sampledIdentities"]
    return result


def pit_summary(path: Path) -> dict[str, float | int | None]:
    if not path.exists():
        return {"samples": 0, "meanEntries": None, "maxEntries": None}
    with path.open(newline="", encoding="utf-8") as source:
        values = [int(row["pit_entries"]) for row in csv.DictReader(source)]
    return {
        "samples": len(values),
        "meanEntries": statistics.fmean(values) if values else None,
        "maxEntries": max(values) if values else None,
    }


def topology_text(loss: int) -> str:
    return ("[nodes]\nmemphis:\nucla:\n\n[links]\n"
            f"memphis:ucla delay=1ms bw=1000 loss={loss}\n")


def fields(line: str) -> dict[str, str]:
    return dict(FIELD_RE.findall(line))


def last_fields(text: str, marker: str) -> dict[str, str]:
    matched = [fields(line) for line in text.splitlines() if marker in line]
    return matched[-1] if matched else {}


def repo_counts(path: Path) -> dict[str, int]:
    if not path.exists():
        return {}
    # Read the WAL too: a failed candidate may terminate before SQLite has
    # checkpointed it into the main database file.
    uri = f"file:{path.resolve()}?mode=ro"
    connection = sqlite3.connect(uri, uri=True)
    try:
        try:
            return {
                str(kind): int(count)
                for kind, count in connection.execute(
                    "SELECT object_type, COUNT(*) FROM objects GROUP BY object_type"
                )
            }
        except sqlite3.OperationalError as error:
            if "no such table" not in str(error):
                raise
            return {"__repo_schema_unavailable__": 1}
    finally:
        connection.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", required=True,
                        choices=("live-only", "recording-only", "live-and-record",
                                 "late-start", "certificate-rotation-replay"))
    parser.add_argument("--loss", type=int, choices=(0, 5), default=0)
    parser.add_argument("--duration-seconds", type=int, default=60)
    parser.add_argument("--output", required=True)
    parser.add_argument("--inject-storage-failure", action="store_true")
    parser.add_argument("--trace", choices=("off", "on"), default="off")
    parser.add_argument(
        "--trace-sample-denominator", type=int, default=50,
        help="stable cursor-sampling denominator when --trace=on (default: 50 = 2%%)")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--reparse-existing", action="store_true")
    args = parser.parse_args()
    if args.duration_seconds < 60 and not args.dry_run:
        parser.error("Spec 120 acceptance requires a measured window of at least 60 seconds")
    if args.trace_sample_denominator < 1:
        parser.error("--trace-sample-denominator must be positive")

    output = Path(args.output).resolve()
    if output.exists() and any(output.iterdir()) and not args.reparse_existing:
        parser.error(f"refusing nonempty candidate directory: {output}")
    output.mkdir(parents=True, exist_ok=True)
    topology = output / "topology.conf"
    topology.write_text(topology_text(args.loss), encoding="utf-8")

    recording_mode = args.mode != "live-only"
    video_bitrate = "1200"
    video_width = "320"
    command = [
        "sudo", "-n", "-E", "timeout", f"{args.duration_seconds + 150}s",
        sys.executable, str(LAUNCHER),
        "--topology-file", str(topology), "--controller-node", "memphis",
        "--gs-node", "memphis", "--drone-node", "ucla", "--drone-headless",
        "--camera-mode", "file", "--no-virtual-camera",
        "--flight-controller-backend", "mock", "--no-start-jmavsim",
        "--no-cli", "--no-xhost", "--nfd-log-level", "WARN",
        "--video-bitrate-kbps", video_bitrate, "--video-width", video_width,
        "--video-fec-parity-shards", "1",
        "--live-stream-prefetch-policy", "mapped-pressure",
        "--output-dir", str(output),
    ]
    env = dict(os.environ)
    env["NDNSF_APP_NDN_LOG"] = (
        "ndn_service_framework.examples.UavGroundStationApp=DEBUG:"
        "ndn_service_framework.examples.UavDroneApp=INFO:"
        "ndn_service_framework.TimelineTrace=DEBUG"
    )
    if args.trace == "on":
        env["NDNSF_TIMELINE_TRACE"] = "1"
        env["NDNSF_TIMELINE_TRACE_SAMPLE_RATE"] = str(
            args.trace_sample_denominator)
    else:
        env.pop("NDNSF_TIMELINE_TRACE", None)
        env["NDNSF_TIMELINE_TRACE_SAMPLE_RATE"] = "50"
    if recording_mode:
        env["NDNSF_UAV_RECORDING_BITRATE_KBPS"] = video_bitrate
        env["NDNSF_UAV_RECORDING_FRAME_WIDTH"] = video_width

    if args.mode == "live-only":
        command += ["--auto-video-test", "--auto-stop-seconds",
                    str(args.duration_seconds), "--auto-start-delay-ms", "1000"]
    elif args.mode == "live-and-record":
        command += ["--auto-video-test", "--camera-record-during-video-test",
                    "--camera-retention-packet-limit", "30000",
                    "--auto-stop-seconds", str(args.duration_seconds),
                    "--auto-start-delay-ms", "1000"]
        env["NDNSF_UAV_FINALIZE_RETENTION_AFTER_VIDEO_TEST"] = "1"
    else:
        command += ["--auto-recording-playback-test",
                    "--camera-retention-packet-limit", "30000",
                    "--auto-stop-seconds", str(args.duration_seconds)]
        env["NDNSF_UAV_RECORDING_PLAYBACK_DELAY_SECONDS"] = str(args.duration_seconds)
        if args.mode == "late-start":
            env["NDNSF_UAV_RECORDING_LIFECYCLE_TEST"] = "1"
        if args.mode == "certificate-rotation-replay":
            env["NDNSF_UAV_ARCHIVED_TRUST_REPLAY_TEST"] = "1"
    if args.inject_storage_failure:
        env["NDNSF_UAV_SIMULATE_STORAGE_FAILURE_AFTER_PACKETS"] = "80"

    metadata = {
        "candidate": output.name, "mode": args.mode, "lossPercent": args.loss,
        "durationSeconds": args.duration_seconds, "trace": args.trace,
        "traceSampleDenominator": (
            args.trace_sample_denominator if args.trace == "on" else None),
        "storageFailureInjected": args.inject_storage_failure,
        "command": command,
    }
    (output / "candidate.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    if args.dry_run:
        print(json.dumps(metadata, indent=2))
        return 0

    if args.reparse_existing:
        prior = json.loads((output / "run-summary.json").read_text(encoding="utf-8"))
        returncode = int(prior.get("returncode", 1))
        elapsed = float(prior.get("elapsedSeconds", 0.0))
    else:
        started = time.monotonic()
        resource_before = resource.getrusage(resource.RUSAGE_CHILDREN)
        with (output / "campaign-launcher.log").open("w", encoding="utf-8") as log:
            result = subprocess.run(command, cwd=REPO, env=env, stdout=log,
                                    stderr=subprocess.STDOUT, check=False)
        resource_after = resource.getrusage(resource.RUSAGE_CHILDREN)
        returncode = result.returncode
        elapsed = time.monotonic() - started
        resource_usage = {
            "userCpuSeconds": resource_after.ru_utime - resource_before.ru_utime,
            "systemCpuSeconds": resource_after.ru_stime - resource_before.ru_stime,
            "maxRssKiB": resource_after.ru_maxrss,
        }
    if args.reparse_existing:
        resource_usage = prior.get("resourceUsage", {
            "userCpuSeconds": None, "systemCpuSeconds": None, "maxRssKiB": None,
        })
    gs_text = (output / "ground-station.log").read_text(
        encoding="utf-8", errors="replace") if (output / "ground-station.log").exists() else ""
    drone_text = (output / "drone.log").read_text(
        encoding="utf-8", errors="replace") if (output / "drone.log").exists() else ""
    provider = last_fields(drone_text, "VIDEO_LIVE_STREAM_CORE_FINAL")
    if not provider:
        provider = last_fields(drone_text, "VIDEO_LIVE_STREAM_CORE_STATUS")
    eligible = max(0, int(provider.get("provider_future_interests", "0")) -
                   int(provider.get("pending_interests", "0")))
    hits = int(provider.get("provider_future_hits", "0"))
    decoded = max((int(value) for value in DECODED_RE.findall(gs_text)), default=0)
    startup_matches = DECODER_STARTUP_RE.findall(gs_text)
    cadence_matches = DECODER_CADENCE_RE.findall(gs_text)
    gui_delays = [float(value) for value in GUI_DELIVERY_RE.findall(gs_text)]
    decoder_process = {
        "firstInputToFirstOutputMs": (
            int(startup_matches[-1]) if startup_matches else None),
        "outputIntervalMs": {
            "samples": 0, "p50": None, "p95": None, "p99": None,
        },
    }
    if cadence_matches:
        samples, p50, p95, p99 = cadence_matches[-1]
        decoder_process["outputIntervalMs"] = {
            "samples": int(samples), "p50": float(p50),
            "p95": float(p95), "p99": float(p99),
        }
    repo = repo_counts(output / "drone-A-camera-recording.sqlite3")
    summary = {
        **metadata, "returncode": returncode, "elapsedSeconds": elapsed,
        "decodedFrames": decoded, "providerFutureEligible": eligible,
        "providerFutureHits": hits,
        "providerFutureHitRatio": hits / eligible if eligible else None,
        "repoObjectCounts": repo,
        "encodedOutputToDecoderOutputMs": {
            "samples": 0, "p50": None, "p95": None, "p99": None,
            "unavailableReason": "h264-input-group-to-decoded-frame-cardinality-ambiguous",
        },
        "decoderProcess": decoder_process,
        "decoderCallbackToGuiMs": distribution(gui_delays),
        "timeline": timeline_summary(drone_text, gs_text),
        "resourceUsage": resource_usage,
        "pit": pit_summary(output / "nfd-pit-samples.csv"),
        "retentionFailures": drone_text.count("CAMERA_CANONICAL_RETENTION_FAILED"),
        "retentionStarts": drone_text.count("CAMERA_CANONICAL_RETENTION_STARTED"),
        "manifestComplete": (
            "complete=true" in gs_text or
            ("CAMERA_CANONICAL_RETENTION_FINALIZED" in drone_text and
             "complete=true" in drone_text)
        ),
        "canonicalReplay": "Canonical recording replay drone=A" in gs_text,
        "canonicalStorage": "CAMERA_RECORDING_CANONICAL drone=A" in drone_text,
        "certificateRotationObserved": (
            "CAMERA_ARCHIVED_TRUST_CERTIFICATE_ROTATED" in drone_text and
            "archived_packets=unchanged" in drone_text
        ),
        "legacyWriterObserved": "recordRawChunk" in drone_text,
        "status": "PASS" if returncode == 0 and decoded > 0 else "FAIL",
    }
    if args.mode in ("recording-only", "late-start", "certificate-rotation-replay"):
        rotation_ok = (summary["certificateRotationObserved"]
                       if args.mode == "certificate-rotation-replay" else True)
        summary["status"] = "PASS" if (
            returncode == 0 and decoded > 0 and summary["manifestComplete"] and
            summary["canonicalReplay"] and summary["canonicalStorage"] and
            rotation_ok
        ) else "FAIL"
    if args.mode == "live-and-record":
        retention_ok = (
            summary["retentionFailures"] > 0 and not summary["manifestComplete"]
            if args.inject_storage_failure else
            summary["retentionFailures"] == 0 and summary["manifestComplete"]
        )
        summary["status"] = "PASS" if (
            returncode == 0 and decoded > 0 and summary["canonicalStorage"] and
            retention_ok and not summary["legacyWriterObserved"]
        ) else "FAIL"
    if args.trace == "on" and summary["timeline"]["sampledCursors"] == 0:
        summary["status"] = "FAIL"
    (output / "run-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
