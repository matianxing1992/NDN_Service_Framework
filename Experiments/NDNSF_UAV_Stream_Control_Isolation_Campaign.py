#!/usr/bin/env python3
"""Isolate UAV stream, Targeted control, and concurrent behavior in MiniNDN."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys
import time
from typing import Any, Iterable


sys.path.insert(0, str(Path(__file__).resolve().parent))
import NDNSF_UAV_Stream_Parity_Campaign as base  # noqa: E402


WORKLOADS = {
    "control-only": {"video": False, "control": True, "parity": -1},
    "video-only-fec0": {"video": True, "control": False, "parity": 0},
    "video-only-fec1": {"video": True, "control": False, "parity": 1},
    "combined-fec0": {"video": True, "control": True, "parity": 0},
    "combined-fec1": {"video": True, "control": True, "parity": 1},
}
DEFAULT_WORKLOADS = ",".join(WORKLOADS)


def parse_workload_modes(value: str) -> list[str]:
    modes: list[str] = []
    for item in value.split(","):
        mode = item.strip()
        if not mode:
            continue
        if mode not in WORKLOADS:
            raise ValueError(f"unknown workload mode: {mode}")
        if mode not in modes:
            modes.append(mode)
    if not modes:
        raise ValueError("at least one workload mode is required")
    return modes


def campaign_cells(modes: Iterable[str], runs: int) -> list[tuple[str, int]]:
    return [
        (mode, repetition)
        for mode in modes
        for repetition in range(1, runs + 1)
    ]


def build_mode_command(*, mode: str, run_dir: Path, topology: Path,
                       duration_seconds: int) -> list[str]:
    workload = WORKLOADS[mode]
    return base.build_command(
        run_dir=run_dir,
        topology=topology,
        duration_seconds=duration_seconds,
        fec_parity_shards=max(0, int(workload["parity"])),
        include_mavlink=bool(workload["control"]),
        include_video=bool(workload["video"]),
    )


def parse_mode_run(run_dir: Path, returncode: int, command: list[str], *,
                   mode: str, repetition: int, loss_percent: int,
                   duration_seconds: int, elapsed_seconds: float) -> dict[str, Any]:
    workload = WORKLOADS[mode]
    video_required = bool(workload["video"])
    control_required = bool(workload["control"])
    result = base.parse_run(
        run_dir,
        returncode,
        command,
        loss_percent=loss_percent,
        fec_parity_shards=max(0, int(workload["parity"])),
        repetition=repetition,
        duration_seconds=duration_seconds if video_required else 0,
        include_mavlink=control_required,
        include_video=video_required,
        elapsed_seconds=elapsed_seconds,
    )
    video_completion = bool(result["videoCompletion"]) if video_required else None
    control_completion = bool(result["controlCompletion"]) if control_required else None
    result.update({
        "runId": f"{mode}-run-{repetition:02d}",
        "workloadMode": mode,
        "videoRequired": video_required,
        "controlRequired": control_required,
        "fecParityShards": int(workload["parity"]),
        "videoCompletion": video_completion,
        "controlCompletion": control_completion,
    })
    result["completion"] = (
        bool(result["processCompletion"]) and
        (not video_required or video_completion is True) and
        (not control_required or control_completion is True)
    )
    result["bufferingAccepted"] = (
        int(result["maxPendingChunks"]) <= 48 and
        int(result["maxPendingBytes"]) <= 16 * 1024 * 1024
    )
    result["staleAccepted"] = (
        int(result["staleSessionRejectCount"]) > 0 or
        int(result["staleStreamRejectCount"]) > 0
    )
    result["accepted"] = base.is_run_accepted(
        result,
        max_pending_chunks=48,
        max_pending_bytes=16 * 1024 * 1024,
    )
    return result


def aggregate_cells(runs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[str, list[dict[str, Any]]] = {}
    for run in runs:
        grouped.setdefault(str(run["workloadMode"]), []).append(run)
    aggregates: list[dict[str, Any]] = []
    for mode in WORKLOADS:
        rows = grouped.get(mode, [])
        if not rows:
            continue
        video_rows = [row for row in rows if row["videoRequired"]]
        control_rows = [row for row in rows if row["controlRequired"]]
        aggregates.append({
            "workloadMode": mode,
            "fecParityShards": WORKLOADS[mode]["parity"],
            "runCount": len(rows),
            "acceptedRuns": sum(bool(row["accepted"]) for row in rows),
            "completionRate": sum(bool(row["accepted"]) for row in rows) / len(rows),
            "videoRunCount": len(video_rows),
            "videoCompletedRuns": sum(row["videoCompletion"] is True for row in video_rows),
            "controlRunCount": len(control_rows),
            "controlCompletedRuns": sum(
                row["controlCompletion"] is True for row in control_rows),
            "meanDecodedFrames": (
                statistics.fmean(row["decodedFrames"] for row in video_rows)
                if video_rows else None
            ),
            "meanFecRecoveredChunks": (
                statistics.fmean(row["fecRecoveredChunks"] for row in video_rows)
                if video_rows else None
            ),
            "meanRttP95Ms": (
                statistics.fmean(row["rttP95Ms"] for row in video_rows)
                if video_rows else None
            ),
            "meanTimeouts": (
                statistics.fmean(row["maxTimeouts"] for row in video_rows)
                if video_rows else None
            ),
            "meanElapsedSeconds": statistics.fmean(row["elapsedSeconds"] for row in rows),
        })
    return aggregates


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True)
    parser.add_argument("--workload-modes", default=DEFAULT_WORKLOADS)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--loss-percent", type=int, default=5)
    parser.add_argument("--auto-stop-seconds", type=int, default=60)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--reparse-existing", action="store_true")
    args = parser.parse_args()
    if args.runs < 1:
        raise SystemExit("--runs must be positive")
    if not 0 <= args.loss_percent <= 100:
        raise SystemExit("--loss-percent must be in [0, 100]")
    if args.auto_stop_seconds < 1:
        raise SystemExit("--auto-stop-seconds must be positive")
    try:
        modes = parse_workload_modes(args.workload_modes)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc

    out_dir = Path(args.out).expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    topology = out_dir / f"uav-loss-{args.loss_percent:02d}.conf"
    topology.write_text(base.topology_text(args.loss_percent), encoding="utf-8")

    prior_runs: dict[str, dict[str, Any]] = {}
    if args.reparse_existing:
        summary_path = out_dir / "campaign-summary.json"
        if not summary_path.exists():
            raise SystemExit("--reparse-existing requires campaign-summary.json")
        prior = json.loads(summary_path.read_text(encoding="utf-8"))
        prior_runs = {str(run["runId"]): run for run in prior.get("runs", [])}

    runs: list[dict[str, Any]] = []
    for mode, repetition in campaign_cells(modes, args.runs):
        run_id = f"{mode}-run-{repetition:02d}"
        run_dir = out_dir / run_id
        command = build_mode_command(
            mode=mode,
            run_dir=run_dir,
            topology=topology,
            duration_seconds=args.auto_stop_seconds,
        )
        if args.dry_run:
            workload = WORKLOADS[mode]
            runs.append({
                "runId": run_id,
                "runDirectory": str(run_dir),
                "workloadMode": mode,
                "videoRequired": workload["video"],
                "controlRequired": workload["control"],
                "fecParityShards": workload["parity"],
                "repetition": repetition,
                "command": command,
            })
            continue
        if args.reparse_existing:
            previous = prior_runs.get(run_id)
            if previous is None:
                raise SystemExit(f"missing prior run record: {run_id}")
            runs.append(parse_mode_run(
                run_dir,
                int(previous.get("returncode", 1)),
                list(previous.get("command", command)),
                mode=mode,
                repetition=repetition,
                loss_percent=args.loss_percent,
                duration_seconds=args.auto_stop_seconds,
                elapsed_seconds=float(previous.get("elapsedSeconds", 0.0)),
            ))
            continue
        run_dir.mkdir(parents=True, exist_ok=True)
        started = time.monotonic()
        with (run_dir / "campaign-launcher.log").open("w", encoding="utf-8") as output:
            completed = subprocess.run(
                command,
                cwd=str(base.REPO),
                check=False,
                stdout=output,
                stderr=subprocess.STDOUT,
            )
        runs.append(parse_mode_run(
            run_dir,
            completed.returncode,
            command,
            mode=mode,
            repetition=repetition,
            loss_percent=args.loss_percent,
            duration_seconds=args.auto_stop_seconds,
            elapsed_seconds=time.monotonic() - started,
        ))

    summary: dict[str, Any] = {
        "status": "DRY_RUN" if args.dry_run else (
            "SUCCESS" if all(bool(run["accepted"]) for run in runs) else "FAILURE"),
        "constants": {
            "workloadModes": modes,
            "runsPerCell": args.runs,
            "lossPercent": args.loss_percent,
            "videoDurationSeconds": args.auto_stop_seconds,
            "linkDelayMs": 1,
            "bandwidthMbps": 1000,
            "automaticRetry": False,
        },
        "runCount": len(runs),
        "runs": runs,
    }
    if not args.dry_run:
        summary["acceptedRuns"] = sum(bool(run["accepted"]) for run in runs)
        summary["cells"] = aggregate_cells(runs)
        base.write_csv(out_dir / "campaign-runs.csv", runs, list(runs[0]))
        base.write_csv(out_dir / "campaign-cells.csv", summary["cells"],
                       list(summary["cells"][0]))
    (out_dir / "campaign-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary["status"] in {"SUCCESS", "DRY_RUN"} else 1


if __name__ == "__main__":
    raise SystemExit(main())
