#!/usr/bin/env python3
"""Isolate UAV stream, Targeted control, and concurrent behavior in MiniNDN."""

from __future__ import annotations

import argparse
import fcntl
import json
import os
from pathlib import Path
import re
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
PROVIDER_LIFECYCLE_LOG = (
    "ndn_service_framework.*=WARN:ndn_service_framework.ServiceProvider=TRACE:"
    "ndn_service_framework.examples.*=INFO:nacabe.*=WARN:ndnsvs.*=WARN:ndnsd.*=WARN"
)


def campaign_child_env(provider_lifecycle_trace: bool) -> dict[str, str] | None:
    if not provider_lifecycle_trace:
        return None
    env = os.environ.copy()
    env["NDNSF_APP_NDN_LOG"] = PROVIDER_LIFECYCLE_LOG
    return env


def classify_targeted_core(terminal_phase: str, handler_phases: set[str],
                           core_events: set[str]) -> str:
    if terminal_phase == "response":
        return "user-response"
    if "RESPONSE_PUBLISH_FAILED" in core_events:
        return "publish-failed"
    if "RESPONSE_PUBLISHED" in core_events and "handler-return" in handler_phases:
        return "response-published-no-user-response"
    if ("RESPONSE_PUBLISHED" in core_events and
            "TARGETED_REQUEST_ACCEPTED" not in core_events):
        return "pre-handler-rejected-response-published"
    if not core_events:
        return "provider-not-observed"
    if "ACK_PUBLISHED" in core_events and "TARGETED_REQUEST_ACCEPTED" not in core_events:
        return "ack-published-selection-not-completed"
    if "REQUEST_RECEIVED" in core_events and "REQUEST_DECRYPT_DONE" not in core_events:
        return "request-received-decrypt-not-completed"
    return "provider-core-incomplete"


def trace_field(line: str, key: str) -> str:
    match = re.search(r"(?:^|\s)" + re.escape(key) + r"=([^\s]+)", line)
    return match.group(1) if match else ""
LIFECYCLE_ABORT_MARKERS = (
    "terminate called without an active exception",
    "__pthread_tpp_change_priority",
)
TERMINAL_COMMAND_STAGES = frozenset({"response", "timeout", "blocked", "busy"})
CAMPAIGN_LOCK_NAME = ".campaign.lock"


def acquire_campaign_lock(out_dir: Path):
    """Hold an exclusive output-directory lock for the campaign lifetime."""
    out_dir.mkdir(parents=True, exist_ok=True)
    lock = (out_dir / CAMPAIGN_LOCK_NAME).open("a+", encoding="utf-8")
    try:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as exc:
        lock.close()
        raise RuntimeError(f"campaign output directory is already in use: {out_dir}") from exc
    lock.seek(0)
    lock.truncate()
    lock.write(f"pid={os.getpid()}\n")
    lock.flush()
    return lock


def require_fresh_output(out_dir: Path) -> None:
    """Reject accidental retries or reuse of measured campaign output."""
    existing = [path for path in out_dir.iterdir() if path.name != CAMPAIGN_LOCK_NAME]
    if existing:
        raise RuntimeError(
            f"campaign output directory is not empty; refusing to overwrite evidence: {out_dir}")


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
    log_text = (
        (run_dir / "ground-station.log").read_text(encoding="utf-8", errors="replace")
        if (run_dir / "ground-station.log").exists() else ""
    )
    drone_log_text = (
        (run_dir / "drone.log").read_text(encoding="utf-8", errors="replace")
        if (run_dir / "drone.log").exists() else ""
    )
    targeted_phase_counts: dict[str, int] = {}
    command_stages: dict[str, str] = {}
    state_convergence_stages: dict[str, str] = {}
    automation_dispatch_counts: dict[str, int] = {}
    automation_terminal_reasons: dict[str, str] = {}
    arm_terminal_ms: int | None = None
    arm_accepted = False
    takeoff_attempt_ms: int | None = None
    armed_telemetry_ms: list[int] = []
    targeted_attempts: dict[str, dict[str, Any]] = {}
    auto_arm_dispatch_ms: int | None = None
    auto_arm_wait_terminal_ms: int | None = None
    auto_arm_terminal_ms: int | None = None
    auto_arm_terminal_reason = "unknown"
    arm_command_terminal_ms: int | None = None
    arm_command_terminal_phase = "unknown"
    armed_wait_terminal_ms: int | None = None
    armed_wait_stage = "unknown"
    for line in log_text.splitlines():
        fields = base.fields_from_line(line)
        if "GS_TARGETED_PHASE" in line and "phase" in fields:
            phase = fields["phase"]
            targeted_phase_counts[phase] = targeted_phase_counts.get(phase, 0) + 1
            request_id = fields.get("request_id", "none")
            if request_id != "none":
                attempt = targeted_attempts.setdefault(request_id, {
                    "provider": fields.get("provider", "unknown"),
                    "service": fields.get("service", "unknown"),
                    "requestId": request_id,
                })
                timestamp_ms = int(fields.get("timestamp_ms", "0") or 0)
                if phase == "dispatched":
                    attempt["dispatchMs"] = timestamp_ms
                if phase in {"response", "timeout", "dispatch-rejected"}:
                    attempt["terminalPhase"] = phase
                    attempt["terminalMs"] = timestamp_ms
                    attempt["elapsedMs"] = int(fields.get("elapsed_ms", "0") or 0)
                    attempt["status"] = fields.get("status", "unknown")
        if "UAV_CONTROL_COMMAND" in line and "command" in fields and "phase" in fields:
            command_stages[fields["command"]] = fields["phase"]
            if fields["command"] == "arm" and fields["phase"] in TERMINAL_COMMAND_STAGES:
                arm_terminal_ms = int(fields.get("timestamp_ms", "0")) or arm_terminal_ms
                arm_accepted = fields.get("accepted") == "true"
                arm_command_terminal_ms = arm_terminal_ms
                arm_command_terminal_phase = fields["phase"]
            if fields["command"] == "takeoff" and fields["phase"] == "attempt":
                takeoff_attempt_ms = int(fields.get("timestamp_ms", "0")) or takeoff_attempt_ms
        if "UAV_AUTO_CONTROL_PHASE" in line and "phase" in fields:
            phase = fields["phase"]
            prerequisite = fields.get("prerequisite", "none")
            step = fields.get("step", "sequence")
            if phase in {"wait-begin", "satisfied", "expired"} and prerequisite != "none":
                state_convergence_stages[prerequisite] = phase
            if phase == "dispatch":
                automation_dispatch_counts[step] = automation_dispatch_counts.get(step, 0) + 1
                if step == "arm":
                    auto_arm_dispatch_ms = int(fields.get("timestamp_ms", "0") or 0)
            if phase == "terminal":
                automation_terminal_reasons[step] = fields.get("reason", "unknown")
                if step == "arm":
                    auto_arm_terminal_ms = int(fields.get("timestamp_ms", "0") or 0)
                    auto_arm_terminal_reason = fields.get("reason", "unknown")
            if step == "arm" and prerequisite == "telemetry-ready" and phase in {"satisfied", "expired"}:
                auto_arm_wait_terminal_ms = int(fields.get("timestamp_ms", "0") or 0)
            if step == "takeoff" and prerequisite == "armed" and phase in {"satisfied", "expired"}:
                armed_wait_terminal_ms = int(fields.get("timestamp_ms", "0") or 0)
                armed_wait_stage = phase
        if "GS_STATUS Telemetry" in line and fields.get("armed") == "true":
            timestamp = re.match(r"^(\d+(?:\.\d+)?)", line)
            if timestamp:
                armed_telemetry_ms.append(int(float(timestamp.group(1)) * 1000))
    lifecycle_abort_reason = next(
        (marker for marker in LIFECYCLE_ABORT_MARKERS if marker in log_text), "")
    drone_armed_ms: list[int] = []
    provider_telemetry_phases: dict[str, set[str]] = {}
    provider_core_events: dict[str, set[str]] = {}
    for line in drone_log_text.splitlines():
        fields = base.fields_from_line(line)
        if "DRONE_HEADLESS_STATUS" in line and fields.get("armed") == "true":
            timestamp = re.match(r"^(\d+(?:\.\d+)?)", line)
            if timestamp:
                drone_armed_ms.append(int(float(timestamp.group(1)) * 1000))
        if "UAV_TELEMETRY_PROVIDER_PHASE" in line and fields.get("request_id"):
            provider_telemetry_phases.setdefault(fields["request_id"], set()).add(
                fields.get("phase", "unknown"))
        if "[NDNSF_TRACE]" in line and trace_field(line, "role") == "provider":
            trace_request_id = trace_field(line, "requestId")
            if trace_request_id:
                provider_core_events.setdefault(trace_request_id, set()).add(
                    trace_field(line, "event") or "unknown")
    unterminated_command_attempts = {
        command: stage
        for command, stage in command_stages.items()
        if stage not in TERMINAL_COMMAND_STAGES
    }
    unterminated_automation_waits = {
        prerequisite: stage
        for prerequisite, stage in state_convergence_stages.items()
        if stage == "wait-begin"
    }
    state_convergence_complete = not unterminated_automation_waits
    automation_single_attempt = all(
        count <= 1 for count in automation_dispatch_counts.values())
    automation_sequence_complete = (
        automation_terminal_reasons.get("sequence") == "completed"
        if "sequence" in automation_terminal_reasons else None
    )
    armed_telemetry_before_takeoff = bool(
        arm_terminal_ms is not None and takeoff_attempt_ms is not None and
        any(arm_terminal_ms < observed_ms <= takeoff_attempt_ms
            for observed_ms in armed_telemetry_ms)
    )
    attempts = sorted(targeted_attempts.values(), key=lambda item: int(item.get("dispatchMs", 0)))
    telemetry_cutoff = auto_arm_dispatch_ms or auto_arm_wait_terminal_ms
    telemetry_attempts = [
        item for item in attempts
        if str(item.get("service", "")).endswith("/UAV/Telemetry/GetStatus") and
        (telemetry_cutoff is None or int(item.get("dispatchMs", 0)) <= telemetry_cutoff)
    ]
    arm_attempt = next((
        item for item in attempts
        if str(item.get("service", "")).endswith("/UAV/MAVLink/Execute") and
        auto_arm_dispatch_ms is not None and
        int(item.get("dispatchMs", 0)) >= auto_arm_dispatch_ms
    ), None)
    telemetry_deadline_overlap = bool(
        state_convergence_stages.get("telemetry-ready") == "expired" and
        auto_arm_wait_terminal_ms is not None and any(
            int(item.get("dispatchMs", 0)) <= auto_arm_wait_terminal_ms <
            int(item.get("terminalMs", 0)) for item in telemetry_attempts
        )
    )
    observer_mismatch = bool(
        arm_command_terminal_ms is not None and auto_arm_terminal_ms is not None and
        arm_command_terminal_ms < auto_arm_terminal_ms and
        auto_arm_terminal_reason == "command-state-not-terminal"
    )
    if lifecycle_abort_reason:
        earliest_boundary = "lifecycle-abort"
    elif any(item.get("terminalPhase") == "timeout" for item in telemetry_attempts) and not auto_arm_dispatch_ms:
        earliest_boundary = "telemetry-sender-timeout"
    elif not auto_arm_dispatch_ms and state_convergence_stages.get("telemetry-ready") == "expired":
        earliest_boundary = "telemetry-convergence-expired"
    elif arm_attempt and arm_attempt.get("terminalPhase") == "timeout":
        earliest_boundary = "arm-sender-timeout"
    elif arm_command_terminal_phase in {"blocked", "busy"}:
        earliest_boundary = "arm-local-block"
    elif (arm_attempt and arm_attempt.get("terminalPhase") == "response" and
          state_convergence_stages.get("armed") == "expired"):
        earliest_boundary = "armed-convergence-expired"
    elif arm_attempt and arm_attempt.get("terminalPhase") == "response":
        earliest_boundary = "arm-response"
    else:
        earliest_boundary = "unknown"
    unterminated_targeted = [
        str(item.get("requestId", "unknown"))
        for item in telemetry_attempts + ([arm_attempt] if arm_attempt else [])
        if "terminalPhase" not in item
    ]
    initial_control_attribution = {
        "earliestBoundary": earliest_boundary,
        "evidenceComplete": earliest_boundary != "unknown",
        "telemetryAttempts": telemetry_attempts,
        "telemetryDeadlineOverlap": telemetry_deadline_overlap,
        "armAttempt": arm_attempt,
        "automationArmTerminal": auto_arm_terminal_reason,
        "observerMismatch": observer_mismatch,
        "unknownReasons": (
            (["no-terminal-initial-control-evidence"] if earliest_boundary == "unknown" else []) +
            ["unterminated-targeted:" + request_id for request_id in unterminated_targeted]
        ),
    }
    first_drone_armed_ms = next((value for value in drone_armed_ms
                                 if arm_terminal_ms is not None and value >= arm_terminal_ms and
                                 (armed_wait_terminal_ms is None or value <= armed_wait_terminal_ms)), None)
    first_gs_armed_ms = next((value for value in armed_telemetry_ms
                              if arm_terminal_ms is not None and value >= arm_terminal_ms and
                              (armed_wait_terminal_ms is None or value <= armed_wait_terminal_ms)), None)
    if armed_wait_stage == "satisfied":
        visibility_class = "satisfied"
    elif armed_wait_stage == "expired" and first_gs_armed_ms is not None:
        visibility_class = "final-observation-missed"
    elif armed_wait_stage == "expired" and first_drone_armed_ms is not None:
        visibility_class = "ground-telemetry-not-visible"
    elif armed_wait_stage == "expired" and drone_log_text:
        visibility_class = "drone-not-armed"
    else:
        visibility_class = "unknown"
    armed_visibility = {
        "class": visibility_class,
        "armResponseMs": arm_terminal_ms,
        "armedWaitTerminalMs": armed_wait_terminal_ms,
        "firstDroneArmedMs": first_drone_armed_ms,
        "firstGroundStationArmedMs": first_gs_armed_ms,
        "telemetryAttemptsDuringWait": [
            item for item in attempts
            if str(item.get("service", "")).endswith("/UAV/Telemetry/GetStatus") and
            arm_terminal_ms is not None and int(item.get("dispatchMs", 0)) >= arm_terminal_ms and
            (armed_wait_terminal_ms is None or int(item.get("dispatchMs", 0)) <= armed_wait_terminal_ms)
        ],
    }
    for attempt in attempts:
        if not str(attempt.get("service", "")).endswith("/UAV/Telemetry/GetStatus"):
            continue
        phases = provider_telemetry_phases.get(str(attempt.get("requestId", "")), set())
        core_events = provider_core_events.get(str(attempt.get("requestId", "")), set())
        attempt["providerCoreEvents"] = sorted(core_events)
        attempt["coreAttribution"] = classify_targeted_core(
            str(attempt.get("terminalPhase", "")), phases, core_events)
        if attempt.get("terminalPhase") == "response":
            attempt["providerAttribution"] = "user-response"
        elif "handler-return" in phases:
            attempt["providerAttribution"] = "handler-returned-no-user-response"
        elif "handler-enter" in phases:
            attempt["providerAttribution"] = "handler-entered-no-return"
        else:
            attempt["providerAttribution"] = "handler-not-observed"
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
        "lifecycleAbort": bool(lifecycle_abort_reason),
        "lifecycleAbortReason": lifecycle_abort_reason,
        "targetedPhaseCounts": targeted_phase_counts,
        "controlCommandStages": command_stages,
        "unterminatedCommandAttempts": unterminated_command_attempts,
        "commandStagesComplete": (
            not control_required or
            (bool(command_stages) and not unterminated_command_attempts)
        ),
        "stateConvergenceStages": state_convergence_stages,
        "automationDispatchCounts": automation_dispatch_counts,
        "automationTerminalReasons": automation_terminal_reasons,
        "unterminatedAutomationWaits": unterminated_automation_waits,
        "stateConvergenceComplete": state_convergence_complete,
        "automationSingleAttempt": automation_single_attempt,
        "automationSequenceComplete": automation_sequence_complete,
        "armAccepted": arm_accepted,
        "armedTelemetryBeforeTakeoff": armed_telemetry_before_takeoff,
        "initialControlAttribution": initial_control_attribution,
        "armedVisibility": armed_visibility,
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
    result["accepted"] = (
        not result["lifecycleAbort"] and
        bool(result["commandStagesComplete"]) and
        bool(result["stateConvergenceComplete"]) and
        bool(result["automationSingleAttempt"]) and
        base.is_run_accepted(
            result,
            max_pending_chunks=48,
            max_pending_bytes=16 * 1024 * 1024,
        )
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
        command_stage_counts: dict[str, dict[str, int]] = {}
        unterminated_command_attempt_counts: dict[str, int] = {}
        convergence_stage_counts: dict[str, dict[str, int]] = {}
        unterminated_automation_wait_counts: dict[str, int] = {}
        duplicate_automation_dispatch_runs = 0
        attribution_boundary_counts: dict[str, int] = {}
        armed_visibility_counts: dict[str, int] = {}
        for row in control_rows:
            for command, stage in row.get("controlCommandStages", {}).items():
                stage_counts = command_stage_counts.setdefault(str(command), {})
                stage_name = str(stage)
                stage_counts[stage_name] = stage_counts.get(stage_name, 0) + 1
                if stage_name not in TERMINAL_COMMAND_STAGES:
                    command_name = str(command)
                    unterminated_command_attempt_counts[command_name] = (
                        unterminated_command_attempt_counts.get(command_name, 0) + 1)
            for prerequisite, stage in row.get("stateConvergenceStages", {}).items():
                stage_counts = convergence_stage_counts.setdefault(str(prerequisite), {})
                stage_name = str(stage)
                stage_counts[stage_name] = stage_counts.get(stage_name, 0) + 1
                if stage_name == "wait-begin":
                    prerequisite_name = str(prerequisite)
                    unterminated_automation_wait_counts[prerequisite_name] = (
                        unterminated_automation_wait_counts.get(prerequisite_name, 0) + 1)
            if not bool(row.get("automationSingleAttempt", True)):
                duplicate_automation_dispatch_runs += 1
            attribution = row.get("initialControlAttribution", {})
            boundary = str(attribution.get("earliestBoundary", "unknown"))
            attribution_boundary_counts[boundary] = (
                attribution_boundary_counts.get(boundary, 0) + 1)
            visibility = str(row.get("armedVisibility", {}).get("class", "unknown"))
            armed_visibility_counts[visibility] = armed_visibility_counts.get(visibility, 0) + 1
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
            "controlCommandStageCounts": command_stage_counts,
            "unterminatedCommandAttemptCounts": unterminated_command_attempt_counts,
            "stateConvergenceStageCounts": convergence_stage_counts,
            "stateConvergenceCompletedRuns": sum(
                bool(row.get("stateConvergenceComplete", True)) for row in control_rows),
            "unterminatedAutomationWaitCounts": unterminated_automation_wait_counts,
            "duplicateAutomationDispatchRuns": duplicate_automation_dispatch_runs,
            "automationSequenceRunCount": sum(
                row.get("automationSequenceComplete") is not None for row in control_rows),
            "automationSequenceCompletedRuns": sum(
                row.get("automationSequenceComplete") is True for row in control_rows),
            "armedTelemetryBeforeTakeoffRuns": sum(
                bool(row.get("armedTelemetryBeforeTakeoff", False)) for row in control_rows),
            "initialControlAttributionBoundaryCounts": attribution_boundary_counts,
            "armedVisibilityCounts": armed_visibility_counts,
            "initialControlAttributionCompleteRuns": sum(
                bool(row.get("initialControlAttribution", {}).get("evidenceComplete", False))
                for row in control_rows),
            "commandObserverMismatchRuns": sum(
                bool(row.get("initialControlAttribution", {}).get("observerMismatch", False))
                for row in control_rows),
            "telemetryDeadlineOverlapRuns": sum(
                bool(row.get("initialControlAttribution", {}).get("telemetryDeadlineOverlap", False))
                for row in control_rows),
            "unterminatedInitialTargetedAttemptRuns": sum(
                any(str(reason).startswith("unterminated-targeted:") for reason in
                    row.get("initialControlAttribution", {}).get("unknownReasons", []))
                for row in control_rows),
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
            "lifecycleAbortRuns": sum(bool(row["lifecycleAbort"]) for row in rows),
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
    parser.add_argument("--provider-lifecycle-trace", action="store_true")
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
    try:
        campaign_lock = acquire_campaign_lock(out_dir)
        if not args.dry_run and not args.reparse_existing:
            require_fresh_output(out_dir)
    except RuntimeError as exc:
        raise SystemExit(str(exc)) from exc
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
                env=campaign_child_env(args.provider_lifecycle_trace),
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
            "providerLifecycleTrace": args.provider_lifecycle_trace,
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
    campaign_lock.close()
    return 0 if summary["status"] in {"SUCCESS", "DRY_RUN"} else 1


if __name__ == "__main__":
    raise SystemExit(main())
