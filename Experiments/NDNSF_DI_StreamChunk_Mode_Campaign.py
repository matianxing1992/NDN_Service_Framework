#!/usr/bin/env python3
"""Compare raw and StreamChunk NDNSF-DI dependency envelope modes.

This wrapper intentionally reuses the canonical NativeTracer MiniNDN harness.
It does not reimplement topology, identity, controller, provider, or user
logic; it only runs the same workload in two dependency-envelope modes and
summarizes the resulting evidence directories.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


REPO = Path(__file__).resolve().parents[1]
HARNESS = REPO / "Experiments/NDNSF_DI_NativeTracer_Minindn.py"


def parse_modes(text: str) -> list[str]:
    modes: list[str] = []
    for item in text.split(","):
        mode = item.strip()
        if not mode:
            continue
        if mode not in {"raw", "streamchunk"}:
            raise argparse.ArgumentTypeError(
                "dependency envelope modes must be raw or streamchunk")
        modes.append(mode)
    if not modes:
        raise argparse.ArgumentTypeError("at least one mode is required")
    return modes


def load_json(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def output_hash(summary: dict[str, Any]) -> str:
    user = summary.get("userExecution", {})
    requests: list[dict[str, Any]] = []
    if isinstance(user, dict):
        for request in user.get("requests", []) or []:
            if not isinstance(request, dict):
                continue
            requests.append({
                "status": request.get("status", ""),
                "responseStatus": bool(request.get("responseStatus", False)),
                "payloadBytes": int(request.get("payloadBytes", 0) or 0),
                "error": request.get("error", ""),
            })
    payload = {
        "requests": requests,
        "payloadBytes": user.get("payloadBytes", 0) if isinstance(user, dict) else 0,
        "successCount": user.get("successCount", 0) if isinstance(user, dict) else 0,
        "failureCount": user.get("failureCount", 0) if isinstance(user, dict) else 0,
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def point_summary(point_dir: Path,
                  mode: str,
                  repeat: int,
                  returncode: int) -> dict[str, Any]:
    summary = load_json(point_dir / "summary.json")
    user = summary.get("userExecution", {}) if isinstance(summary.get("userExecution"), dict) else {}
    counters = (
        summary.get("streamChunkDependencyCounters", {})
        if isinstance(summary.get("streamChunkDependencyCounters"), dict) else
        load_json(point_dir / "streamchunk_counters.json")
    )
    request_count = int(user.get("requestCount", summary.get("requestCount", 0)) or 0)
    success_count = int(user.get("successCount", 0) or 0)
    failure_count = int(user.get("failureCount", max(0, request_count - success_count)) or 0)
    success_rate = round(success_count / request_count, 6) if request_count else 0.0
    failure_rate = round(1.0 - success_rate, 6) if request_count else 1.0
    return {
        "mode": mode,
        "repeat": repeat,
        "returncode": returncode,
        "status": summary.get("status", "FAILURE" if returncode else "UNKNOWN"),
        "stable": returncode == 0 and failure_count == 0 and success_count == request_count,
        "requestCount": request_count,
        "successCount": success_count,
        "failureCount": failure_count,
        "successRate": success_rate,
        "failureRate": failure_rate,
        "p50Ms": float(user.get("p50Ms", 0.0) or 0.0),
        "p95Ms": float(user.get("p95Ms", 0.0) or 0.0),
        "p99Ms": float(user.get("p99Ms", user.get("p95Ms", 0.0)) or 0.0),
        "meanMs": float(user.get("meanMs", 0.0) or 0.0),
        "throughputRps": float(user.get("throughputRps", 0.0) or 0.0),
        "timeoutCount": int(
            summary.get("failureBreakdown", {}).get("timeoutCount", 0)
            if isinstance(summary.get("failureBreakdown"), dict) else 0),
        "dependencyEventCount": int(counters.get("eventCount", 0) or 0),
        "dependencyDecodeErrorCount": int(counters.get("decodeErrorCount", 0) or 0),
        "payloadBytesByMode": counters.get("payloadBytesByMode", {}),
        "wireBytesByMode": counters.get("wireBytesByMode", {}),
        "envelopeBytesByMode": counters.get("envelopeBytesByMode", {}),
        "overheadRatioByMode": counters.get("overheadRatioByMode", {}),
        "outputHash": output_hash(summary),
        "summaryPath": str(point_dir / "summary.json"),
        "countersPath": str(point_dir / "streamchunk_counters.json"),
    }


def write_outputs(out_dir: Path,
                  points: list[dict[str, Any]],
                  commands: list[list[str]]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    output_hashes_by_mode: dict[str, list[str]] = {}
    for point in points:
        output_hashes_by_mode.setdefault(str(point["mode"]), []).append(str(point["outputHash"]))
    payload = {
        "status": "SUCCESS" if points and all(int(p["returncode"]) == 0 for p in points) else "FAILURE",
        "pointCount": len(points),
        "modes": sorted({str(point["mode"]) for point in points}),
        "outputHashesByMode": output_hashes_by_mode,
        "matchingOutputHashes": (
            len({hash_value for values in output_hashes_by_mode.values() for hash_value in values}) == 1
            if output_hashes_by_mode else False
        ),
        "points": points,
        "commands": commands,
    }
    (out_dir / "streamchunk-mode-campaign-summary.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    with (out_dir / "streamchunk-mode-campaign-summary.csv").open(
        "w", newline="", encoding="utf-8") as output:
        fieldnames = [
            "mode", "repeat", "returncode", "status", "stable",
            "requestCount", "successCount", "failureCount", "successRate",
            "failureRate", "p50Ms", "p95Ms", "p99Ms", "meanMs",
            "throughputRps", "timeoutCount", "dependencyEventCount",
            "dependencyDecodeErrorCount", "outputHash", "summaryPath",
            "countersPath",
        ]
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        writer.writeheader()
        for point in points:
            writer.writerow({key: point.get(key, "") for key in fieldnames})


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True,
                        help="Output directory for the mode comparison campaign")
    parser.add_argument("--modes", type=parse_modes, default=["raw", "streamchunk"],
                        help="Comma-separated modes to run: raw,streamchunk")
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--requests", type=int, default=1)
    parser.add_argument("--concurrency", type=int, default=1)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("extra_harness_args", nargs=argparse.REMAINDER,
                        help="Arguments appended to each NativeTracer MiniNDN run")
    args = parser.parse_args(argv)
    if args.repeats <= 0:
        raise SystemExit("--repeats must be positive")
    if args.requests <= 0:
        raise SystemExit("--requests must be positive")
    if args.concurrency <= 0:
        raise SystemExit("--concurrency must be positive")

    out_dir = Path(args.out).expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    extras = list(args.extra_harness_args)
    if extras and extras[0] == "--":
        extras = extras[1:]
    commands: list[list[str]] = []
    points: list[dict[str, Any]] = []
    for mode in args.modes:
        for repeat in range(1, args.repeats + 1):
            point_dir = out_dir / mode / f"run-{repeat}"
            cmd = [
                sys.executable,
                str(HARNESS),
                "--out", str(point_dir),
                "--full-network",
                "--tracer-deterministic-runner",
                "--dependency-envelope-mode", mode,
                "--requests", str(args.requests),
                "--concurrency", str(args.concurrency),
            ]
            cmd.extend(extras)
            commands.append(cmd)
            if args.dry_run:
                continue
            completed = subprocess.run(cmd, cwd=str(REPO), check=False)
            points.append(point_summary(point_dir, mode, repeat, completed.returncode))

    (out_dir / "streamchunk-mode-campaign-commands.json").write_text(
        json.dumps(commands, indent=2) + "\n",
        encoding="utf-8")
    if args.dry_run:
        print(json.dumps({"status": "DRY_RUN", "commands": commands}, indent=2))
        return 0
    write_outputs(out_dir, points, commands)
    print((out_dir / "streamchunk-mode-campaign-summary.json").read_text(encoding="utf-8"))
    return 0 if points and all(int(point["returncode"]) == 0 for point in points) else 1


if __name__ == "__main__":
    raise SystemExit(main())
