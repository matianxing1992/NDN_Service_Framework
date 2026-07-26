#!/usr/bin/env python3
"""Run matched UAV H264/FEC/control acceptance campaigns in MiniNDN."""

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
import time
from typing import Any, Iterable


REPO = Path(__file__).resolve().parents[1]
LAUNCHER = REPO / "Experiments/NDNSF_UAV_GUI_Minindn.py"
ADAPTIVE_MARKER = "GS_VIDEO_ADAPTIVE_STATE"
REQUIRED_ADAPTIVE_METRICS = (
    "rtt_ms",
    "pending_chunks",
    "pending_bytes",
    "fec_recovered_chunks",
    "timeouts",
    "nacks",
    "duplicates",
    "decoded_frame_gap",
)
FIELD_RE = re.compile(r"([a-z_]+)=([^\s]+)")
LOG_TIME_RE = re.compile(r"^(\d+(?:\.\d+)?)\s")
DECODED_RE = re.compile(r"GS_DECODED_FRAMES count=(\d+)")
DECODE_LAG_RE = re.compile(r"capture_to_decode_ms=(\d+)")
PARITY_RE = re.compile(r"fec_parity_shards=(\d+)")
SAMPLE_PERIOD_RE = re.compile(r"sample_period_ms=(\d+)")
SECRET_FIELDS = ("stream_key_hex=", "nonce_salt_hex=")


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


def parse_int_csv(value: str, *, minimum: int, maximum: int) -> list[int]:
    values: list[int] = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        parsed = int(item)
        if parsed < minimum or parsed > maximum:
            raise ValueError(f"value {parsed} outside [{minimum}, {maximum}]")
        if parsed not in values:
            values.append(parsed)
    if not values:
        raise ValueError("at least one value is required")
    return values


def campaign_cells(losses: Iterable[int], parity_values: Iterable[int],
                   runs: int) -> list[tuple[int, int, int]]:
    return [
        (loss, parity, repetition)
        for loss in losses
        for parity in parity_values
        for repetition in range(1, runs + 1)
    ]


def topology_text(loss_percent: int, *, delay_ms: int = 1,
                  bandwidth_mbps: int = 1000) -> str:
    return (
        "[nodes]\n"
        "memphis:\n"
        "ucla:\n\n"
        "[links]\n"
        f"memphis:ucla delay={delay_ms}ms bw={bandwidth_mbps} "
        f"loss={loss_percent}\n"
    )


def build_command(*, run_dir: Path, topology: Path, duration_seconds: int,
                  fec_parity_shards: int, include_mavlink: bool,
                  include_video: bool = True,
                  prefetch_policy: str = "mapped-pressure") -> list[str]:
    timeout_seconds = max(180, duration_seconds + 120)
    command = [
        "sudo", "-n", "-E", "timeout", f"{timeout_seconds}s", "xvfb-run", "-a",
        sys.executable, str(LAUNCHER),
        "--topology-file", str(topology),
        "--controller-node", "memphis",
        "--gs-node", "memphis",
        "--drone-node", "ucla",
        "--drone-headless",
        "--camera-mode", "file",
        "--no-virtual-camera",
        "--output-dir", str(run_dir),
        "--nfd-log-level", "WARN",
        "--no-cli",
        "--no-xhost",
    ]
    if include_video:
        command.extend([
            "--auto-video-test",
            "--auto-stop-seconds", str(duration_seconds),
            "--auto-start-delay-ms", "1000",
            "--video-bitrate-kbps", "1200",
            "--video-width", "320",
            "--video-fec-parity-shards", str(fec_parity_shards),
            "--live-stream-prefetch-policy", prefetch_policy,
        ])
    if include_mavlink:
        command.append("--auto-mavlink-test")
    return command


def _line_timestamp(line: str) -> float | None:
    match = LOG_TIME_RE.match(line)
    return float(match.group(1)) if match else None


def parse_run(run_dir: Path, returncode: int, command: list[str], *,
              loss_percent: int = 5, fec_parity_shards: int = 1,
              repetition: int = 1, duration_seconds: int = 0,
              include_mavlink: bool = False,
              elapsed_seconds: float = 0.0,
              min_decoded_frame_ratio: float = 0.5,
              include_video: bool = True,
              prefetch_policy: str = "mapped-pressure") -> dict[str, Any]:
    gs_log = run_dir / "ground-station.log"
    text = gs_log.read_text(encoding="utf-8", errors="replace") if gs_log.exists() else ""
    lines = text.splitlines()
    snapshots = [
        fields_from_line(line)
        for line in lines
        if ADAPTIVE_MARKER in line and "VideoAdaptive" in line
    ]
    core_snapshots = [
        fields_from_line(line) for line in lines
        if "GS_VIDEO_CORE_STATUS" in line
    ]
    drone_log = run_dir / "drone.log"
    drone_text = drone_log.read_text(
        encoding="utf-8", errors="replace") if drone_log.exists() else ""
    provider_snapshots = [
        fields_from_line(line) for line in drone_text.splitlines()
        if "VIDEO_LIVE_STREAM_CORE_" in line
    ]
    malformed_metrics: list[str] = []
    if include_video and not snapshots:
        malformed_metrics.append("adaptiveSnapshot:missing")
    for index, snapshot in enumerate(snapshots):
        for field in REQUIRED_ADAPTIVE_METRICS:
            value = snapshot.get(field)
            if value is None:
                malformed_metrics.append(f"snapshot-{index}:{field}:missing")
                continue
            try:
                int(value)
            except ValueError:
                malformed_metrics.append(f"snapshot-{index}:{field}:invalid")

    def integer_values(field: str) -> list[int]:
        values: list[int] = []
        for snapshot in snapshots:
            if field not in snapshot:
                continue
            try:
                values.append(int(snapshot[field]))
            except ValueError:
                continue
        return values

    rtts = [float(value) for value in integer_values("rtt_ms")]
    decoded_frames = [int(value) for value in DECODED_RE.findall(text)]
    decoded_frames.extend(integer_values("decoded_frames"))
    decode_lags = [float(value) for value in DECODE_LAG_RE.findall(text)]
    parity_values = [
        int(match.group(1))
        for line in lines if "GS_VIDEO_STREAM_READY" in line
        for match in [PARITY_RE.search(line)] if match
    ]
    sample_period_values = [
        int(match.group(1))
        for line in lines if "GS_VIDEO_STREAM_READY" in line
        for match in [SAMPLE_PERIOD_RE.search(line)]
        if match and int(match.group(1)) > 0
    ]
    start_times = [
        timestamp for line in lines
        if "GS_VIDEO_STREAM_READY" in line and "status=streaming" in line
        for timestamp in [_line_timestamp(line)] if timestamp is not None
    ]
    stop_times = [
        timestamp for line in lines
        if ("GS_VIDEO_ADAPTIVE_STATE reason=stop-ack" in line or
            "GS_VIDEO_ADAPTIVE_STATE reason=stop-requested" in line)
        for timestamp in [_line_timestamp(line)] if timestamp is not None
    ]
    stream_duration = (
        max(0.0, stop_times[-1] - start_times[0])
        if start_times and stop_times else 0.0
    )
    arm = f"MAVLink arm drone=A accepted=true" in text
    takeoff = f"MAVLink takeoff drone=A accepted=true" in text
    land = f"MAVLink land drone=A accepted=true" in text
    controls_ok = not include_mavlink or (arm and takeoff and land)
    observed_policy = core_snapshots[-1].get("policy", "") if core_snapshots else ""
    policy_accepted = observed_policy == prefetch_policy
    accepted_parity = parity_values[-1] if parity_values else -1
    duration_ok = (
        not include_video or duration_seconds <= 0 or
        stream_duration >= duration_seconds * 0.90
    )
    measured_sample_period_ms = sample_period_values[-1] if sample_period_values else 1000
    minimum_decoded_frames = (
        max(3, int(duration_seconds * 1000 / measured_sample_period_ms *
                   min_decoded_frame_ratio))
        if include_video and duration_seconds > 0 else (3 if include_video else 0)
    )
    decoded_frame_count = max(decoded_frames, default=0)
    video_completion = (
        not include_video or (
            decoded_frame_count >= minimum_decoded_frames and
            "GS_GUI_EXIT rc=0" in text and
            accepted_parity == fec_parity_shards and
            duration_ok
        )
    )
    persisted_logs = ""
    for path in sorted(run_dir.glob("*.log")):
        persisted_logs += path.read_text(encoding="utf-8", errors="replace")
    secret_leak_count = sum(persisted_logs.count(field) for field in SECRET_FIELDS)
    secret_scan_accepted = secret_leak_count == 0
    completion = (returncode == 0 and video_completion and controls_ok and
                  secret_scan_accepted and policy_accepted)

    def last_core_int(field: str) -> int:
        for snapshot in reversed(core_snapshots):
            value = snapshot.get(field, "")
            if value.isdigit():
                return int(value)
        return 0

    def last_provider_int(field: str) -> int:
        for snapshot in reversed(provider_snapshots):
            value = snapshot.get(field, "")
            if value.isdigit():
                return int(value)
        return 0

    provider_future_interests = last_provider_int("provider_future_interests")
    provider_future_hits = last_provider_int("provider_future_hits")
    provider_pending_at_stop = last_provider_int("pending_interests")
    provider_future_eligible = max(
        0, provider_future_interests - provider_pending_at_stop)
    core_in_flight = [
        int(snapshot["in_flight"]) for snapshot in core_snapshots
        if snapshot.get("in_flight", "").isdigit()
    ]
    mapping_interest_count = last_core_int("mapping_interests")
    payload_interest_count = last_core_int("payload_interests")
    pit_samples: list[int] = []
    pit_path = run_dir / "nfd-pit-samples.csv"
    if pit_path.exists():
        with pit_path.open(encoding="utf-8", errors="replace") as stream:
            reader = csv.DictReader(stream)
            for row in reader:
                value = row.get("pit_entries", "")
                if value.isdigit():
                    pit_samples.append(int(value))

    pending_chunks = integer_values("pending_chunks")
    pending_bytes = integer_values("pending_bytes")
    decoded_gaps = integer_values("decoded_frame_gap")
    timeouts = integer_values("timeouts")
    nacks = integer_values("nacks")
    duplicates = integer_values("duplicates")
    fec_recovered = integer_values("fec_recovered_chunks")
    return {
        "runId": f"loss-{loss_percent:02d}-fec-{fec_parity_shards}-run-{repetition:02d}",
        "runDirectory": str(run_dir),
        "returncode": returncode,
        "command": command,
        "lossPercent": loss_percent,
        "fecParityShards": fec_parity_shards,
        "prefetchPolicy": prefetch_policy,
        "observedPrefetchPolicy": observed_policy,
        "policyAccepted": policy_accepted,
        "acceptedFecParityShards": accepted_parity,
        "repetition": repetition,
        "durationSeconds": duration_seconds,
        "streamDurationSeconds": stream_duration,
        "elapsedSeconds": elapsed_seconds,
        "completion": completion,
        "processCompletion": returncode == 0,
        "videoCompletion": video_completion,
        "controlCompletion": controls_ok,
        "secretLeakCount": secret_leak_count,
        "secretScanAccepted": secret_scan_accepted,
        "durationAccepted": duration_ok,
        "adaptiveSnapshotCount": len(snapshots),
        "metricsValid": not malformed_metrics,
        "malformedMetrics": ";".join(malformed_metrics),
        "decodedFrames": decoded_frame_count,
        "minimumDecodedFrames": minimum_decoded_frames,
        "measuredSamplePeriodMs": measured_sample_period_ms,
        "decodedFrameRate": (
            decoded_frame_count / stream_duration if stream_duration > 0 else 0.0
        ),
        "captureToDecodeP50Ms": percentile(decode_lags, 0.50),
        "captureToDecodeP95Ms": percentile(decode_lags, 0.95),
        "mappingInterests": mapping_interest_count,
        "payloadInterests": payload_interest_count,
        "futurePayloadInterests": last_core_int("future_payload_interests"),
        "mappingBytes": last_core_int("mapping_bytes"),
        "mappingInterestShare": (
            mapping_interest_count / (mapping_interest_count + payload_interest_count)
            if mapping_interest_count + payload_interest_count else 0.0
        ),
        "maxCoreInFlight": max(core_in_flight, default=0),
        "maxNfdPitEntries": max(pit_samples, default=0),
        "providerFutureInterests": provider_future_interests,
        "providerFutureEligible": provider_future_eligible,
        "providerFutureHits": provider_future_hits,
        "providerFutureHitRatio": (
            provider_future_hits / provider_future_eligible
            if provider_future_eligible else 0.0
        ),
        "mavlinkArm": arm,
        "mavlinkTakeoff": takeoff,
        "mavlinkLand": land,
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


RUN_FIELDS = [
    "runId", "runDirectory", "returncode", "lossPercent", "fecParityShards",
    "prefetchPolicy",
    "observedPrefetchPolicy", "policyAccepted",
    "acceptedFecParityShards", "repetition", "durationSeconds",
    "streamDurationSeconds", "elapsedSeconds", "completion",
    "processCompletion", "videoCompletion", "controlCompletion",
    "secretLeakCount", "secretScanAccepted",
    "durationAccepted", "adaptiveSnapshotCount", "decodedFrames",
    "metricsValid", "malformedMetrics",
    "minimumDecodedFrames", "measuredSamplePeriodMs", "decodedFrameRate",
    "captureToDecodeP50Ms", "captureToDecodeP95Ms", "mappingInterests",
    "payloadInterests", "futurePayloadInterests", "providerFutureInterests",
    "providerFutureEligible",
    "providerFutureHits", "providerFutureHitRatio", "mappingBytes",
    "mappingInterestShare", "maxCoreInFlight", "maxNfdPitEntries",
    "mavlinkArm", "mavlinkTakeoff", "mavlinkLand",
    "staleSessionRejectCount", "staleStreamRejectCount",
    "fecRecoveryLogCount", "fecRecoveredChunks", "maxPendingChunks",
    "maxPendingBytes", "maxDecodedFrameGap", "maxTimeouts", "maxNacks",
    "maxDuplicates", "rttP50Ms", "rttP95Ms",
]


def write_csv(path: Path, rows: list[dict[str, Any]], fields: list[str]) -> None:
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def is_run_accepted(run: dict[str, Any], *, max_pending_chunks: int,
                    max_pending_bytes: int) -> bool:
    return (
        bool(run["completion"]) and
        bool(run["metricsValid"]) and
        int(run["maxPendingChunks"]) <= max_pending_chunks and
        int(run["maxPendingBytes"]) <= max_pending_bytes and
        int(run["staleSessionRejectCount"]) == 0 and
        int(run["staleStreamRejectCount"]) == 0 and
        bool(run["secretScanAccepted"])
        and bool(run["policyAccepted"])
    )


def aggregate_treatments(runs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, int, int], list[dict[str, Any]]] = {}
    for run in runs:
        grouped.setdefault(
            (str(run.get("prefetchPolicy", "mapped-pressure")),
             int(run["lossPercent"]), int(run["fecParityShards"])), []
        ).append(run)
    aggregates: list[dict[str, Any]] = []
    for (policy, loss, parity), rows in sorted(grouped.items()):
        aggregates.append({
            "prefetchPolicy": policy,
            "lossPercent": loss,
            "fecParityShards": parity,
            "runCount": len(rows),
            "completedRuns": sum(bool(row["completion"]) for row in rows),
            "completionRate": sum(bool(row["completion"]) for row in rows) / len(rows),
            "videoCompletedRuns": sum(bool(row["videoCompletion"]) for row in rows),
            "videoCompletionRate": sum(bool(row["videoCompletion"]) for row in rows) / len(rows),
            "controlCompletedRuns": sum(bool(row["controlCompletion"]) for row in rows),
            "controlCompletionRate": sum(bool(row["controlCompletion"]) for row in rows) / len(rows),
            "meanDecodedFrames": statistics.fmean(row["decodedFrames"] for row in rows),
            "meanFecRecoveredChunks": statistics.fmean(
                row["fecRecoveredChunks"] for row in rows),
            "meanTimeouts": statistics.fmean(row["maxTimeouts"] for row in rows),
            "maxDecodedFrameGap": max(row["maxDecodedFrameGap"] for row in rows),
            "maxPendingChunks": max(row["maxPendingChunks"] for row in rows),
            "maxPendingBytes": max(row["maxPendingBytes"] for row in rows),
            "meanRttP50Ms": statistics.fmean(row["rttP50Ms"] for row in rows),
            "meanRttP95Ms": statistics.fmean(row["rttP95Ms"] for row in rows),
            "meanCaptureToDecodeP50Ms": statistics.fmean(
                row.get("captureToDecodeP50Ms", 0.0) for row in rows),
            "meanCaptureToDecodeP95Ms": statistics.fmean(
                row.get("captureToDecodeP95Ms", 0.0) for row in rows),
            "meanMappingInterests": statistics.fmean(
                row.get("mappingInterests", 0) for row in rows),
            "meanPayloadInterests": statistics.fmean(
                row.get("payloadInterests", 0) for row in rows),
            "meanFuturePayloadInterests": statistics.fmean(
                row.get("futurePayloadInterests", 0) for row in rows),
            "meanMappingBytes": statistics.fmean(
                row.get("mappingBytes", 0) for row in rows),
            "meanMappingInterestShare": statistics.fmean(
                row.get("mappingInterestShare", 0.0) for row in rows),
            "maxCoreInFlight": max(row.get("maxCoreInFlight", 0) for row in rows),
            "maxNfdPitEntries": max(row.get("maxNfdPitEntries", 0) for row in rows),
            "meanProviderFutureHitRatio": statistics.fmean(
                row.get("providerFutureHitRatio", 0.0) for row in rows),
            "controlsPassed": sum(
                row["mavlinkArm"] and row["mavlinkTakeoff"] and row["mavlinkLand"]
                for row in rows),
            "secretLeakCount": sum(int(row.get("secretLeakCount", 0)) for row in rows),
        })
    return aggregates


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--loss-percentages", default="0,5")
    parser.add_argument("--fec-parity-shards", default="0,1")
    parser.add_argument("--auto-stop-seconds", type=int, default=60)
    parser.add_argument("--link-delay-ms", type=int, default=1)
    parser.add_argument("--bandwidth-mbps", type=int, default=1000)
    parser.add_argument("--min-decoded-frame-ratio", type=float, default=0.5)
    parser.add_argument("--max-pending-chunks", type=int, default=48)
    parser.add_argument("--max-pending-bytes", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--no-mavlink", action="store_true")
    parser.add_argument(
        "--live-stream-prefetch-policy",
        choices=("mapped-pressure", "mapped-live-v1-future-on",
                 "mapped-live-v1-future-off"),
        default="mapped-pressure")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--reparse-existing", action="store_true",
                        help="Rebuild summaries from existing immutable run logs without executing MiniNDN.")
    args = parser.parse_args()
    if args.runs < 1:
        raise SystemExit("--runs must be positive")
    if args.auto_stop_seconds < 1:
        raise SystemExit("--auto-stop-seconds must be positive")
    if not 0.0 < args.min_decoded_frame_ratio <= 1.0:
        raise SystemExit("--min-decoded-frame-ratio must be in (0, 1]")

    try:
        losses = parse_int_csv(args.loss_percentages, minimum=0, maximum=100)
        parity_values = parse_int_csv(args.fec_parity_shards, minimum=0, maximum=1)
    except (ValueError, TypeError) as exc:
        raise SystemExit(str(exc)) from exc

    out_dir = Path(args.out).expanduser().resolve()
    prior_runs: dict[str, dict[str, Any]] = {}
    if args.reparse_existing:
        summary_path = out_dir / "campaign-summary.json"
        if not summary_path.exists():
            raise SystemExit("--reparse-existing requires campaign-summary.json")
        prior = json.loads(summary_path.read_text(encoding="utf-8"))
        prior_runs = {str(run.get("runId", "")): run for run in prior.get("runs", [])}
    topology_dir = out_dir / "topologies"
    topology_dir.mkdir(parents=True, exist_ok=True)
    topologies: dict[int, Path] = {}
    for loss in losses:
        topology = topology_dir / f"uav-loss-{loss:02d}.conf"
        topology.write_text(topology_text(
            loss, delay_ms=args.link_delay_ms,
            bandwidth_mbps=args.bandwidth_mbps), encoding="utf-8")
        topologies[loss] = topology

    runs: list[dict[str, Any]] = []
    include_mavlink = not args.no_mavlink
    for loss, parity, repetition in campaign_cells(losses, parity_values, args.runs):
        run_id = f"loss-{loss:02d}-fec-{parity}-run-{repetition:02d}"
        run_dir = out_dir / run_id
        command = build_command(
            run_dir=run_dir,
            topology=topologies[loss],
            duration_seconds=args.auto_stop_seconds,
            fec_parity_shards=parity,
            include_mavlink=include_mavlink,
            prefetch_policy=args.live_stream_prefetch_policy,
        )
        if args.reparse_existing:
            previous = prior_runs.get(run_id)
            if previous is None:
                raise SystemExit(f"missing prior run record: {run_id}")
            runs.append(parse_run(
                run_dir,
                int(previous.get("returncode", 1)),
                list(previous.get("command", command)),
                loss_percent=loss,
                fec_parity_shards=parity,
                repetition=repetition,
                duration_seconds=args.auto_stop_seconds,
                include_mavlink=include_mavlink,
                elapsed_seconds=float(previous.get("elapsedSeconds", 0.0)),
                min_decoded_frame_ratio=args.min_decoded_frame_ratio,
                prefetch_policy=args.live_stream_prefetch_policy,
            ))
            continue
        if args.dry_run:
            runs.append({
                "runId": run_id,
                "runDirectory": str(run_dir),
                "lossPercent": loss,
                "fecParityShards": parity,
                "repetition": repetition,
                "durationSeconds": args.auto_stop_seconds,
                "command": command,
            })
            continue
        run_dir.mkdir(parents=True, exist_ok=True)
        started = time.monotonic()
        with (run_dir / "campaign-launcher.log").open("w", encoding="utf-8") as output:
            completed = subprocess.run(
                command,
                cwd=str(REPO),
                env=dict(os.environ),
                stdout=output,
                stderr=subprocess.STDOUT,
                check=False,
            )
        runs.append(parse_run(
            run_dir, completed.returncode, command,
            loss_percent=loss,
            fec_parity_shards=parity,
            repetition=repetition,
            duration_seconds=args.auto_stop_seconds,
            include_mavlink=include_mavlink,
            elapsed_seconds=time.monotonic() - started,
            min_decoded_frame_ratio=args.min_decoded_frame_ratio,
            prefetch_policy=args.live_stream_prefetch_policy,
        ))

    if args.dry_run:
        summary: dict[str, Any] = {
            "status": "DRY_RUN",
            "constants": {
                "lossPercentages": losses,
                "fecParityShards": parity_values,
                "runsPerTreatment": args.runs,
                "durationSeconds": args.auto_stop_seconds,
                "includeMavlink": include_mavlink,
                "linkDelayMs": args.link_delay_ms,
                "bandwidthMbps": args.bandwidth_mbps,
                "videoBitrateKbps": 1200,
                "videoWidth": 320,
                "minDecodedFrameRatio": args.min_decoded_frame_ratio,
            },
            "runs": runs,
        }
    else:
        for run in runs:
            run["bufferingAccepted"] = (
                int(run["maxPendingChunks"]) <= args.max_pending_chunks and
                int(run["maxPendingBytes"]) <= args.max_pending_bytes
            )
            run["staleAccepted"] = (
                int(run["staleSessionRejectCount"]) > 0 or
                int(run["staleStreamRejectCount"]) > 0
            )
            run["accepted"] = is_run_accepted(
                run,
                max_pending_chunks=args.max_pending_chunks,
                max_pending_bytes=args.max_pending_bytes,
            )
        treatments = aggregate_treatments(runs)
        all_accepted = all(bool(run["accepted"]) for run in runs)
        summary = {
            "status": "SUCCESS" if all_accepted else "FAILURE",
            "constants": {
                "lossPercentages": losses,
                "fecParityShards": parity_values,
                "runsPerTreatment": args.runs,
                "durationSeconds": args.auto_stop_seconds,
                "includeMavlink": include_mavlink,
                "linkDelayMs": args.link_delay_ms,
                "bandwidthMbps": args.bandwidth_mbps,
                "videoBitrateKbps": 1200,
                "videoWidth": 320,
                "minDecodedFrameRatio": args.min_decoded_frame_ratio,
                "maxPendingChunks": args.max_pending_chunks,
                "maxPendingBytes": args.max_pending_bytes,
                "automaticRetry": False,
                "prefetchPolicy": args.live_stream_prefetch_policy,
            },
            "runCount": len(runs),
            "acceptedRuns": sum(bool(run["accepted"]) for run in runs),
            "treatments": treatments,
            "runs": runs,
        }
        write_csv(out_dir / "campaign-runs.csv", runs,
                  RUN_FIELDS + ["bufferingAccepted", "staleAccepted", "accepted"])
        write_csv(out_dir / "campaign-treatments.csv", treatments,
                  list(treatments[0]) if treatments else [])

    (out_dir / "campaign-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary["status"] in {"SUCCESS", "DRY_RUN"} else 1


if __name__ == "__main__":
    raise SystemExit(main())
