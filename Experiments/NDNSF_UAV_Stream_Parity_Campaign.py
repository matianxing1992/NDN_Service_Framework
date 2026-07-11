#!/usr/bin/env python3
"""Run matched UAV video stream parity acceptance campaigns in MiniNDN."""

from __future__ import annotations

import argparse
import csv
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
from typing import Any


REPO = Path(__file__).resolve().parents[1]
LAUNCHER = REPO / "Experiments/NDNSF_UAV_GUI_Minindn.py"
DEFAULT_TOPOLOGY = REPO / "Experiments/Topology/UAV_Stream_Parity_5pct.conf"
ADAPTIVE_MARKER = "GS_VIDEO_ADAPTIVE_STATE"
FIELD_RE = re.compile(r"([a-z_]+)=([^\s]+)")


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(len(ordered) - 1, lower + 1)
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def fields_from_line(line: str) -> dict[str, str]:
    return {key: value for key, value in FIELD_RE.findall(line)}


def parse_run(run_dir: Path, returncode: int, command: list[str]) -> dict[str, Any]:
    gs_log = run_dir / "ground-station.log"
    text = gs_log.read_text(encoding="utf-8", errors="replace") if gs_log.exists() else ""
    snapshots = [
        fields_from_line(line)
        for line in text.splitlines()
        if ADAPTIVE_MARKER in line
    ]

    def integer_values(field: str) -> list[int]:
        values: list[int] = []
        for snapshot in snapshots:
            try:
                values.append(int(snapshot.get(field, "0")))
            except ValueError:
                continue
        return values

    rtts = [float(value) for value in integer_values("rtt_ms")]
    pending_chunks = integer_values("pending_chunks")
    pending_bytes = integer_values("pending_bytes")
    decoded_gaps = integer_values("decoded_frame_gap")
    timeouts = integer_values("timeouts")
    nacks = integer_values("nacks")
    duplicates = integer_values("duplicates")
    fec_recovered = integer_values("fec_recovered_chunks")
    completion = (
        returncode == 0 and
        "GS_DECODED_FRAMES count=30" in text and
        "GS_GUI_EXIT rc=0" in text
    )
    return {
        "runDirectory": str(run_dir),
        "returncode": returncode,
        "command": command,
        "completion": completion,
        "adaptiveSnapshotCount": len(snapshots),
        "staleSessionRejectCount": text.count("GS_VIDEO_STALE_SESSION_"),
        "staleStreamRejectCount": text.count("GS_VIDEO_STALE_STREAM_"),
        "fecRecoveryLogCount": text.count("GS_VIDEO_FEC_RECOVERED"),
        "fecRecoveredChunks": max(fec_recovered, default=0),
        "maxPendingChunks": max(pending_chunks, default=0),
        "maxPendingBytes": max(pending_bytes, default=0),
        "maxDecodedFrameGap": max(decoded_gaps, default=0),
        "maxTimeouts": max(timeouts, default=0),
        "maxNacks": max(nacks, default=0),
        "maxDuplicates": max(duplicates, default=0),
        "rttP50Ms": percentile(rtts, 0.50),
        "rttP95Ms": percentile(rtts, 0.95),
    }


def write_csv(path: Path, runs: list[dict[str, Any]]) -> None:
    fields = [
        "runDirectory", "returncode", "completion", "adaptiveSnapshotCount",
        "staleSessionRejectCount", "staleStreamRejectCount",
        "fecRecoveryLogCount", "fecRecoveredChunks", "maxPendingChunks",
        "maxPendingBytes", "maxDecodedFrameGap", "maxTimeouts", "maxNacks",
        "maxDuplicates", "rttP50Ms", "rttP95Ms",
    ]
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for run in runs:
            writer.writerow({field: run.get(field, "") for field in fields})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--topology", default=str(DEFAULT_TOPOLOGY))
    parser.add_argument("--auto-stop-seconds", type=int, default=8)
    parser.add_argument("--max-pending-chunks", type=int, default=48)
    parser.add_argument("--max-pending-bytes", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.runs < 1:
        raise SystemExit("--runs must be positive")

    out_dir = Path(args.out).expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    topology = Path(args.topology).expanduser().resolve()
    runs: list[dict[str, Any]] = []

    for index in range(1, args.runs + 1):
        run_dir = out_dir / f"run-{index:02d}"
        command = [
            "sudo", "-n", "-E", "timeout", "120s", "xvfb-run", "-a",
            sys.executable, str(LAUNCHER),
            "--topology-file", str(topology),
            "--controller-node", "memphis",
            "--gs-node", "memphis",
            "--drone-node", "ucla",
            "--drone-headless",
            "--camera-mode", "file",
            "--no-virtual-camera",
            "--auto-video-test",
            "--auto-stop-seconds", str(args.auto_stop_seconds),
            "--auto-start-delay-ms", "1000",
            "--video-bitrate-kbps", "1200",
            "--video-width", "320",
            "--output-dir", str(run_dir),
            "--nfd-log-level", "WARN",
            "--no-cli",
            "--no-xhost",
        ]
        if args.dry_run:
            runs.append({"runDirectory": str(run_dir), "command": command})
            continue
        run_dir.mkdir(parents=True, exist_ok=True)
        with (run_dir / "campaign-launcher.log").open("w", encoding="utf-8") as output:
            completed = subprocess.run(
                command,
                cwd=str(REPO),
                env=dict(os.environ),
                stdout=output,
                stderr=subprocess.STDOUT,
                check=False,
            )
        runs.append(parse_run(run_dir, completed.returncode, command))

    if args.dry_run:
        summary = {"status": "DRY_RUN", "topology": str(topology), "runs": runs}
    else:
        completed_runs = sum(bool(run["completion"]) for run in runs)
        bounded = all(
            int(run["maxPendingChunks"]) <= args.max_pending_chunks and
            int(run["maxPendingBytes"]) <= args.max_pending_bytes
            for run in runs
        )
        summary = {
            "status": "SUCCESS" if completed_runs == args.runs and bounded else "FAILURE",
            "topology": str(topology),
            "lossPercent": 5,
            "runCount": args.runs,
            "completedRuns": completed_runs,
            "boundedBuffering": bounded,
            "maxPendingChunksThreshold": args.max_pending_chunks,
            "maxPendingBytesThreshold": args.max_pending_bytes,
            "aggregate": {
                "meanRttP50Ms": statistics.fmean(run["rttP50Ms"] for run in runs),
                "meanRttP95Ms": statistics.fmean(run["rttP95Ms"] for run in runs),
                "fecRecoveryLogCount": sum(run["fecRecoveryLogCount"] for run in runs),
                "maxPendingChunks": max(run["maxPendingChunks"] for run in runs),
                "maxPendingBytes": max(run["maxPendingBytes"] for run in runs),
                "maxDecodedFrameGap": max(run["maxDecodedFrameGap"] for run in runs),
                "staleRejectCount": sum(
                    run["staleSessionRejectCount"] + run["staleStreamRejectCount"]
                    for run in runs
                ),
            },
            "runs": runs,
        }
        write_csv(out_dir / "campaign-summary.csv", runs)

    (out_dir / "campaign-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary["status"] in {"SUCCESS", "DRY_RUN"} else 1


if __name__ == "__main__":
    raise SystemExit(main())
