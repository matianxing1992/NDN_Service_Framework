#!/usr/bin/env python3
"""Run frozen Spec 122 UAV video cells exactly once and preserve failures.

This driver intentionally has no retry path.  Every cell owns a unique output
directory and a terminal summary, including launch failures and invalid frame
identity.  Exploratory cells cannot be counted as confirmatory evidence.
"""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time


ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "Experiments/NDNSF_UAV_GUI_Minindn.py"
ANALYZER_PATH = ROOT / "Experiments/analyze_stream_latency.py"
SPEC = importlib.util.spec_from_file_location("stream_latency", ANALYZER_PATH)
assert SPEC and SPEC.loader
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)

CELLS = {
    "baseline-legacy-stdio": {
        "backend": "legacy-pipe", "readMode": "stdio-batched",
        "classification": "exploratory-baseline",
    },
    "baseline-provisional-posix": {
        "backend": "legacy-pipe", "readMode": "posix-time-bounded",
        "classification": "exploratory-baseline",
    },
    "candidate-gstreamer-exact": {
        "backend": "gstreamer", "readMode": "access-unit",
        "prefetchPolicy": "mapped-pressure",
        "classification": "exploratory-candidate",
    },
    "candidate-gstreamer-future-on": {
        "backend": "gstreamer", "readMode": "access-unit",
        "prefetchPolicy": "mapped-live-v1-future-on",
        "classification": "exploratory-candidate",
    },
}

DECODED_RE = re.compile(r"AUTO_VIDEO_GUI_RENDER_GATE status=PASS decoded_frames=(\d+)")
STARTUP_RE = re.compile(r"first_input_to_first_output_ms=(\d+)")


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def monitor_resources(stop: threading.Event, output: Path) -> None:
    with output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("timestamp_ms", "process", "pid", "cpu_percent", "rss_kib"))
        while not stop.wait(1.0):
            result = subprocess.run(
                ["ps", "-C", "UavDroneApp,UavGroundStationApp,nfd", "-o",
                 "comm=,pid=,%cpu=,rss="], text=True, capture_output=True)
            for line in result.stdout.splitlines():
                fields = line.split()
                if len(fields) == 4:
                    writer.writerow((round(time.time() * 1000), *fields))
            stream.flush()


def summarize(cell: str, definition: dict[str, str], output: Path,
              return_code: int, started: str, ended: str) -> dict[str, object]:
    logs = [output / "drone.log", output / "ground-station.log"]
    texts = [path.read_text(encoding="utf-8", errors="replace")
             for path in logs if path.exists()]
    joined = "\n".join(texts)
    latency = analyzer.analyze_texts(
        texts, warmup_ms=5000, shared_monotonic_clock=True)
    exact = latency["exactFrameTimeline"]
    decoded_match = DECODED_RE.search(joined)
    startup_match = STARTUP_RE.search(joined)
    security_failures = sum(joined.count(token) for token in (
        "VIDEO_AUTHENTICATION_FAILED", "VIDEO_PROTECTED_PUBLICATION_FAILED",
        "FRAME_BINDING_REJECT", "nonce reuse"))
    identity_valid = exact["acceptedFrames"] > 0 and exact["identityCoverage"] >= 0.99
    terminal = "PASS" if return_code == 0 and identity_valid and security_failures == 0 else (
        "INVALID_IDENTITY" if return_code == 0 and not identity_valid else "FAILED")
    return {
        "schemaVersion": 1,
        "cell": cell,
        "classification": definition["classification"],
        "configuration": definition,
        "startedAt": started,
        "endedAt": ended,
        "returnCode": return_code,
        "terminalStatus": terminal,
        "identityGate": {
            "valid": identity_valid,
            "acceptedFrames": exact["acceptedFrames"],
            "coverage": exact["identityCoverage"],
        },
        "decodedFrames": int(decoded_match.group(1)) if decoded_match else 0,
        "decoderStartupMs": int(startup_match.group(1)) if startup_match else None,
        "securityFailures": security_failures,
        "latency": latency,
        "physicalPresentation": "unavailable",
        "rerunAllowed": False,
    }


def run_cell(cell: str, output_root: Path, duration: int) -> dict[str, object]:
    definition = CELLS[cell]
    output = (output_root / cell).resolve()
    if output.exists():
        raise RuntimeError(f"refusing to replace existing terminal cell: {output}")
    output.mkdir(parents=True)
    env = os.environ.copy()
    env.update({
        "NDNSF_TIMELINE_TRACE": "1",
        "NDNSF_TIMELINE_TRACE_SAMPLE_RATE": "10",
        # Exact frame traces remain enabled, but high-rate cursor traces would
        # otherwise perturb the 12+1 packet, 30-fps load being measured.
        "NDNSF_STREAM_PACKET_TIMELINE_TRACE": "0",
        "NDNSF_APP_NDN_LOG": (
            "ndn_service_framework.*=WARN:"
            "ndn_service_framework.examples.*=INFO:"
            "nacabe.*=WARN:ndnsvs.*=WARN:ndnsd.*=WARN:"
            "ndn_service_framework.TimelineTrace=DEBUG"),
    })
    if definition["backend"] == "gstreamer":
        env["NDNSF_UAV_VIDEO_PIPELINE"] = "gstreamer"
        env["NDNSF_UAV_GSTREAMER_SOURCE"] = "videotestsrc"
    else:
        env["NDNSF_UAV_VIDEO_PIPELINE"] = "legacy-pipe"
        env["NDNSF_UAV_ENCODER_PIPE_READ_MODE"] = definition["readMode"]
    command = [
        "sudo", "-n", "-E", "xvfb-run", "-a", "python3", str(LAUNCHER),
        "--auto-video-test", "--auto-stop-seconds", str(duration), "--no-cli",
        "--camera-mode", "file", "--no-virtual-camera", "--drone-headless",
        "--live-stream-prefetch-policy",
        definition.get("prefetchPolicy", "mapped-pressure"),
        "--output-dir", str(output),
    ]
    (output / "cell-definition.json").write_text(json.dumps({
        "cell": cell, "definition": definition, "durationSeconds": duration,
        "command": command,
    }, indent=2) + "\n")
    started = utc_now()
    stop = threading.Event()
    monitor = threading.Thread(
        target=monitor_resources, args=(stop, output / "process-resources.csv"),
        daemon=True)
    monitor.start()
    with (output / "launcher.log").open("w", encoding="utf-8") as log:
        try:
            completed = subprocess.run(
                command, cwd=ROOT, env=env, stdout=log,
                stderr=subprocess.STDOUT, timeout=duration + 90)
            return_code = completed.returncode
        except subprocess.TimeoutExpired:
            return_code = 124
    stop.set()
    monitor.join(timeout=3)
    summary = summarize(cell, definition, output, return_code, started, utc_now())
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cell", choices=tuple(CELLS), action="append", required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--duration-seconds", type=int, default=60)
    args = parser.parse_args()
    if args.duration_seconds < 60:
        raise SystemExit("Spec 122 performance cells require at least 60 seconds")
    summaries = []
    for cell in args.cell:
        summaries.append(run_cell(cell, args.output_root, args.duration_seconds))
    args.output_root.mkdir(parents=True, exist_ok=True)
    (args.output_root / "matrix-summary.json").write_text(
        json.dumps({"cells": summaries}, indent=2) + "\n")
    return 0 if all(value["returnCode"] == 0 for value in summaries) else 1


if __name__ == "__main__":
    raise SystemExit(main())
