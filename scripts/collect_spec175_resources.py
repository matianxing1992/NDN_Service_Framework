#!/usr/bin/env python3
"""Collect bounded process, GPU, and host-network samples for Spec175 G7."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time


SCHEMA = "ndnsf-di-spec175-resource-sample-v1"
_STOP = False


def _stop(_signum, _frame) -> None:
    global _STOP
    _STOP = True


def _proc_rows() -> dict[int, tuple[int, int, int]]:
    rows: dict[int, tuple[int, int, int]] = {}
    page_size = os.sysconf("SC_PAGE_SIZE")
    ticks = os.sysconf("SC_CLK_TCK")
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text(encoding="utf-8").split()
            rss_pages = int((entry / "statm").read_text(
                encoding="utf-8").split()[1])
            rows[int(entry.name)] = (
                int(stat[3]), rss_pages * page_size,
                int((int(stat[13]) + int(stat[14])) / ticks * 1_000_000_000),
            )
        except (OSError, IndexError, ValueError):
            continue
    return rows


def _descendants(parent: int, rows: dict[int, tuple[int, int, int]]) -> set[int]:
    selected = {parent}
    changed = True
    while changed:
        changed = False
        for pid, (ppid, _rss, _cpu) in rows.items():
            if ppid in selected and pid not in selected:
                selected.add(pid)
                changed = True
    selected.discard(os.getpid())
    return selected


def _network() -> tuple[int, int]:
    rx = tx = 0
    for line in Path("/proc/net/dev").read_text(encoding="utf-8").splitlines()[2:]:
        interface, fields = line.split(":", 1)
        if interface.strip() == "lo":
            continue
        values = fields.split()
        rx += int(values[0])
        tx += int(values[8])
    return rx, tx


def _gpus() -> tuple[list[dict[str, object]], str]:
    command = [
        "nvidia-smi", "--query-gpu=index,uuid,utilization.gpu,"
        "memory.used,memory.total,power.draw",
        "--format=csv,noheader,nounits",
    ]
    try:
        completed = subprocess.run(
            command, check=False, text=True, capture_output=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return [], f"{type(exc).__name__}:{exc}"
    if completed.returncode != 0:
        return [], f"nvidia-smi:{completed.returncode}:{completed.stderr.strip()}"
    result = []
    for line in completed.stdout.splitlines():
        fields = [value.strip() for value in line.split(",")]
        if len(fields) != 6:
            return [], "nvidia-smi:malformed-row"
        try:
            result.append({
                "index": int(fields[0]), "uuid": fields[1],
                "utilizationPercent": float(fields[2]),
                "memoryUsedMiB": float(fields[3]),
                "memoryTotalMiB": float(fields[4]),
                "powerDrawW": float(fields[5]),
            })
        except ValueError:
            return [], "nvidia-smi:non-numeric-row"
    return result, ""


def collect(parent_pid: int) -> dict[str, object]:
    rows = _proc_rows()
    pids = _descendants(parent_pid, rows)
    try:
        network_rx, network_tx = _network()
        network_error = ""
    except (OSError, ValueError, IndexError) as exc:
        network_rx = network_tx = 0
        network_error = f"{type(exc).__name__}:{exc}"
    gpus, gpu_error = _gpus()
    return {
        "schemaVersion": SCHEMA,
        "monotonicMs": time.monotonic() * 1000.0,
        "epochMs": time.time() * 1000.0,
        "parentPid": parent_pid,
        "processCount": len(pids),
        "rssBytes": sum(rows[pid][1] for pid in pids if pid in rows),
        "cpuTimeNs": sum(rows[pid][2] for pid in pids if pid in rows),
        "networkRxBytes": network_rx,
        "networkTxBytes": network_tx,
        "networkScope": "compute-node-non-loopback",
        "gpus": gpus,
        "gpuError": gpu_error,
        "networkError": network_error,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--parent-pid", required=True, type=int)
    parser.add_argument("--interval-ms", type=int, default=500)
    args = parser.parse_args()
    if args.parent_pid <= 1 or not 100 <= args.interval_ms <= 10_000:
        parser.error("invalid parent PID or interval")
    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)
    output = args.output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as stream:
        while True:
            stream.write(json.dumps(
                collect(args.parent_pid), sort_keys=True,
                separators=(",", ":")) + "\n")
            stream.flush()
            if _STOP:
                break
            deadline = time.monotonic() + args.interval_ms / 1000.0
            while not _STOP and time.monotonic() < deadline:
                time.sleep(min(0.1, deadline - time.monotonic()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
