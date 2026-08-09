#!/usr/bin/env python3

import argparse
from collections import Counter
import csv
import datetime
import fcntl
import hashlib
import importlib.metadata
import inspect
import json
import math
import os
import platform
import random
import re
import shlex
import signal
import subprocess
import sys
import threading
import time
import uuid
from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace

from mininet.log import info, setLogLevel
from mn_wifi.link import wmediumd
from mn_wifi.wmediumdConnector import interference
from minindn.apps.app_manager import AppManager
from minindn.apps.nfd import Nfd
from minindn.helpers.nfdc import Nfdc
from minindn.minindn import Minindn
from minindn.wifi.minindnwifi import MinindnWifi
from minindn.util import getPopen

import NDNSF_NewAPI_Minindn_Perf as perf


REPO_ROOT = Path(__file__).resolve().parents[1]
MOBILITY_BUILD_DIR = Path(
    os.environ.get("NDNSF_MOBILITY_BUILD_DIR", str(REPO_ROOT / "build"))
).expanduser().resolve()
APP_CONTROLLER = MOBILITY_BUILD_DIR / "examples/App_ServiceController"
APP_PROVIDER = MOBILITY_BUILD_DIR / "examples/App_WifiMobilityProvider"
APP_USER = MOBILITY_BUILD_DIR / "examples/App_WifiMobilityUser"
GRPC_DIR = REPO_ROOT / "Experiments/gRPC"
GRPC_FAILOVER_CLIENT = GRPC_DIR / "greeter_failover_client.py"
NSC_DIR = REPO_ROOT / "Experiments/NDN_NSC"
FORMAL_SYSTEMS = ("grpc", "nsc")
FORMAL_RANGES = (100.0, 150.0, 200.0)
SINGLE_PROVIDER_SYSTEMS = ("grpc-single", "nsc-single")
SUPPORTED_SYSTEMS = ("ndnsf", "grpc", "nsc") + SINGLE_PROVIDER_SYSTEMS
DEFAULT_PROVIDER_NAMES = ("ucla", "wustl", "uiuc")
FOUR_PROVIDER_NAMES = DEFAULT_PROVIDER_NAMES + ("arizona",)
PROVIDER_ENDPOINTS = {
    "ucla": "10.0.0.2:50051",
    "wustl": "10.0.0.3:50051",
    "uiuc": "10.0.0.4:50051",
    "arizona": "10.0.0.5:50051",
}
PROVIDER_PREFIXES = {
    "ucla": "/muas/ucla",
    "wustl": "/muas/wustl",
    "uiuc": "/muas/uiuc",
    "arizona": "/muas/arizona",
}
ACTIVE_PROVIDER_NAMES = list(DEFAULT_PROVIDER_NAMES)
ACTIVE_PROFILE = "three-provider"
ACTIVE_AP_LAYOUT = "single"
ACTIVE_SPEED_MPS = (1.0, 2.0)
AP_LAYOUTS = {
    "single": ((200.0, 200.0),),
    # This is a deterministic coverage model.  The MiniNDN topology keeps a
    # single backhaul AP for stable process connectivity; availability is
    # derived from the nearest AP in this layout and recorded in the trace.
    "multi-ap": ((130.0, 200.0), (200.0, 200.0), (270.0, 200.0)),
}
TRACE_INTERVAL_S = 1.0
HANDOFF_PERIOD_S = 1.0
TRAFFIC_START_DELAY_S = 2.0
TRAFFIC_PHASE_TOLERANCE_S = 0.05
SMOKE_OUTAGE_START_S = 2.4
SMOKE_OUTAGE_END_S = 4.4
CAMPAIGN_SCHEMA = "ndnsf-mobility-baseline-campaign-v1"
CAMPAIGN_LOCK = REPO_ROOT / "results" / ".wifi-router-mobility-campaign.lock"


def log(message):
    info(f"{message}\n")


def parse_csv_floats(value):
    return [float(item) for item in value.split(",") if item.strip()]


def parse_csv_strings(value):
    return [item.strip().lower() for item in value.split(",") if item.strip()]


def configure_profile(profile, ap_layout=None, speed_mps=None):
    """Select a reproducible provider/AP/speed profile for this process."""
    global ACTIVE_PROFILE, ACTIVE_AP_LAYOUT, ACTIVE_SPEED_MPS
    if profile not in {"three-provider", "four-provider-single-ap",
                       "four-provider-multi-ap"}:
        raise ValueError(f"unsupported mobility profile: {profile}")
    ACTIVE_PROFILE = profile
    ACTIVE_PROVIDER_NAMES[:] = (
        list(FOUR_PROVIDER_NAMES) if profile in {
            "four-provider-single-ap", "four-provider-multi-ap"}
        else list(DEFAULT_PROVIDER_NAMES))
    ACTIVE_AP_LAYOUT = ap_layout or (
        "multi-ap" if profile == "four-provider-multi-ap" else "single")
    if ACTIVE_AP_LAYOUT not in AP_LAYOUTS:
        raise ValueError(f"unsupported AP layout: {ACTIVE_AP_LAYOUT}")
    if speed_mps is None:
        ACTIVE_SPEED_MPS = (8.0, 8.0) if profile in {
            "four-provider-single-ap", "four-provider-multi-ap"} else (1.0, 2.0)
    else:
        value = float(speed_mps)
        if not math.isfinite(value) or value <= 0:
            raise ValueError("speed_mps must be a finite positive value")
        ACTIVE_SPEED_MPS = (value, value)


def active_ap_positions():
    return AP_LAYOUTS[ACTIVE_AP_LAYOUT]


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")


def range_label(ap_range):
    value = float(ap_range)
    return str(int(value)) if value.is_integer() else str(value).replace(".", "p")


def sha256_file(path):
    path = Path(path)
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_text(value):
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def write_json(path, payload):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.{uuid.uuid4().hex}.tmp")
    try:
        temporary.write_text(json.dumps(
            payload, indent=2, sort_keys=True, allow_nan=False) + "\n")
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


@contextmanager
def campaign_lock(lock_path, output_dir, campaign_id):
    """Hold one OS-enforced parent-driver lock for the complete campaign."""
    lock_path = Path(lock_path)
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    lock_file = lock_path.open("a+", encoding="utf-8")
    locked = False
    try:
        try:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            lock_file.seek(0)
            owner = lock_file.read().strip() or "unknown owner"
            raise RuntimeError(
                f"another mobility campaign owns {lock_path}: {owner}") from error
        locked = True
        metadata = {
            "active": True,
            "pid": os.getpid(),
            "command": shlex.join(sys.argv),
            "campaign_id": campaign_id,
            "output_dir": str(Path(output_dir).resolve()),
            "acquired_at": utc_now(),
        }
        lock_file.seek(0)
        lock_file.truncate()
        json.dump(metadata, lock_file, indent=2, sort_keys=True)
        lock_file.write("\n")
        lock_file.flush()
        os.fsync(lock_file.fileno())
        yield metadata
    finally:
        if locked:
            try:
                if lock_file.seekable():
                    lock_file.seek(0)
                    lock_file.truncate()
                    json.dump({
                        "active": False,
                        "pid": os.getpid(),
                        "campaign_id": campaign_id,
                        "output_dir": str(Path(output_dir).resolve()),
                        "released_at": utc_now(),
                    }, lock_file, indent=2, sort_keys=True)
                    lock_file.write("\n")
                    lock_file.flush()
                    os.fsync(lock_file.fileno())
            finally:
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
            lock_file.close()
        else:
            lock_file.close()


def parse_json_marker(text, marker):
    lines = [line for line in text.splitlines() if line.startswith(marker)]
    if len(lines) != 1:
        raise RuntimeError(
            f"expected exactly one {marker.strip()} marker, found {len(lines)}")
    try:
        return json.loads(lines[0][len(marker):])
    except json.JSONDecodeError as error:
        raise RuntimeError(f"invalid {marker.strip()} JSON: {error}") from error


def required_number(values, key, *, integer=False):
    if key not in values:
        raise RuntimeError(f"required summary field missing: {key}")
    try:
        value = float(values[key])
    except (TypeError, ValueError) as error:
        raise RuntimeError(f"summary field {key} is not numeric: {values[key]!r}") from error
    if not math.isfinite(value) or value < 0:
        raise RuntimeError(f"summary field {key} must be finite and nonnegative")
    if integer and not value.is_integer():
        raise RuntimeError(f"summary field {key} must be an integer")
    return int(value) if integer else value


def required_finite_number(values, key):
    """Parse a finite numeric field whose signed value is meaningful.

    Measurement-start lateness is an error relative to the requested barrier,
    so a small negative value means the child started early and must not be
    rejected as if it were a counter.
    """
    if key not in values:
        raise RuntimeError(f"required summary field missing: {key}")
    try:
        value = float(values[key])
    except (TypeError, ValueError) as error:
        raise RuntimeError(f"summary field {key} is not numeric: {values[key]!r}") from error
    if not math.isfinite(value):
        raise RuntimeError(f"summary field {key} must be finite")
    return value


def validate_latency_order(p50_ms, p95_ms, p99_ms):
    if not (0.0 <= p50_ms <= p95_ms <= p99_ms):
        raise RuntimeError(
            f"invalid latency order: p50={p50_ms}, p95={p95_ms}, p99={p99_ms}")


def validate_grpc_values(marker, values, args):
    strict = args.formal_cell or args.smoke
    if strict and marker != "GRPC_FAILOVER_RATE":
        raise RuntimeError(f"strict gRPC cell requires GRPC_FAILOVER_RATE, got {marker}")
    if not strict:
        return
    integer_keys = (
        "sent", "success", "failures", "attempts", "failovers",
        "health_checks", "health_success", "health_directed_selections",
        "handler_executions_observed", "status_ok", "status_unavailable",
        "status_deadline_exceeded", "status_health_directed_selection",
        "application_rpc_calls", "application_messages",
        "pre_measurement_health_checks", "pre_measurement_health_success",
        "parallel_issued", "parallel_winners", "parallel_cancellations",
    )
    parsed = {key: required_number(values, key, integer=True) for key in integer_keys}
    p50 = required_number(values, "p50_ms")
    p95 = required_number(values, "p95_ms")
    p99 = required_number(values, "p99_ms")
    mean_ms = required_number(values, "mean_ms")
    lateness = required_finite_number(values, "measurement_start_lateness_ms")
    actual_rps = required_number(values, "actual_success_rps")
    validate_latency_order(p50, p95, p99)
    if mean_ms < 0:
        raise RuntimeError("gRPC mean latency must be non-negative")
    expected_sent = int(round(args.duration_s * args.rate_rps))
    if parsed["sent"] != expected_sent:
        raise RuntimeError(
            f"gRPC sent mismatch: expected {expected_sent}, got {parsed['sent']}")
    if parsed["success"] + parsed["failures"] != parsed["sent"]:
        raise RuntimeError("gRPC success + failures must equal sent")
    provider_count = len(configured_provider_nodes(args))
    if not parsed["sent"] <= parsed["attempts"] <= provider_count * parsed["sent"]:
        raise RuntimeError(
            f"gRPC attempts must be within [sent, {provider_count}*sent]")
    parallel = bool(getattr(args, "grpc_parallel", False))
    if parallel:
        if values.get("execution_mode") != "parallel-first-success":
            raise RuntimeError("parallel gRPC execution mode marker is missing")
        if parsed["failovers"] != 0:
            raise RuntimeError("parallel gRPC must not report serial failovers")
        if parsed["parallel_issued"] != parsed["attempts"]:
            raise RuntimeError("parallel gRPC issued-attempt accounting mismatch")
        if parsed["parallel_winners"] != parsed["success"]:
            raise RuntimeError("parallel gRPC winner accounting mismatch")
        if parsed["parallel_cancellations"] > parsed["parallel_issued"]:
            raise RuntimeError("parallel gRPC cancellations exceed issued attempts")
    elif parsed["failovers"] != parsed["attempts"] - parsed["sent"]:
        raise RuntimeError("gRPC failovers must equal actually issued extra attempts")
    if parsed["health_success"] > parsed["health_checks"]:
        raise RuntimeError("gRPC health_success exceeds health_checks")
    if parsed["pre_measurement_health_success"] > parsed["pre_measurement_health_checks"]:
        raise RuntimeError("gRPC pre-measurement health_success exceeds checks")
    if parsed["application_rpc_calls"] != parsed["attempts"] + parsed["health_checks"]:
        raise RuntimeError("gRPC application_rpc_calls accounting mismatch")
    if parsed["application_messages"] != 2 * parsed["application_rpc_calls"]:
        raise RuntimeError("gRPC application_messages accounting mismatch")
    if parallel:
        if parsed["status_ok"] < parsed["success"]:
            raise RuntimeError("parallel gRPC status_ok is below winners")
    elif parsed["status_ok"] != parsed["success"]:
        raise RuntimeError("gRPC status_ok must equal successful logical requests")
    if parsed["status_health_directed_selection"] != parsed["health_directed_selections"]:
        raise RuntimeError("gRPC health-directed selection accounting mismatch")
    if abs(lateness) > TRAFFIC_PHASE_TOLERANCE_S * 1000.0:
        raise RuntimeError("gRPC measurement-start lateness exceeds tolerance")
    if values.get("message_definition") != "rpc_request_plus_terminal_event":
        raise RuntimeError("gRPC message_definition missing or unsupported")
    if not math.isfinite(actual_rps):
        raise RuntimeError("gRPC actual_success_rps must be finite")


def parse_provider_attempts(value):
    result = {}
    for item in value.split(","):
        if ":" not in item:
            continue
        provider, count = item.rsplit(":", 1)
        try:
            parsed = int(count)
        except ValueError as error:
            raise RuntimeError(f"invalid provider_attempts entry: {item}") from error
        if provider in result:
            raise RuntimeError(f"duplicate provider_attempts entry: {provider}")
        if parsed < 0:
            raise RuntimeError(f"negative provider_attempts entry: {item}")
        result[provider] = parsed
    return result


def validate_nsc_values(marker, values, args):
    strict = args.formal_cell or args.smoke
    if strict and marker != "NSC_FAILOVER_SUMMARY":
        raise RuntimeError(f"strict NSC cell requires NSC_FAILOVER_SUMMARY, got {marker}")
    if not strict:
        return
    integer_keys = (
        "count", "success", "terminal_failures", "attempts",
        "attempt_timeouts", "nacks", "failovers", "late_callbacks",
        "application_messages", "notification_interests", "notification_data",
        "input_interests", "input_data", "result_interests", "result_data",
    )
    parsed = {key: required_number(values, key, integer=True) for key in integer_keys}
    p50 = required_number(values, "p50_ms")
    p95 = required_number(values, "p95_ms")
    p99 = required_number(values, "p99_ms")
    mean_ms = required_number(values, "mean_ms")
    lateness = required_finite_number(values, "first_request_start_lateness_ms")
    validate_latency_order(p50, p95, p99)
    if mean_ms < 0:
        raise RuntimeError("NSC mean latency must be non-negative")
    expected_sent = int(round(args.duration_s * args.rate_rps))
    if parsed["count"] != expected_sent:
        raise RuntimeError(
            f"NSC count mismatch: expected {expected_sent}, got {parsed['count']}")
    if parsed["success"] + parsed["terminal_failures"] != parsed["count"]:
        raise RuntimeError("NSC success + terminal_failures must equal count")
    provider_count = len(configured_provider_nodes(args))
    if not parsed["count"] <= parsed["attempts"] <= provider_count * parsed["count"]:
        raise RuntimeError(
            f"NSC attempts must be within [count, {provider_count}*count]")
    if parsed["failovers"] != parsed["attempts"] - parsed["count"]:
        raise RuntimeError("NSC failovers must equal actually issued extra attempts")
    stage_total = sum(parsed[key] for key in (
        "notification_interests", "notification_data", "input_interests",
        "input_data", "result_interests", "result_data"))
    if parsed["application_messages"] != stage_total:
        raise RuntimeError("NSC application_messages accounting mismatch")
    attempts_by_provider = parse_provider_attempts(values.get("provider_attempts", ""))
    if set(attempts_by_provider) != {
            PROVIDER_PREFIXES[name] for name in configured_provider_nodes(args)}:
        raise RuntimeError(
            f"NSC provider_attempts must contain exactly {provider_count} Providers")
    if sum(attempts_by_provider.values()) != parsed["attempts"]:
        raise RuntimeError("NSC provider_attempts do not sum to attempts")
    if abs(lateness) > TRAFFIC_PHASE_TOLERANCE_S * 1000.0:
        raise RuntimeError("NSC first-request lateness exceeds tolerance")
    if values.get("message_definition") != (
            "consumer_observed_accepted_or_sent_nsc_stage_events_"
            "excludes_late_and_wire_retransmissions"):
        raise RuntimeError("NSC message_definition missing or unsupported")


def summarize_grpc_server_stats(snapshot, sent, measurement_start_s, duration_s):
    if snapshot.get("schema") != "ndnsf.grpc.baseline.aggregate-stats.v1":
        raise RuntimeError("gRPC aggregate Stats schema mismatch")
    providers = snapshot.get("providers")
    if not isinstance(providers, dict) or set(providers) != set(provider_nodes()):
        raise RuntimeError(
            f"gRPC Stats snapshot must contain exactly {len(provider_nodes())} Providers")
    aggregate_counts = {}
    exact_handlers = 0
    exact_health_checks = 0
    exact_health_success = 0
    window_handlers = 0
    same_provider_extra = 0
    same_provider_duplicate_ids = set()
    measurement_end_s = measurement_start_s + duration_s
    aggregate_snapshot_s = snapshot.get("snapshot_monotonic_s")
    if (not isinstance(aggregate_snapshot_s, (int, float)) or
            not math.isfinite(aggregate_snapshot_s)):
        raise RuntimeError("invalid gRPC aggregate Stats timestamp")
    for provider_id, provider in providers.items():
        if provider.get("schema") != "ndnsf.grpc.baseline.provider-stats.v1":
            raise RuntimeError(f"gRPC provider Stats schema mismatch: {provider_id}")
        if provider.get("provider_id") != provider_id:
            raise RuntimeError(f"gRPC Stats provider mismatch: {provider_id}")
        if provider.get("stats_epoch") != 0:
            raise RuntimeError(f"unexpected gRPC Stats epoch for {provider_id}")
        reset_s = provider.get("stats_reset_monotonic_s")
        snapshot_s = provider.get("snapshot_monotonic_s")
        if (not all(isinstance(value, (int, float)) and math.isfinite(value)
                    for value in (reset_s, snapshot_s)) or
                reset_s > snapshot_s or snapshot_s > aggregate_snapshot_s):
            raise RuntimeError(f"invalid gRPC Stats snapshot interval for {provider_id}")
        handler_count = provider.get("handler_executions")
        health_checks = provider.get("health_checks")
        health_success = provider.get("health_success")
        if not all(isinstance(value, int) and value >= 0
                   for value in (handler_count, health_checks, health_success)):
            raise RuntimeError(f"invalid gRPC Stats counters for {provider_id}")
        if health_success > health_checks:
            raise RuntimeError(f"gRPC Stats health count mismatch for {provider_id}")
        exact_handlers += handler_count
        exact_health_checks += health_checks
        exact_health_success += health_success
        request_counts = provider.get("request_id_counts")
        if not isinstance(request_counts, dict):
            raise RuntimeError(f"missing request_id_counts for {provider_id}")
        for request_id, count in request_counts.items():
            if not str(request_id).isdigit() or not isinstance(count, int) or count <= 0:
                raise RuntimeError(f"invalid request_id_counts for {provider_id}")
            request_number = int(request_id)
            if request_number < 0 or request_number >= sent:
                raise RuntimeError(f"out-of-window request ID {request_id} for {provider_id}")
            aggregate_counts[str(request_number)] = aggregate_counts.get(str(request_number), 0) + count
            if count > 1:
                same_provider_extra += count - 1
                same_provider_duplicate_ids.add(str(request_number))
        service_events = provider.get("service_events")
        if not isinstance(service_events, list) or len(service_events) != handler_count:
            raise RuntimeError(f"gRPC service event count mismatch for {provider_id}")
        if sum(request_counts.values()) != handler_count:
            raise RuntimeError(f"gRPC request count mismatch for {provider_id}")
        health_events = provider.get("health_events")
        if not isinstance(health_events, list) or len(health_events) != health_checks:
            raise RuntimeError(f"gRPC health event count mismatch for {provider_id}")
        for event in service_events + health_events:
            if not isinstance(event, dict):
                raise RuntimeError(f"malformed gRPC Stats event for {provider_id}")
            started = event.get("started_monotonic_s")
            if not isinstance(started, (int, float)) or not math.isfinite(started):
                raise RuntimeError(f"invalid gRPC Stats timestamp for {provider_id}")
            if not isinstance(event.get("status"), str):
                raise RuntimeError(f"invalid gRPC Stats status for {provider_id}")
            completed = event.get("completed_monotonic_s")
            if (not isinstance(completed, (int, float)) or
                    not math.isfinite(completed) or completed < started or
                    event["status"] == "IN_PROGRESS"):
                raise RuntimeError(f"non-terminal gRPC Stats event for {provider_id}")
            if event.get("stats_epoch") != 0:
                raise RuntimeError(f"gRPC Stats event epoch mismatch for {provider_id}")
        service_ids = [event.get("event_id") for event in service_events]
        health_ids = [event.get("event_id") for event in health_events]
        if (service_ids != list(range(handler_count)) or
                health_ids != list(range(health_checks))):
            raise RuntimeError(f"gRPC Stats event IDs are not canonical for {provider_id}")
        for event in service_events:
            request_id = str(event.get("request_id"))
            attempt = event.get("attempt")
            if (not request_id.isdigit() or int(request_id) >= sent or
                    not isinstance(attempt, int) or
                    not 1 <= attempt <= len(provider_nodes())):
                raise RuntimeError(f"invalid gRPC service event for {provider_id}")
        derived_request_counts = dict(Counter(
            str(event["request_id"]) for event in service_events))
        if derived_request_counts != request_counts:
            raise RuntimeError(f"gRPC request/event counts differ for {provider_id}")
        if dict(Counter(event["status"] for event in service_events)) != provider.get(
                "service_status_counts"):
            raise RuntimeError(f"gRPC service status counts differ for {provider_id}")
        if dict(Counter(event["status"] for event in health_events)) != provider.get(
                "health_status_counts"):
            raise RuntimeError(f"gRPC health status counts differ for {provider_id}")
        if sum(event["status"] == "SERVING" for event in health_events) != health_success:
            raise RuntimeError(f"gRPC health success events differ for {provider_id}")
        window_handlers += sum(
            1 for event in service_events
            if isinstance(event, dict) and
            measurement_start_s <= float(event.get("started_monotonic_s", -1)) < measurement_end_s)
    multi_provider_ids = {
        request_id for request_id, count in aggregate_counts.items() if count > 1}
    return {
        "server_handler_executions_snapshot_exact": exact_handlers,
        "server_handler_executions_started_in_client_window_exact": window_handlers,
        "server_health_checks_total_snapshot": exact_health_checks,
        "server_health_success_total_snapshot": exact_health_success,
        "server_request_id_execution_counts": dict(sorted(
            aggregate_counts.items(), key=lambda item: int(item[0]))),
        "server_unique_request_ids_executed": len(aggregate_counts),
        "server_extra_executions_per_request_exact": sum(
            count - 1 for count in aggregate_counts.values() if count > 1),
        "server_request_ids_with_multiple_executions_exact": len(multi_provider_ids),
        "server_same_provider_extra_executions_exact": same_provider_extra,
        "server_request_ids_with_same_provider_duplicates_exact": len(
            same_provider_duplicate_ids),
    }


def advance_mobility(mobility, duration_s, step_s=TRACE_INTERVAL_S):
    """Advance a mobility model before timestamp zero without wall-clock delay."""
    duration_s = float(duration_s)
    step_s = float(step_s)
    if duration_s < 0 or not math.isfinite(duration_s):
        raise ValueError("mobility_warmup_s must be finite and non-negative")
    if step_s <= 0 or not math.isfinite(step_s):
        raise ValueError("mobility warm-up step must be finite and positive")
    remaining_s = duration_s
    while remaining_s > 1e-9:
        increment_s = min(step_s, remaining_s)
        mobility.step(increment_s)
        remaining_s -= increment_s


def generate_mobility_trace(path, ap_range, seed, horizon_s,
                            interval_s=TRACE_INTERVAL_S, profile="random-waypoint",
                            handoff_period_s=HANDOFF_PERIOD_S,
                            mobility_warmup_s=0.0, measurement_start_s=None,
                            measurement_duration_s=None):
    """Generate the canonical deterministic availability schedule for one range."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if profile not in {"random-waypoint", "forced-failover-smoke", "single-active-handoff"}:
        raise ValueError(f"unsupported mobility trace profile: {profile}")
    if handoff_period_s <= 0 or not math.isfinite(handoff_period_s):
        raise ValueError("handoff_period_s must be finite and positive")
    if (measurement_start_s is None) != (measurement_duration_s is None):
        raise ValueError(
            "measurement_start_s and measurement_duration_s must be provided together")
    if measurement_start_s is not None:
        measurement_start_s = float(measurement_start_s)
        measurement_duration_s = float(measurement_duration_s)
        if (measurement_start_s < 0 or not math.isfinite(measurement_start_s) or
                measurement_duration_s <= 0 or
                not math.isfinite(measurement_duration_s)):
            raise ValueError("measurement trace window must be finite and positive")
    mobility = None if profile == "single-active-handoff" else RandomWaypointCoverage(
        provider_nodes(), seed, min_speed=ACTIVE_SPEED_MPS[0],
        max_speed=ACTIVE_SPEED_MPS[1], ap_positions=active_ap_positions())
    if mobility is not None:
        advance_mobility(mobility, mobility_warmup_s)
    elif mobility_warmup_s < 0 or not math.isfinite(float(mobility_warmup_s)):
        raise ValueError("mobility_warmup_s must be finite and non-negative")
    handoff_order = list(provider_nodes())
    if profile == "single-active-handoff":
        # Keep one reachable Provider per epoch while making repetitions
        # genuinely trace-distinct.  The order is frozen in the trace
        # metadata, so this is not a post-hoc selection of a favorable cell.
        random.Random(seed).shuffle(handoff_order)
    extended = ACTIVE_PROFILE in {"four-provider-single-ap", "four-provider-multi-ap"}
    measurement_reachable_counts = Counter()
    measurement_provider_reachable = Counter()
    measurement_epoch_count = 0
    with path.open("w", newline="") as raw:
        writer = csv.writer(raw, lineterminator="\n")
        writer.writerow((
            "time_s", "provider", "x", "y", "distance_m", "in_range",
            *(('nearest_ap',) if extended else ()),
        ))
        step = 0
        while step * interval_s <= horizon_s + 1e-9:
            timestamp = step * interval_s
            in_measurement_window = (
                measurement_start_s is not None and
                measurement_start_s <= timestamp <
                measurement_start_s + measurement_duration_s)
            if profile == "single-active-handoff":
                active_index = int(timestamp / handoff_period_s) % len(handoff_order)
                active_name = handoff_order[active_index]
                active_ap = active_ap_positions()[active_index % len(active_ap_positions())]
                rows = []
                for name in provider_nodes():
                    if name == active_name:
                        x, y = active_ap
                    else:
                        x, y = 400.0, 400.0
                    distance, ap_index = min(
                        (math.hypot(x - ap_x, y - ap_y), index)
                        for index, (ap_x, ap_y)
                        in enumerate(active_ap_positions()))
                    nearest_ap = f"ap{ap_index + 1}"
                    rows.append((name, x, y, distance, nearest_ap))
            else:
                rows = mobility.snapshot(
                    ap_positions=active_ap_positions(), include_ap=True)
            reachable_count = 0
            for name, x, y, distance, nearest_ap in rows:
                allowed = distance <= ap_range
                # The short smoke campaign needs a deterministic outage longer
                # than the registered 1 s retry timeout; normal RandomWaypoint
                # movement cannot leave a 200 m AP in five seconds.
                if (profile == "forced-failover-smoke" and name == "ucla" and
                        SMOKE_OUTAGE_START_S <= timestamp < SMOKE_OUTAGE_END_S):
                    x, y = 400.0, 400.0
                    distance, nearest_ap = mobility.nearest_distance(
                        x, y, active_ap_positions())
                    allowed = False
                if in_measurement_window and allowed:
                    reachable_count += 1
                    measurement_provider_reachable[name] += 1
                row = (
                    f"{timestamp:.3f}", name, f"{x:.2f}", f"{y:.2f}",
                    f"{distance:.2f}", int(allowed),
                )
                writer.writerow(row + ((nearest_ap,) if extended else ()))
            if in_measurement_window:
                measurement_reachable_counts[reachable_count] += 1
                measurement_epoch_count += 1
            if mobility is not None:
                mobility.step(interval_s)
            step += 1
    measurement_coverage = None
    if measurement_start_s is not None:
        if measurement_epoch_count <= 0:
            raise ValueError("measurement trace window contains no epochs")
        measurement_coverage = {
            "start_s": measurement_start_s,
            "duration_s": measurement_duration_s,
            "epoch_count": measurement_epoch_count,
            "reachable_provider_count_epochs": {
                str(count): measurement_reachable_counts[count]
                for count in sorted(measurement_reachable_counts)
            },
            "at_least_one_fraction": (
                1.0 - measurement_reachable_counts[0] / measurement_epoch_count),
            "all_unreachable_fraction": (
                measurement_reachable_counts[0] / measurement_epoch_count),
            "at_least_two_fraction": (
                sum(value for count, value in measurement_reachable_counts.items()
                    if count >= 2) / measurement_epoch_count),
            "all_providers_fraction": (
                measurement_reachable_counts[len(provider_nodes())] /
                measurement_epoch_count),
            "mean_reachable_providers": (
                sum(count * value for count, value
                    in measurement_reachable_counts.items()) /
                measurement_epoch_count),
            "provider_in_range_fraction": {
                name: measurement_provider_reachable[name] / measurement_epoch_count
                for name in provider_nodes()
            },
        }
    metadata = {
        "schema": "ndnsf-mobility-trace-v1",
        "generated_at": utc_now(),
        "path": str(path.resolve()),
        "sha256": sha256_file(path),
        "seed": int(seed),
        "range_m": float(ap_range),
        "horizon_s": float(horizon_s),
        "interval_s": float(interval_s),
        "profile": profile,
        "mobility_warmup_s": float(mobility_warmup_s),
        "measurement_coverage": measurement_coverage,
        "handoff_period_s": float(handoff_period_s)
        if profile == "single-active-handoff" else None,
        "providers": provider_nodes(),
        "handoff_order": handoff_order if profile == "single-active-handoff" else None,
        "provider_count": len(provider_nodes()),
        "ap_layout": ACTIVE_AP_LAYOUT,
        "ap_positions_m": [list(position) for position in active_ap_positions()],
        "model": {
            "name": (
                "SingleActiveHandoffSchedule"
                if profile == "single-active-handoff" else "RandomWaypointCoverage"
            ),
            "area_m": [400.0, 400.0],
            "ap_positions_m": [list(position) for position in active_ap_positions()],
            "initial_radius_m": 50.0,
            "mobility_warmup_s": float(mobility_warmup_s),
            "speed_mps": list(ACTIVE_SPEED_MPS),
        },
        "forced_smoke_transition": (
            {"provider": "ucla", "start_s": SMOKE_OUTAGE_START_S,
             "end_s": SMOKE_OUTAGE_END_S,
             "position_m": [400.0, 400.0]}
            if profile == "forced-failover-smoke" else None),
    }
    write_json(path.with_suffix(".meta.json"), metadata)
    return metadata


def load_mobility_trace(path, expected_range=None, expected_seed=None):
    """Load and strictly validate a canonical provider trace."""
    path = Path(path)
    if not path.is_file():
        raise RuntimeError(f"mobility trace not found: {path}")
    groups = []
    current_time = None
    current_rows = []
    with path.open(newline="") as raw:
        reader = csv.DictReader(raw)
        required = {"time_s", "provider", "x", "y", "distance_m", "in_range"}
        optional = {"nearest_ap", "applied_unix_s", "applied_monotonic_s"}
        fields = set(reader.fieldnames or ())
        if not required.issubset(fields) or not fields.issubset(required | optional):
            raise RuntimeError(f"invalid mobility trace columns in {path}: {reader.fieldnames}")
        for row in reader:
            parsed = {
                "time_s": float(row["time_s"]),
                "provider": row["provider"],
                "x": float(row["x"]),
                "y": float(row["y"]),
                "distance_m": float(row["distance_m"]),
                "in_range": bool(int(row["in_range"])),
            }
            if "nearest_ap" in fields:
                parsed["nearest_ap"] = row["nearest_ap"]
            if current_time is None or abs(parsed["time_s"] - current_time) < 1e-9:
                current_time = parsed["time_s"]
                current_rows.append(parsed)
            else:
                groups.append((current_time, current_rows))
                current_time = parsed["time_s"]
                current_rows = [parsed]
    if current_time is not None:
        groups.append((current_time, current_rows))
    if not groups:
        raise RuntimeError(f"empty mobility trace: {path}")
    expected_providers = set(provider_nodes())
    previous = -1.0
    for timestamp, rows in groups:
        if timestamp < previous:
            raise RuntimeError(f"non-monotonic mobility trace: {path}")
        previous = timestamp
        actual = {row["provider"] for row in rows}
        if actual != expected_providers or len(rows) != len(expected_providers):
            raise RuntimeError(
                f"trace time {timestamp} has providers {sorted(actual)}, "
                f"expected {sorted(expected_providers)}")
    meta_path = path.with_suffix(".meta.json")
    if meta_path.is_file():
        meta = json.loads(meta_path.read_text())
        if expected_range is not None and float(meta.get("range_m")) != float(expected_range):
            raise RuntimeError(f"trace range mismatch in {meta_path}")
        if expected_seed is not None and int(meta.get("seed")) != int(expected_seed):
            raise RuntimeError(f"trace seed mismatch in {meta_path}")
        if meta.get("sha256") != sha256_file(path):
            raise RuntimeError(f"trace hash mismatch in {meta_path}")
    return groups


def provider_nodes():
    return list(ACTIVE_PROVIDER_NAMES)


def configured_provider_nodes(args=None, configuration=None):
    """Return the provider endpoints used by this workload.

    The topology and mobility trace always retain the complete provider set.
    A single-provider control narrows only the client-side target list, so it
    measures the no-failover behavior under the same AP/trace conditions.
    """
    raw = getattr(args, "provider_scope", "") if args is not None else ""
    if configuration is not None:
        raw = configuration.get("provider_scope", raw)
    if isinstance(raw, str) and raw.strip():
        names = [item.strip() for item in raw.split(",") if item.strip()]
    elif raw:
        names = list(raw)
    else:
        names = provider_nodes()
    expected = set(provider_nodes())
    if not names or len(set(names)) != len(names) or not set(names) <= expected:
        raise RuntimeError(
            f"provider_scope must be a non-empty subset of {sorted(expected)}")
    return names


def base_system_id(system):
    return "grpc" if system == "grpc-single" else (
        "nsc" if system == "nsc-single" else system)


def all_app_nodes(ndn):
    return [ndn.net["memphis"]] + [ndn.net[name] for name in provider_nodes()]


def make_perf_args():
    return SimpleNamespace(
        providers=len(provider_nodes()),
        provider_nodes=",".join(provider_nodes()),
        user_node="memphis",
        controller_node="memphis",
        workload_mode="open-loop",
        # Keep the SVS fetch-window controller informed of the same offered
        # load used by the mobility user.  Without this field app_env() emits
        # expectedRps=0 even though the user sends at 5 RPS.
        rate_rps=5.0,
        # App_WifiMobilityUser is backed by App_IntermittentUser, whose
        # admission control is intentionally disabled for matched mobility
        # comparisons.  Keep the setting explicit in the harness arguments
        # and evidence even though the specialized binary enforces it.
        adaptive_admission_control=False,
        disable_adaptive_admission_control=True,
        performance_mode=False,
        ack_threads=-1,
        timeline_trace_sample_rate=100,
        serve_provider_certs=False,
        debug_ack=False,
        timeline_trace=False,
        dk_bootstrap_check=False,
        crypto_diagnostics=False,
        diag_plaintext_ack=False,
        diag_plaintext_response=False,
        svs_parallel_sync_processing=False,
        svs_parallel_workers=4,
        svs_parallel_queue=256,
        svs_sync_publish=False,
        svs_disable_parallel_production=False,
        svs_parallel_production_workers=None,
        svs_disable_parallel_production_signing=False,
        svs_parallel_production_signing=False,
        svs_disable_parallel_production_extra_block=False,
        svs_parallel_production_extra_block=False,
        svs_sync_batching=False,
        svs_sync_batch_ms=0,
    )


def wait_for_log(path, pattern, timeout_s, process=None):
    regex = re.compile(pattern)
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if process is not None and process.poll() is not None:
            return False
        if path.exists() and regex.search(path.read_text(errors="replace")):
            return True
        time.sleep(0.2)
    return False


def start_process(node, name, command, output_dir, env=None):
    log_path = output_dir / f"{name}.log"
    log(f"Starting {name} on {node.name} -> {log_path}")
    log_file = log_path.open("xb")
    process = getPopen(node, command, envDict=env, shell=True,
                       stdout=log_file, stderr=subprocess.STDOUT)
    return process, log_file, log_path


def stop_processes(processes):
    for process, log_file, _ in reversed(processes):
        if process.poll() is None:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=3)
            except Exception:
                process.kill()
        log_file.close()


class RandomWaypointCoverage:
    def __init__(self, provider_names, seed, area_size=400.0, min_speed=1.0,
                 max_speed=2.0, ap_x=200.0, ap_y=200.0, initial_radius=50.0,
                 ap_positions=None):
        self.rng = random.Random(seed)
        self.area_size = area_size
        self.min_speed = min_speed
        self.max_speed = max_speed
        self.ap_positions = tuple(ap_positions or ((ap_x, ap_y),))
        self.states = {}
        for name in provider_names:
            angle = self.rng.uniform(0.0, 2.0 * math.pi)
            self.states[name] = {
                "x": self.ap_positions[0][0] + math.cos(angle) * initial_radius,
                "y": self.ap_positions[0][1] + math.sin(angle) * initial_radius,
                "target_x": self.rng.uniform(0, area_size),
                "target_y": self.rng.uniform(0, area_size),
                "speed": self.rng.uniform(min_speed, max_speed),
            }

    def step(self, dt):
        for state in self.states.values():
            dx = state["target_x"] - state["x"]
            dy = state["target_y"] - state["y"]
            dist = math.hypot(dx, dy)
            travel = state["speed"] * dt
            if dist <= travel or dist < 1e-6:
                state["x"] = state["target_x"]
                state["y"] = state["target_y"]
                state["target_x"] = self.rng.uniform(0, self.area_size)
                state["target_y"] = self.rng.uniform(0, self.area_size)
                state["speed"] = self.rng.uniform(self.min_speed, self.max_speed)
            else:
                state["x"] += dx / dist * travel
                state["y"] += dy / dist * travel

    def nearest_distance(self, x, y, ap_positions=None):
        positions = tuple(ap_positions or self.ap_positions)
        distance, index = min(
            (math.hypot(x - ap_x, y - ap_y), position_index)
            for position_index, (ap_x, ap_y) in enumerate(positions))
        return distance, f"ap{index + 1}"

    def snapshot(self, ap_x=200.0, ap_y=200.0, *, ap_positions=None,
                 include_ap=False):
        positions = tuple(ap_positions or self.ap_positions)
        rows = []
        for name, state in self.states.items():
            distance, nearest_ap = self.nearest_distance(
                state["x"], state["y"], positions)
            row = (name, state["x"], state["y"], distance)
            rows.append(row + ((nearest_ap,) if include_ap else ()))
        return rows


def start_coverage_gate(ndn, output_dir, ap_range, stop_event, seed,
                        interval_s=1.0, block_network=False, pause_processes=None,
                        trace_replay_path=None, epoch_monotonic=None,
                        repair_ndn_faces=False, mobility_warmup_s=0.0):
    providers = [ndn.net[name] for name in provider_nodes()]
    mobility = None if trace_replay_path else RandomWaypointCoverage(
        provider_nodes(), seed, min_speed=ACTIVE_SPEED_MPS[0],
        max_speed=ACTIVE_SPEED_MPS[1], ap_positions=active_ap_positions())
    if mobility is not None:
        advance_mobility(mobility, mobility_warmup_s)
    replay_groups = None
    if trace_replay_path:
        replay_groups = load_mobility_trace(
            trace_replay_path, expected_range=ap_range, expected_seed=seed)
    trace_path = output_dir / "mobility_trace.csv"
    status_dir = output_dir / "availability"
    status_dir.mkdir(parents=True, exist_ok=True)
    in_range = {}
    pause_processes = pause_processes or {}
    epoch_monotonic = epoch_monotonic if epoch_monotonic is not None else time.monotonic()

    def apply_gate(node, allowed):
        iface = f"{node.name}-wlan0"
        node.cmd("iptables -F NDNSF_WIFI_RANGE >/dev/null 2>&1 || true")
        node.cmd("iptables -N NDNSF_WIFI_RANGE >/dev/null 2>&1 || true")
        node.cmd("iptables -C INPUT -j NDNSF_WIFI_RANGE >/dev/null 2>&1 || "
                 "iptables -I INPUT 1 -j NDNSF_WIFI_RANGE")
        node.cmd("iptables -C OUTPUT -j NDNSF_WIFI_RANGE >/dev/null 2>&1 || "
                 "iptables -I OUTPUT 1 -j NDNSF_WIFI_RANGE")
        if not allowed:
            node.cmd(f"iptables -A NDNSF_WIFI_RANGE -i {iface} -j DROP")
            node.cmd(f"iptables -A NDNSF_WIFI_RANGE -o {iface} -j DROP")

    def capture_gate_counters():
        if not block_network:
            return
        counters_path = output_dir / "network-gate-counters.txt"
        with counters_path.open("a", encoding="utf-8") as counters:
            counters.write(f"snapshot_unix_s={time.time():.6f}\n")
            for node in providers:
                iface = f"{node.name}-wlan0"
                output = node.cmd(
                    "iptables -L NDNSF_WIFI_RANGE -v -n -x 2>/dev/null || true")
                counters.write(f"provider={node.name} interface={iface}\n")
                counters.write(output)
                if not output.endswith("\n"):
                    counters.write("\n")

    def cleanup_gate():
        capture_gate_counters()
        for node in providers:
            node.cmd("iptables -D INPUT -j NDNSF_WIFI_RANGE >/dev/null 2>&1 || true")
            node.cmd("iptables -D OUTPUT -j NDNSF_WIFI_RANGE >/dev/null 2>&1 || true")
            node.cmd("iptables -F NDNSF_WIFI_RANGE >/dev/null 2>&1 || true")
            node.cmd("iptables -X NDNSF_WIFI_RANGE >/dev/null 2>&1 || true")

    def apply_provider_state(name, allowed):
        node = ndn.net[name]
        previous = in_range.get(name)
        if previous != allowed:
            if block_network:
                apply_gate(node, allowed)
                if allowed and previous is False and repair_ndn_faces:
                    configure_ndn_multicast(
                        all_app_nodes(ndn), target_names={name}, recreate_faces=True)
                    log(f"NFD_FACE_RECONNECT_REPAIR provider={name}")
                    if os.environ.get("NDNSF_MOBILITY_REPAIR_NFD_DIAGNOSTICS") == "1":
                        snapshot = node.cmd(
                            "nfdc face list 2>&1; nfdc route list 2>&1")
                        with (output_dir / "nfd-reconnect-state.log").open(
                                "a", encoding="utf-8") as stream:
                            stream.write(
                                f"timestamp_unix_s={time.time():.6f} provider={name}\n")
                            stream.write(snapshot)
                            if not snapshot.endswith("\n"):
                                stream.write("\n")
            process = pause_processes.get(name)
            if process is not None and process.poll() is None:
                process.send_signal(signal.SIGCONT if allowed else signal.SIGSTOP)
            in_range[name] = allowed
        (status_dir / f"{name}.state").write_text("1\n" if allowed else "0\n")

    def run_replay(out):
        extended = any("nearest_ap" in row for _, rows in replay_groups for row in rows)
        out.write("time_s,provider,x,y,distance_m,in_range,"
                  "applied_unix_s,applied_monotonic_s" +
                  (",nearest_ap\n" if extended else "\n"))
        for timestamp, rows in replay_groups:
            remaining = epoch_monotonic + timestamp - time.monotonic()
            if remaining > 0 and stop_event.wait(remaining):
                break
            if stop_event.is_set():
                break
            for row in rows:
                apply_provider_state(row["provider"], row["in_range"])
                applied_unix_s = time.time()
                applied_monotonic_s = time.monotonic()
                line = (
                    f"{timestamp:.3f},{row['provider']},{row['x']:.2f},"
                    f"{row['y']:.2f},{row['distance_m']:.2f},{int(row['in_range'])},"
                    f"{applied_unix_s:.6f},{applied_monotonic_s:.6f}")
                if extended:
                    line += f",{row.get('nearest_ap', '')}"
                out.write(line + "\n")
            out.flush()

    def run():
        with trace_path.open("w") as out:
            if replay_groups is not None:
                run_replay(out)
            else:
                extended = ACTIVE_PROFILE in {
                    "four-provider-single-ap", "four-provider-multi-ap"}
                out.write("time_s,provider,x,y,distance_m,in_range,"
                          "applied_unix_s,applied_monotonic_s" +
                          (",nearest_ap\n" if extended else "\n"))
                start = time.time()
                while not stop_event.is_set():
                    now = time.time() - start
                    for name, x, y, distance, nearest_ap in mobility.snapshot(
                            ap_positions=active_ap_positions(), include_ap=True):
                        allowed = distance <= ap_range
                        apply_provider_state(name, allowed)
                        applied_unix_s = time.time()
                        applied_monotonic_s = time.monotonic()
                        line = (
                            f"{now:.3f},{name},{x:.2f},{y:.2f},{distance:.2f},"
                            f"{int(allowed)},{applied_unix_s:.6f},"
                            f"{applied_monotonic_s:.6f}")
                        if extended:
                            line += f",{nearest_ap}"
                        out.write(line + "\n")
                        out.flush()
                    mobility.step(interval_s)
                    stop_event.wait(interval_s)
        if block_network:
            cleanup_gate()
        for process in pause_processes.values():
            if process is not None and process.poll() is None:
                process.send_signal(signal.SIGCONT)

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return thread, trace_path, status_dir


def node_cmd(node, command):
    home = node.params["params"]["homeDir"]
    return node.cmd(f"HOME={perf.shell_quote(home)} {command}")


def initialize_keychains(nodes, controller, output_dir):
    security_dir = output_dir / "security"
    security_dir.mkdir(parents=True, exist_ok=True)
    identity_owners = {
        "/example/hello/controller": "memphis",
        "/example/hello/user": "memphis",
        "/example/hello/provider": "ucla",
    }
    identity_owners.update({
        f"/example/hello/provider/{chr(ord('A') + index)}": name
        for index, name in enumerate(provider_nodes())
    })
    nodes_by_name = {node.name: node for node in nodes}
    for node in nodes:
        for identity in ["/example/hello"] + list(identity_owners):
            node_cmd(node, f"ndnsec delete {perf.shell_quote(identity)} >/dev/null 2>&1 || true")
    root_cert = security_dir / "root.cert"
    node_cmd(controller, f"ndnsec key-gen -t r /example/hello > {perf.shell_quote(root_cert)}")
    node_cmd(controller, f"ndnsec cert-install -f {perf.shell_quote(root_cert)} >/dev/null 2>&1 || true")
    certs = []
    for index, (identity, owner_name) in enumerate(identity_owners.items()):
        owner = nodes_by_name[owner_name]
        cert = security_dir / f"identity-{index}.cert"
        req = security_dir / f"identity-{index}.req"
        node_cmd(owner, f"ndnsec key-gen -n -t r {perf.shell_quote(identity)} > {perf.shell_quote(req)}")
        node_cmd(controller, f"ndnsec cert-gen -s /example/hello -i ROOT {perf.shell_quote(req)} > {perf.shell_quote(cert)}")
        certs.append(cert)
    for node in nodes:
        node_cmd(node, f"ndnsec cert-install -f {perf.shell_quote(root_cert)} >/dev/null 2>&1 || true")
        for cert in certs:
            node_cmd(node, f"ndnsec cert-install -f {perf.shell_quote(cert)} >/dev/null 2>&1 || true")

    # A fresh MiniNDN home does not have a public-key entry for a Provider
    # identity on the controller, so cert-install alone cannot import a new
    # fourth identity.  Transfer each provider's exact key/certificate as a
    # short-lived SafeBag; this keeps controller-side permission encryption
    # bound to the same key the Provider owns.  Bags never remain in evidence.
    bag_dir = Path("/tmp") / f"ndnsf-mobility-safebags-{os.getpid()}"
    bag_dir.mkdir(parents=True, exist_ok=True)
    try:
        for index, provider_name in enumerate(provider_nodes()):
            identity = f"/example/hello/provider/{chr(ord('A') + index)}"
            owner = nodes_by_name[provider_name]
            bag_path = bag_dir / f"provider-{index}.safe"
            passphrase = uuid.uuid4().hex
            exported = node_cmd(
                owner,
                f"ndnsec export -i {perf.shell_quote(identity)} "
                f"-P {perf.shell_quote(passphrase)} -o {perf.shell_quote(bag_path)} "
                "&& echo NDNSF_SAFEBAG_EXPORTED")
            if "NDNSF_SAFEBAG_EXPORTED" not in exported:
                raise RuntimeError(f"failed to export provider SafeBag: {identity}")
            imported = node_cmd(
                controller,
                f"ndnsec import -P {perf.shell_quote(passphrase)} "
                f"-i {perf.shell_quote(bag_path)} && echo NDNSF_SAFEBAG_IMPORTED")
            if "NDNSF_SAFEBAG_IMPORTED" not in imported:
                raise RuntimeError(f"failed to import provider SafeBag: {identity}")
    finally:
        for bag_path in bag_dir.glob("*.safe"):
            bag_path.unlink(missing_ok=True)
        bag_dir.rmdir()


def configure_ndn_multicast(nodes, target_names=None, recreate_faces=False):
    all_nodes = list(nodes)
    targets = [
        node for node in all_nodes
        if target_names is None or node.name in target_names
    ]
    identity_owners = {
        "/example/hello/controller": "memphis",
        "/example/hello/user": "memphis",
        "/muas/memphis": "memphis",
    }
    identity_owners.update({
        f"/example/hello/provider/{chr(ord('A') + index)}": name
        for index, name in enumerate(provider_nodes())
    })
    identity_owners.update({f"/muas/{name}": name for name in provider_nodes()})
    group_prefixes = ["/example/hello/group", "/muas"]

    face_uri = {}
    face_nexthop = {}
    diagnose_recreate = (
        recreate_faces and
        os.environ.get("NDNSF_MOBILITY_REPAIR_NFD_DIAGNOSTICS") == "1")
    for node in targets:
        for peer in all_nodes:
            if peer.name == node.name:
                continue
            uri = f"udp4://{peer.IP()}"
            face_uri[(node.name, peer.name)] = uri
            if recreate_faces:
                destroy_result = node.cmd(f"nfdc face destroy {uri} 2>&1 || true")
                create_result = node.cmd(f"nfdc face create {uri} 2>&1")
                match = re.search(r"(?:faceid|id)=(\d+)", create_result)
                face_nexthop[(node.name, peer.name)] = (
                    match.group(1) if match else uri)
                if diagnose_recreate:
                    log(
                        f"NFD_FACE_RECREATE provider={node.name} peer={peer.name} "
                        f"destroy={destroy_result.strip()!r} create={create_result.strip()!r}")
            else:
                node.cmd(f"nfdc face create {uri} >/dev/null 2>&1 || true")
                face_nexthop[(node.name, peer.name)] = uri

    for node in targets:
        for prefix in group_prefixes:
            node.cmd(f"nfdc strategy set {prefix} /localhost/nfd/strategy/multicast >/dev/null 2>&1 || true")
            for peer in all_nodes:
                if peer.name != node.name:
                    nexthop = face_nexthop[(node.name, peer.name)]
                    route_command = (
                        f"nfdc route add prefix {prefix} nexthop {nexthop} cost 100")
                    route_result = node.cmd(
                        route_command + (" 2>&1" if recreate_faces else
                                         " >/dev/null 2>&1 || true"))
                    if diagnose_recreate:
                        log(
                            f"NFD_ROUTE_RECREATE provider={node.name} peer={peer.name} "
                            f"prefix={prefix} nexthop={nexthop} result={route_result.strip()!r}")
        for prefix, owner_name in identity_owners.items():
            node.cmd(f"nfdc strategy set {prefix} /localhost/nfd/strategy/best-route >/dev/null 2>&1 || true")
            if owner_name != node.name:
                nexthop = face_nexthop[(node.name, owner_name)]
                route_command = (
                    f"nfdc route add prefix {prefix} nexthop {nexthop} cost 10")
                route_result = node.cmd(
                    route_command + (" 2>&1" if recreate_faces else
                                     " >/dev/null 2>&1 || true"))
                if diagnose_recreate:
                    log(
                        f"NFD_ROUTE_RECREATE provider={node.name} owner={owner_name} "
                        f"prefix={prefix} nexthop={nexthop} result={route_result.strip()!r}")


def build_wifi_topology(ap_range, seed):
    Minindn.cleanUp()
    original_argv = sys.argv[:]
    try:
        sys.argv = [sys.argv[0]]
        Minindn.verifyDependencies()
        ndn = MinindnWifi(noTopo=True, link=wmediumd,
                          wmediumd_mode=interference)
    finally:
        sys.argv = original_argv
    net = ndn.net
    net.setPropagationModel(model="logDistance", exp=3)
    memphis = net.addHost("memphis", ip="10.0.0.1/24")
    station_positions = {
        "ucla": (190, 200), "wustl": (200, 190), "uiuc": (210, 200),
        "arizona": (200, 210),
    }
    stations = {
        name: net.addStation(
            name, ip=f"10.0.0.{index + 2}/24",
            position=f"{station_positions[name][0]},{station_positions[name][1]},0")
        for index, name in enumerate(provider_nodes())
    }
    ap1 = net.addAccessPoint("ap1", ssid="ndnsf-wifi", mode="g", channel="1",
                             position="200,200,0", range=int(ap_range))
    c0 = net.addController("c0")
    net.configureNodes()
    net.addLink(memphis, ap1)
    for station in stations.values():
        net.addLink(station, ap1)
    net.build()
    c0.start()
    ap1.start([c0])
    ndn.initParams([memphis] + list(stations.values()))
    time.sleep(3)
    return ndn


def app_env(output_dir, session_base):
    env = perf.app_env(output_dir, session_base, make_perf_args())
    # The mobility harness may run an isolated NDNSF build against an isolated
    # NDN-SVS installation.  Keep the selected framework and SVS runtime ahead
    # of the repository's default build so a campaign cannot silently mix ABI
    # generations.  The default remains the historical build when no override
    # is supplied.
    runtime_lib_dir = os.environ.get("NDNSF_MOBILITY_RUNTIME_LIB_DIR", "")
    runtime_parts = []
    if runtime_lib_dir:
        runtime_parts.append(str(Path(runtime_lib_dir).expanduser().resolve()))
    runtime_parts.append(str(MOBILITY_BUILD_DIR))
    inherited = env.get("LD_LIBRARY_PATH", "")
    if inherited:
        runtime_parts.append(inherited)
    env["LD_LIBRARY_PATH"] = ":".join(dict.fromkeys(runtime_parts))
    # Keep optional NDNSF lifecycle tracing scoped to NDNSF processes.  The
    # NSC baseline inherits the process environment and its older ndn-cxx
    # logging parser rejects this framework-specific trace configuration.
    ndnsf_log = os.environ.get("NDNSF_MOBILITY_NDN_LOG", "")
    if ndnsf_log:
        env["NDN_LOG"] = ndnsf_log
    return env


def write_mobility_policy(output_dir):
    """Create a profile-local controller policy with one block per Provider."""
    policy_path = Path(output_dir) / "mobility.policies"
    allow = "\n".join(
        "            " + prefix for prefix in (
            "/HELLO", "/NDNSF/DistributedRepo/Store",
            "/NDNSF/DistributedRepo/Store/ROLE/artifact-replica-0",
            "/NDNSF/DistributedRepo/Artifact/v2/STORE",
            "/NDNSF/DistributedRepo/Artifact/v2/STORE/ROLE/artifact-replica-0",
        ))
    provider_blocks = []
    for index, _ in enumerate(provider_nodes()):
        provider_blocks.append(
            "    provider-policy\n    {\n"
            f"        for /example/hello/provider/{chr(ord('A') + index)}\n"
            "        allow\n        {\n" + allow + "\n        }\n    }")
    policy_path.write_text(
        "name /example/hello/controller/NDNSF/ControllerPolicy/v1\n\n"
        "provider-policies\n{\n" + "\n".join(provider_blocks) +
        "\n}\n\nuser-policies\n{\n"
        "    user-policy\n    {\n"
        "        for /example/hello/user\n        allow\n        {\n"
        "            /HELLO\n"
        "            /NDNSF/DistributedRepo/Store\n"
        "            /NDNSF/DistributedRepo/Artifact/v2/STORE\n"
        "        }\n    }\n}\n")
    return policy_path


def run_ndnsf(ndn, output_dir, args):
    processes = []
    packet_capture_commands = {}
    gate_stop = threading.Event()
    gate_thread = None
    nodes = all_app_nodes(ndn)
    session_base = int(time.time()) + os.getpid()
    def provider_argv(index, node_name):
        provider_id = chr(ord("A") + index)
        fault_provider = str(getattr(
            args, "ndnsf_response_fault_provider", "")).upper()
        processing_delay_ms = args.processing_delay_ms
        ack_delay_ms = 0
        if fault_provider:
            if provider_id == fault_provider:
                processing_delay_ms = int(
                    getattr(args, "ndnsf_response_fault_delay_ms", 0))
            else:
                ack_delay_ms = int(
                    getattr(args, "ndnsf_standby_ack_delay_ms", 0))
        return [
            "--provider-id", provider_id,
            "--failure-probability", "0",
            "--availability-file", str(
                output_dir / "availability" / f"{node_name}.state"),
            "--processing-delay-ms", str(processing_delay_ms),
            "--ack-delay-ms", str(ack_delay_ms),
            "--handler-threads", str(max(
                1, int(getattr(args, "service_workers", 4)))),
        ]
    try:
        configure_ndn_multicast(nodes)
        if os.environ.get("NDNSF_MOBILITY_CAPTURE_PCAP") == "1":
            for node_name in ("memphis", "uiuc"):
                pcap_path = output_dir / f"{node_name}-ndn-udp.pcap"
                capture_command = (
                    "exec /usr/sbin/tcpdump -Z root -U -n -i any "
                    f"-w {perf.shell_quote(pcap_path)} 'udp port 6363'")
                capture, lf, lp = start_process(
                    ndn.net[node_name], f"packet-capture-{node_name}",
                    capture_command, output_dir)
                processes.append((capture, lf, lp))
                packet_capture_commands[node_name] = capture_command
            time.sleep(0.5)
        env = app_env(output_dir, session_base)
        controller_args = []
        if ACTIVE_PROFILE in {"four-provider-single-ap", "four-provider-multi-ap"}:
            controller_args = ["--policy-file", str(write_mobility_policy(output_dir))]
        controller_command = perf.managed_cmd(APP_CONTROLLER, controller_args)
        controller, lf, lp = start_process(
            ndn.net["memphis"], "ndnsf-controller",
            controller_command, output_dir, env)
        processes.append((controller, lf, lp))
        if not wait_for_log(lp, r"ServiceController listening on:", 20, controller):
            raise RuntimeError(f"controller not ready; see {lp}")
        time.sleep(3)
        for index, node_name in enumerate(provider_nodes()):
            provider_id = chr(ord("A") + index)
            provider_command = perf.managed_cmd(
                APP_PROVIDER, provider_argv(index, node_name))
            proc, lf, lp = start_process(
                ndn.net[node_name], f"ndnsf-provider-{provider_id}",
                provider_command, output_dir, env)
            processes.append((proc, lf, lp))
            if not wait_for_log(lp, r"INTERMITTENT_PROVIDER_READY", 30, proc):
                raise RuntimeError(f"provider {provider_id} not ready; see {lp}")
            time.sleep(1)
        time.sleep(args.settle_seconds)
        gate_epoch = time.monotonic() + 0.25
        gate_thread, trace_path, _ = start_coverage_gate(
            ndn, output_dir, args.ap_range, gate_stop, args.seed,
            block_network=bool(getattr(args, "block_network", False)),
            trace_replay_path=args.trace_replay or None,
            epoch_monotonic=gate_epoch,
            repair_ndn_faces=bool(getattr(args, "block_network", False)),
            mobility_warmup_s=args.mobility_warmup_s)
        log(f"RandomWaypoint coverage trace -> {trace_path}")
        client_launch_offset_s = wait_for_traffic_phase(gate_epoch, 0.0)
        measurement_start_target = gate_epoch + args.traffic_start_delay_s
        user_argv = [
            "--rate-rps", str(args.rate_rps),
            "--duration-ms", str(args.duration_s * 1000),
            "--ack-timeout-ms", str(args.ack_timeout_ms),
            "--timeout-ms", str(args.timeout_ms),
            "--strategy", args.ndnsf_strategy,
            "--startup-delay-ms", "0",
            "--measurement-start-monotonic-ms",
            str(int(math.ceil(measurement_start_target * 1000.0))),
        ]
        if getattr(args, "ndnsf_response_retry", False):
            user_argv.extend([
                "--response-retry",
                "--response-attempt-timeout-ms", str(args.attempt_timeout_ms),
                "--response-max-attempts", str(len(provider_nodes())),
            ])
        user_command = perf.managed_cmd(APP_USER, user_argv)
        runtime_commands_path = output_dir / "runtime-commands.json"
        write_json(runtime_commands_path, {
            "controller": controller_command,
            "providers": {
                node_name: perf.managed_cmd(
                    APP_PROVIDER, provider_argv(index, node_name))
                for index, node_name in enumerate(provider_nodes())
            },
            "user": user_command,
            "packet_capture": packet_capture_commands,
            "reconnect_face_repair": bool(getattr(args, "block_network", False)),
            "measurement_start_target_monotonic_s": measurement_start_target,
            "traffic_start_delay_s": args.traffic_start_delay_s,
        })
        user, lf, user_log = start_process(
            ndn.net["memphis"], "ndnsf-user",
            user_command, output_dir, env)
        processes.append((user, lf, user_log))
        user.wait(timeout=args.duration_s + args.timeout_ms / 1000.0 + 40)
        text = user_log.read_text(errors="replace")
        ready_matches = re.findall(r"INTERMITTENT_USER_READY[^\n\r]*", text)
        if not ready_matches:
            raise RuntimeError(f"NDNSF user readiness marker missing; see {user_log}")
        if "adaptiveAdmission=disabled" not in ready_matches[-1]:
            raise RuntimeError(
                "NDNSF mobility user did not report adaptive admission disabled; "
                f"see {user_log}")
        if (getattr(args, "ndnsf_response_retry", False) and
                "responseRetry=enabled" not in ready_matches[-1]):
            raise RuntimeError(
                "NDNSF mobility user did not report Response retry enabled; "
                f"see {user_log}")
        matches = re.findall(r"INTERMITTENT_USER_SUMMARY[^\n\r]*", text)
        if not matches:
            raise RuntimeError(f"NDNSF user summary missing; see {user_log}")
        line = matches[-1]
        vals = dict(re.findall(r"([A-Za-z0-9_]+)=([^ ]+)", line))
        measurement_lateness_ms = float(vals["measurement_start_lateness_ms"])
        traffic_offset_s = (
            args.traffic_start_delay_s + measurement_lateness_ms / 1000.0)
        validate_traffic_phase(traffic_offset_s, args)
        measurement_start_monotonic_s = (
            float(vals["measurement_start_monotonic_ms"]) / 1000.0)
        summary = {
            "system_id": "ndnsf",
            "system_label": "NDNSF",
            "sent": int(float(vals["sent"])),
            "accepted": int(float(vals["accepted"])),
            "completed": int(float(vals["completed"])),
            "success": int(float(vals["success"])),
            "timeout": int(float(vals["timeout"])),
            "bad_response": int(float(vals["bad_response"])),
            "deadline_failures": int(float(vals["timeout"])),
            "success_rate": float(vals["success_rate"]),
            "actual_rps": float(vals["actual_rps"]),
            "mean_ms": float(vals.get("mean_ms", 0.0)),
            "p50_ms": float(vals.get("p50_ms", 0.0)),
            "p95_ms": float(vals.get("p95_ms", 0.0)),
            "p99_ms": float(vals.get("p99_ms", 0.0)),
            "ndnsf_strategy": args.ndnsf_strategy,
            "ndnsf_response_retry": bool(
                getattr(args, "ndnsf_response_retry", False)),
            "response_attempt_timeout_ms": args.attempt_timeout_ms,
            "response_max_attempts": len(provider_nodes()),
            "response_attempts_started": len(re.findall(
                r"event=RESPONSE_ATTEMPT_STARTED\b", text)),
            "response_reselections": len(re.findall(
                r"event=RESPONSE_RESELECTION\b", text)),
            "response_fault_provider": str(getattr(
                args, "ndnsf_response_fault_provider", "")).upper(),
            "response_fault_delay_ms": int(getattr(
                args, "ndnsf_response_fault_delay_ms", 0)),
            "standby_ack_delay_ms": int(getattr(
                args, "ndnsf_standby_ack_delay_ms", 0)),
            "admission_control": "disabled",
            "reconnect_face_repair": bool(getattr(args, "block_network", False)),
            "client_launch_offset_s": client_launch_offset_s,
            "traffic_launch_offset_s": traffic_offset_s,
            "measurement_start_monotonic_s": measurement_start_monotonic_s,
            "measurement_start_target_monotonic_s": measurement_start_target,
            "measurement_start_lateness_ms": measurement_lateness_ms,
            "trace_source": str(Path(args.trace_replay).resolve())
                if args.trace_replay else str(trace_path.resolve()),
            "runtime_commands_file": str(runtime_commands_path),
            "runtime_commands_sha256": sha256_file(runtime_commands_path),
            **summarize_ndnsf_provider_executions(output_dir),
        }
        if args.smoke:
            if summary["sent"] <= 0 or summary["success"] != summary["sent"]:
                raise RuntimeError(
                    f"NDNSF smoke acceptance failed; see {user_log}")
            summary["smoke_ok"] = True
        return summary
    finally:
        gate_stop.set()
        if gate_thread is not None:
            gate_thread.join(timeout=3)
        stop_processes(processes)


def extract_marker_values(text, markers):
    for marker in markers:
        lines = [
            line for line in text.splitlines()
            if line.startswith(marker + " ")
        ]
        if lines:
            if len(markers) == 1 and len(lines) != 1:
                raise RuntimeError(
                    f"expected exactly one {marker} marker, found {len(lines)}")
            if len(markers) == 1:
                values = {}
                for token in lines[0][len(marker) + 1:].split():
                    match = re.fullmatch(r"([A-Za-z0-9_]+)=([^\s=]+)", token)
                    if match is None:
                        raise RuntimeError(
                            f"malformed token in {marker}: {token!r}")
                    key, value = match.groups()
                    if key in values:
                        raise RuntimeError(f"duplicate field in {marker}: {key}")
                    values[key] = value
                return marker, values
            return marker, dict(re.findall(r"([A-Za-z0-9_]+)=([^ ]+)", lines[-1]))
    raise RuntimeError(f"none of the summary markers were found: {', '.join(markers)}")


def numeric(values, key, default=0.0):
    try:
        return float(values[key])
    except (KeyError, TypeError, ValueError):
        return float(default)


def wait_for_traffic_phase(epoch_monotonic, delay_s):
    remaining = epoch_monotonic + delay_s - time.monotonic()
    if remaining > 0:
        time.sleep(remaining)
    return time.monotonic() - epoch_monotonic


def validate_traffic_phase(actual_offset_s, args):
    error_s = actual_offset_s - args.traffic_start_delay_s
    if args.formal_cell and abs(error_s) > TRAFFIC_PHASE_TOLERANCE_S:
        raise RuntimeError(
            f"traffic phase missed: expected +{args.traffic_start_delay_s:.3f}s, "
            f"actual +{actual_offset_s:.3f}s")


def stop_gate_and_resume(gate_stop, gate_thread, processes):
    gate_stop.set()
    if gate_thread is not None:
        gate_thread.join(timeout=3)
        if gate_thread.is_alive():
            raise RuntimeError("mobility gate did not stop within three seconds")
    for process in processes.values():
        if process.poll() is None:
            process.send_signal(signal.SIGCONT)


def summarize_ndnsf_provider_executions(output_dir):
    """Extract Provider-side execution attempts from the NDNSF cell logs."""
    counts = {}
    request_ids = set()
    marker = re.compile(
        r"INTERMITTENT_PROVIDER_REQUEST_EXECUTION\s+"
        r"provider=(?P<provider>[^ ]+)\s+request=(?P<request>[^ ]+)")
    for log_path in sorted(Path(output_dir).glob("ndnsf-provider-*.log")):
        for line in log_path.read_text(errors="replace").splitlines():
            match = marker.search(line)
            if match is None:
                continue
            provider = match.group("provider")
            request_id = match.group("request")
            counts[provider] = counts.get(provider, 0) + 1
            if request_id:
                request_ids.add(request_id)
    # A zero-marker result is valid when every provider is unavailable for the
    # complete trace: the user can observe accepted requests and timeouts while
    # no Provider reaches the request handler.  Keep that distinction explicit
    # instead of turning an informative all-unavailable cell into a harness
    # failure.  Formal runs with any successful response still require the
    # instrumentation marker via the accounting validation below.
    return {
        "attempts": sum(counts.values()),
        "provider_executions": sum(counts.values()),
        "provider_execution_counts": dict(sorted(counts.items())),
        "unique_request_ids_executed": len(request_ids),
        "provider_execution_markers_present": bool(counts),
    }


def run_grpc(ndn, output_dir, args):
    processes = []
    gate_stop = threading.Event()
    gate_thread = None
    server_commands = {}
    try:
        selected_providers = configured_provider_nodes(args)
        servers = {}
        for node_name in provider_nodes():
            service_workers = max(1, int(getattr(args, "service_workers", 4)))
            server_command = (
                f"cd {perf.shell_quote(REPO_ROOT)} && exec python3 "
                f"{perf.shell_quote(GRPC_DIR / 'greeter_server.py')} "
                f"--bind 0.0.0.0:50051 --provider-id {node_name} "
                f"--delay-ms {args.processing_delay_ms} "
                f"--workers {service_workers} --quiet")
            server, lf, lp = start_process(
                ndn.net[node_name], f"grpc-server-{node_name}",
                server_command, output_dir)
            processes.append((server, lf, lp))
            servers[node_name] = server
            server_commands[node_name] = server_command
            if not wait_for_log(lp, r"GRPC_SERVER_READY", 10, server):
                raise RuntimeError(f"gRPC server {node_name} not ready; see {lp}")
        gate_epoch = time.monotonic() + 0.25
        gate_thread, trace_path, _ = start_coverage_gate(
            ndn, output_dir, args.ap_range, gate_stop, args.seed,
            block_network=bool(getattr(args, "block_network", False)),
            pause_processes=(None if getattr(args, "block_network", False)
                             else servers),
            trace_replay_path=args.trace_replay or None,
            epoch_monotonic=gate_epoch,
            mobility_warmup_s=args.mobility_warmup_s)
        log(f"Mobility coverage trace -> {trace_path}")
        client_launch_offset_s = wait_for_traffic_phase(gate_epoch, 0.0)
        measurement_start_target = gate_epoch + args.traffic_start_delay_s
        workload_targets = " ".join(
            f"--target {name}={PROVIDER_ENDPOINTS[name]}"
            for name in selected_providers)
        stats_targets = " ".join(
            f"--target {name}={PROVIDER_ENDPOINTS[name]}" for name in provider_nodes())
        smoke_flag = " --smoke" if args.smoke else ""
        client_command = (
            f"cd {perf.shell_quote(REPO_ROOT)} && exec python3 "
            f"{perf.shell_quote(GRPC_FAILOVER_CLIENT)} {workload_targets} "
            f"--rate-rps {args.rate_rps} --duration-s {args.duration_s} "
            f"--global-deadline-s {args.timeout_ms / 1000.0} "
            f"--attempt-timeout-s {args.attempt_timeout_ms / 1000.0} "
            f"--health-interval-s {args.health_interval_ms / 1000.0} "
            f"--health-timeout-s {args.attempt_timeout_ms / 1000.0} "
            f"--health-stale-s {2.0 * args.health_interval_ms / 1000.0} "
            f"--measurement-start-monotonic-s {measurement_start_target:.9f} "
            f"--measurement-start-tolerance-s {TRAFFIC_PHASE_TOLERANCE_S} "
            f"--warmup-s 0 --quiet{smoke_flag}")
        if len(selected_providers) == 1:
            client_command += " --single-provider"
        if (not args.grpc_no_health_routing and
                args.trace_profile != "single-active-handoff"):
            client_command += " --require-all-prewarmed"
        elif args.grpc_no_health_routing:
            client_command += " --disable-health-routing"
        if getattr(args, "grpc_parallel", False):
            client_command += " --parallel --disable-health-routing"
        runtime_commands_path = output_dir / "runtime-commands.json"
        runtime_commands = {
            "servers": server_commands,
            "client": client_command,
            "client_environment": {},
        }
        write_json(runtime_commands_path, runtime_commands)
        client, lf, client_log = start_process(
            ndn.net["memphis"], "grpc-client",
            client_command, output_dir)
        processes.append((client, lf, client_log))
        client.wait(timeout=args.duration_s + args.timeout_ms / 1000.0 + 30)
        text = client_log.read_text(errors="replace")
        markers = (("GRPC_FAILOVER_RATE",) if args.formal_cell or args.smoke else
                   ("GRPC_FAILOVER_RATE", "GRPC_CLIENT_RATE"))
        marker, vals = extract_marker_values(text, markers)
        validate_grpc_values(marker, vals, args)
        sent = int(numeric(vals, "sent"))
        success = int(numeric(vals, "success"))
        failures = int(numeric(vals, "failures"))
        measurement_lateness_ms = numeric(vals, "measurement_start_lateness_ms")
        traffic_offset_s = args.traffic_start_delay_s + measurement_lateness_ms / 1000.0
        validate_traffic_phase(traffic_offset_s, args)

        # Stop replay and resume every server before querying the server-owned
        # execution ledger. Stats are captured while the exact workload server
        # processes are still alive, then persisted independently of client logs.
        stop_gate_and_resume(gate_stop, gate_thread, servers)
        gate_thread = None
        time.sleep(0.2)
        stats_command = (
            f"cd {perf.shell_quote(REPO_ROOT)} && exec python3 "
            f"{perf.shell_quote(GRPC_FAILOVER_CLIENT)} {stats_targets} "
            "--stats-only --stats-timeout-s 2 --quiet")
        runtime_commands["stats_client"] = stats_command
        write_json(runtime_commands_path, runtime_commands)
        stats_process, stats_file, stats_log = start_process(
            ndn.net["memphis"], "grpc-stats", stats_command, output_dir)
        processes.append((stats_process, stats_file, stats_log))
        stats_process.wait(timeout=10)
        if stats_process.returncode != 0:
            raise RuntimeError(f"gRPC Stats collection failed; see {stats_log}")
        stats_text = stats_log.read_text(errors="replace")
        stats_snapshot = parse_json_marker(
            stats_text, "GRPC_STATS_SNAPSHOT_JSON ")
        stats_path = output_dir / "grpc-server-stats.json"
        write_json(stats_path, stats_snapshot)
        for provider_id, provider_snapshot in stats_snapshot["providers"].items():
            write_json(
                output_dir / f"grpc-provider-stats-{provider_id}.json",
                provider_snapshot)
        actual_measurement_start_s = (
            measurement_start_target + measurement_lateness_ms / 1000.0)
        exact_stats = summarize_grpc_server_stats(
            stats_snapshot, sent, actual_measurement_start_s, args.duration_s)
        summary = {
            "system_id": "grpc",
            "system_label": (
                f"gRPC-PAR-{len(selected_providers)}"
                if getattr(args, "grpc_parallel", False)
                else (
                    f"gRPC-SEQ-{len(selected_providers)}"
                    if args.grpc_no_health_routing
                    else f"gRPC-HC-{len(selected_providers)}")),
            "summary_marker": marker,
            "sent": sent,
            "success": success,
            "failures": failures,
            "timeout": failures,
            "success_rate": 100.0 * success / sent if sent else 0.0,
            "actual_rps": numeric(vals, "actual_success_rps", success / float(args.duration_s)),
            "attempts": int(numeric(vals, "attempts", sent)),
            "failovers": int(numeric(vals, "failovers")),
            "health_checks": int(numeric(vals, "health_checks")),
            "health_success": int(numeric(vals, "health_success")),
            "client_window_health_checks": int(numeric(vals, "health_checks")),
            "client_window_health_success": int(numeric(vals, "health_success")),
            "health_directed_selections": int(numeric(
                vals, "health_directed_selections")),
            "health_routing": vals.get(
                "health_routing", "disabled" if args.grpc_no_health_routing else "enabled"),
            "execution_mode": vals.get(
                "execution_mode",
                "parallel-first-success" if getattr(args, "grpc_parallel", False)
                else "sequential-failover"),
            "provider_scope": list(selected_providers),
            "parallel_issued": int(numeric(vals, "parallel_issued", 0)),
            "parallel_winners": int(numeric(vals, "parallel_winners", 0)),
            "parallel_cancellations": int(
                numeric(vals, "parallel_cancellations", 0)),
            "handler_executions_observed": int(numeric(
                vals, "handler_executions_observed",
                numeric(vals, "handler_executions"))),
            "application_messages": int(numeric(vals, "application_messages")),
            "application_rpc_calls": int(numeric(vals, "application_rpc_calls")),
            "message_definition": vals.get("message_definition", ""),
            "pre_measurement_health_checks": int(numeric(
                vals, "pre_measurement_health_checks")),
            "pre_measurement_health_success": int(numeric(
                vals, "pre_measurement_health_success")),
            "status_ok": int(numeric(vals, "status_ok")),
            "status_unavailable": int(numeric(vals, "status_unavailable")),
            "status_deadline_exceeded": int(numeric(
                vals, "status_deadline_exceeded")),
            "status_health_directed_selection": int(numeric(
                vals, "status_health_directed_selection")),
            "p50_ms": numeric(vals, "p50_ms"),
            "p95_ms": numeric(vals, "p95_ms"),
            "p99_ms": numeric(vals, "p99_ms"),
            "mean_ms": numeric(vals, "mean_ms"),
            "client_launch_offset_s": client_launch_offset_s,
            "traffic_launch_offset_s": traffic_offset_s,
            "measurement_start_monotonic_s": actual_measurement_start_s,
            "measurement_start_lateness_ms": measurement_lateness_ms,
            "trace_source": str(Path(args.trace_replay).resolve()) if args.trace_replay else None,
            "runtime_commands_file": str(runtime_commands_path),
            "runtime_commands_sha256": sha256_file(runtime_commands_path),
            "server_stats_file": str(stats_path),
            "server_stats_sha256": sha256_file(stats_path),
            **exact_stats,
        }
        if args.smoke:
            failover_or_fanout = (
                summary["parallel_issued"] >= sent
                if getattr(args, "grpc_parallel", False)
                else summary["failovers"] >= 1)
            if ("SMOKE_OK" not in text or sent <= 0 or success != sent or
                    not failover_or_fanout):
                raise RuntimeError(f"gRPC smoke acceptance failed; see {client_log}")
            summary["smoke_ok"] = True
        return summary
    finally:
        gate_stop.set()
        if gate_thread is not None:
            gate_thread.join(timeout=3)
        stop_processes(processes)


def run_nsc(ndn, output_dir, args):
    processes = []
    gate_stop = threading.Event()
    gate_thread = None
    nodes = all_app_nodes(ndn)
    producer_commands = {}
    try:
        selected_providers = configured_provider_nodes(args)
        configure_ndn_multicast(nodes)
        for node in nodes:
            node.cmd(f"ndnsec key-gen -t r /muas/{node.name} >/dev/null 2>&1 || true")
        producers = {}
        for node_name in provider_nodes():
            producer_command = (
                f"cd {perf.shell_quote(REPO_ROOT)} && exec "
                f"{perf.shell_quote(NSC_DIR / 'producer')} "
                f"{PROVIDER_PREFIXES[node_name]} /FlightControl /ManualControl "
                f"{args.processing_delay_ms}")
            producer, lf, lp = start_process(
                ndn.net[node_name], f"nsc-producer-{node_name}",
                producer_command, output_dir)
            processes.append((producer, lf, lp))
            producers[node_name] = producer
            producer_commands[node_name] = producer_command
            if not wait_for_log(lp, r"REGISTER PREFIX", 10, producer):
                raise RuntimeError(f"NSC producer {node_name} not ready; see {lp}")
        gate_epoch = time.monotonic() + 0.25
        gate_thread, trace_path, _ = start_coverage_gate(
            ndn, output_dir, args.ap_range, gate_stop, args.seed,
            block_network=bool(getattr(args, "block_network", False)),
            pause_processes=(None if getattr(args, "block_network", False)
                             else producers),
            trace_replay_path=args.trace_replay or None,
            epoch_monotonic=gate_epoch,
            repair_ndn_faces=bool(getattr(args, "block_network", False)),
            mobility_warmup_s=args.mobility_warmup_s)
        log(f"Mobility coverage trace -> {trace_path}")
        client_launch_offset_s = wait_for_traffic_phase(gate_epoch, 0.0)
        measurement_start_target_ms = int(math.ceil(
            (gate_epoch + args.traffic_start_delay_s) * 1000.0))
        count = int(args.duration_s * args.rate_rps)
        interval_ms = max(1, int(round(1000.0 / args.rate_rps)))
        run_id = f"wifi-{args.seed}-{range_label(args.ap_range)}-{uuid.uuid4().hex[:8]}"
        prefixes = ",".join(PROVIDER_PREFIXES[name] for name in selected_providers)
        client_env = {"NSC_SMOKE_TEST": "1"} if args.smoke else None
        consumer_command = (
            f"cd {perf.shell_quote(REPO_ROOT)} && exec {perf.shell_quote(NSC_DIR / 'consumer')} "
            f"/muas/memphis {prefixes} /FlightControl /ManualControl "
            f"{interval_ms} {count} {run_id} 0 {args.timeout_ms} "
            f"{args.attempt_timeout_ms} {measurement_start_target_ms} "
            f"{int(round(TRAFFIC_PHASE_TOLERANCE_S * 1000.0))}")
        runtime_commands_path = output_dir / "runtime-commands.json"
        write_json(runtime_commands_path, {
            "servers": producer_commands,
            "client": consumer_command,
            "client_environment": client_env or {},
            "reconnect_face_repair": bool(getattr(args, "block_network", False)),
        })
        consumer, lf, client_log = start_process(
            ndn.net["memphis"], "nsc-consumer",
            consumer_command,
            output_dir, client_env)
        processes.append((consumer, lf, client_log))
        consumer.wait(timeout=args.duration_s + args.timeout_ms / 1000.0 + 30)
        text = client_log.read_text(errors="replace")
        markers = (("NSC_FAILOVER_SUMMARY",) if args.formal_cell or args.smoke else
                   ("NSC_FAILOVER_SUMMARY", "NSC_CLIENT_SUMMARY"))
        marker, vals = extract_marker_values(text, markers)
        validate_nsc_values(marker, vals, args)
        sent = int(numeric(vals, "count"))
        success = int(numeric(vals, "success"))
        timeout = int(numeric(vals, "terminal_failures", numeric(vals, "timeout")))
        measurement_lateness_ms = numeric(
            vals, "first_request_start_lateness_ms",
            numeric(vals, "measurement_start_lateness_ms"))
        target_offset_s = measurement_start_target_ms / 1000.0 - gate_epoch
        traffic_offset_s = target_offset_s + measurement_lateness_ms / 1000.0
        validate_traffic_phase(traffic_offset_s, args)
        summary = {
            "system_id": "nsc",
            "system_label": f"NSC-{len(selected_providers)}",
            "summary_marker": marker,
            "sent": sent,
            "success": success,
            "terminal_failures": timeout,
            "timeout": timeout,
            "success_rate": numeric(
                vals, "success_rate", 100.0 * success / sent if sent else 0.0),
            "actual_rps": success / float(args.duration_s),
            "attempts": int(numeric(vals, "attempts", sent)),
            "attempt_timeouts": int(numeric(vals, "attempt_timeouts", timeout)),
            "nacks": int(numeric(vals, "nacks")),
            "failovers": int(numeric(vals, "failovers")),
            "late_callbacks": int(numeric(vals, "late_callbacks")),
            "p50_ms": numeric(vals, "p50_ms"),
            "p95_ms": numeric(vals, "p95_ms"),
            "p99_ms": numeric(vals, "p99_ms"),
            "mean_ms": numeric(vals, "mean_ms"),
            "provider_attempts": vals.get("provider_attempts", ""),
            "provider_scope": list(selected_providers),
            "application_messages": int(numeric(vals, "application_messages")),
            "message_definition": vals.get("message_definition", ""),
            "notification_interests": int(numeric(vals, "notification_interests")),
            "notification_data": int(numeric(vals, "notification_data")),
            "input_interests": int(numeric(vals, "input_interests")),
            "input_data": int(numeric(vals, "input_data")),
            "result_interests": int(numeric(vals, "result_interests")),
            "result_data": int(numeric(vals, "result_data")),
            "client_launch_offset_s": client_launch_offset_s,
            "traffic_launch_offset_s": traffic_offset_s,
            "measurement_start_lateness_ms": measurement_lateness_ms,
            "trace_source": str(Path(args.trace_replay).resolve()) if args.trace_replay else None,
            "runtime_commands_file": str(runtime_commands_path),
            "runtime_commands_sha256": sha256_file(runtime_commands_path),
            "reconnect_face_repair": bool(getattr(args, "block_network", False)),
        }
        if args.smoke:
            attempt_parts = {
                key: int(value)
                for key, value in (
                    item.rsplit(":", 1)
                    for item in vals.get("provider_attempts", "").split(",")
                    if ":" in item)
            }
            all_providers_exercised = all(
                attempt_parts.get(PROVIDER_PREFIXES[name], 0) > 0
                for name in provider_nodes())
            if ("SMOKE_OK" not in text or sent <= 0 or success != sent or
                    summary["failovers"] < 1 or not all_providers_exercised):
                raise RuntimeError(f"NSC smoke acceptance failed; see {client_log}")
            summary["smoke_ok"] = True
        return summary
    finally:
        gate_stop.set()
        if gate_thread is not None:
            gate_thread.join(timeout=3)
        stop_processes(processes)


def run_one(system, ap_range, args, output_dir):
    args.ap_range = ap_range
    ndn = build_wifi_topology(ap_range, args.seed)
    try:
        if system == "ndnsf":
            # PIB/SafeBag setup must finish before NFD opens its database.
            initialize_keychains(
                all_app_nodes(ndn), ndn.net["memphis"], output_dir)
        log("Starting NFD")
        AppManager(ndn, all_app_nodes(ndn), Nfd, logLevel="ERROR")
        time.sleep(2)
        if system == "ndnsf":
            return run_ndnsf(ndn, output_dir, args)
        if base_system_id(system) == "grpc":
            return run_grpc(ndn, output_dir, args)
        if base_system_id(system) == "nsc":
            return run_nsc(ndn, output_dir, args)
        raise RuntimeError(f"unknown system {system}")
    finally:
        ndn.net.stop()
        Minindn.cleanUp()
        subprocess.run(["mn", "-c"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=False)
        subprocess.run(["modprobe", "-r", "mac80211_hwsim"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       check=False)
        time.sleep(3)


def campaign_artifacts(systems):
    paths = [
        ("source", Path(__file__).resolve()),
    ]
    if any(base_system_id(system) == "grpc" for system in systems):
        paths.extend([
            ("source", GRPC_DIR / "greeter_server.py"),
            ("source", GRPC_FAILOVER_CLIENT),
            ("protocol", GRPC_DIR / "helloworld.proto"),
            ("generated", GRPC_DIR / "helloworld_pb2.py"),
            ("generated", GRPC_DIR / "helloworld_pb2_grpc.py"),
        ])
    if any(base_system_id(system) == "nsc" for system in systems):
        paths.extend([
            ("source", NSC_DIR / "producer.cpp"),
            ("source", NSC_DIR / "consumer.cpp"),
            ("binary", NSC_DIR / "producer"),
            ("binary", NSC_DIR / "consumer"),
        ])
    if "ndnsf" in systems:
        paths.extend([
            ("binary", APP_CONTROLLER),
            ("binary", APP_PROVIDER),
            ("binary", APP_USER),
            ("binary", MOBILITY_BUILD_DIR / "libndn-service-framework.so"),
        ])
    return paths


def validate_preflight(systems):
    errors = []
    artifacts = campaign_artifacts(systems)
    for kind, path in artifacts:
        if not path.is_file():
            errors.append(f"missing {kind}: {path}")
            continue
        if kind == "binary" and not os.access(path, os.X_OK):
            errors.append(f"not executable: {path}")
        if path.suffix == ".py":
            try:
                compile(path.read_text(), str(path), "exec")
            except SyntaxError as error:
                errors.append(f"Python syntax error in {path}: {error}")
    if errors:
        raise RuntimeError("preflight failed:\n  " + "\n  ".join(errors))
    return [
        {
            "kind": kind,
            "path": str(path.resolve()),
            "sha256": sha256_file(path),
        }
        for kind, path in artifacts
    ]


def runtime_provenance(systems):
    packages = {}
    for package in ("grpcio", "protobuf"):
        try:
            packages[package] = importlib.metadata.version(package)
        except importlib.metadata.PackageNotFoundError:
            packages[package] = None
    ndn_cxx = subprocess.run(
        ["pkg-config", "--modversion", "libndn-cxx"],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    git_head = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=REPO_ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    git_tree = subprocess.run(
        ["git", "rev-parse", "HEAD^{tree}"], cwd=REPO_ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    tracked_paths = []
    for _, path in campaign_artifacts(systems):
        try:
            tracked_paths.append(str(path.resolve().relative_to(REPO_ROOT)))
        except ValueError:
            continue
    git_diff = subprocess.run(
        ["git", "diff", "--binary", "--no-ext-diff", "HEAD", "--", *tracked_paths],
        cwd=REPO_ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    git_status = subprocess.run(
        ["git", "status", "--porcelain=v1", "--", *tracked_paths],
        cwd=REPO_ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    svs_runtime_dir = os.environ.get("NDNSF_MOBILITY_RUNTIME_LIB_DIR", "")
    svs_runtime_path = (
        Path(svs_runtime_dir).expanduser().resolve() / "libndn-svs.so.0.1.0"
        if svs_runtime_dir else None)
    framework_path = MOBILITY_BUILD_DIR / "libndn-service-framework.so"
    return {
        "python_executable": sys.executable,
        "python_version": platform.python_version(),
        "platform": platform.platform(),
        "packages": packages,
        "libndn_cxx_version": (
            ndn_cxx.stdout.strip() if ndn_cxx.returncode == 0 else None),
        "git_head": git_head.stdout.strip() if git_head.returncode == 0 else None,
        "git_tree": git_tree.stdout.strip() if git_tree.returncode == 0 else None,
        "tracked_diff_sha256": hashlib.sha256(git_diff.stdout).hexdigest(),
        "campaign_paths_status_sha256": hashlib.sha256(git_status.stdout).hexdigest(),
        "ndnsf_runtime": {
            "build_dir": str(MOBILITY_BUILD_DIR),
            "framework_library": str(framework_path),
            "framework_library_sha256": (
                sha256_file(framework_path) if framework_path.is_file() else None),
            "svs_library": str(svs_runtime_path) if svs_runtime_path else None,
            "svs_library_sha256": (
                sha256_file(svs_runtime_path)
                if svs_runtime_path and svs_runtime_path.is_file() else None),
            "runtime_library_dir": str(svs_runtime_dir) if svs_runtime_dir else None,
        },
    }


def ndnsf_nonexecution_evidence():
    dependencies = (
        initialize_keychains, configure_ndn_multicast, start_coverage_gate,
        app_env, perf.managed_cmd,
    )
    return {
        "executed_in_baseline_campaign": False,
        "scope": "run_ndnsf body and directly referenced orchestration helpers",
        "run_ndnsf_source_sha256": sha256_text(inspect.getsource(run_ndnsf)),
        "dependency_source_sha256": {
            function.__name__: sha256_text(inspect.getsource(function))
            for function in dependencies
        },
        "app_binary_sha256": {
            str(path.resolve()): sha256_file(path)
            for path in (APP_CONTROLLER, APP_PROVIDER, APP_USER)
            if path.is_file()
        },
    }


def prepare_output_dir(requested, prefix="wifi_router_mobility_baselines"):
    if requested:
        output_dir = Path(requested).resolve()
    else:
        timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        output_dir = (REPO_ROOT / "results" /
                      f"{prefix}_{timestamp}_{os.getpid()}_{uuid.uuid4().hex[:8]}").resolve()
    if output_dir.exists() and any(output_dir.iterdir()):
        raise RuntimeError(
            f"output directory is not empty; choose a unique directory: {output_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)
    return output_dir


def validate_formal_configuration(args, ranges, systems):
    problems = []
    if tuple(systems) != FORMAL_SYSTEMS:
        problems.append(f"systems must be {','.join(FORMAL_SYSTEMS)}")
    if tuple(float(value) for value in ranges) != FORMAL_RANGES:
        problems.append("ranges must be 100,150,200")
    fixed = (
        (args.duration_s, 60, "duration-s"),
        (args.rate_rps, 5.0, "rate-rps"),
        (args.processing_delay_ms, 5, "processing-delay-ms"),
        (args.timeout_ms, 5000, "timeout-ms"),
        (args.attempt_timeout_ms, 200, "attempt-timeout-ms"),
        (args.health_interval_ms, 200, "health-interval-ms"),
        (args.traffic_start_delay_s, 2.0, "traffic-start-delay-s"),
        (args.seed, 20, "seed"),
    )
    for actual, expected, name in fixed:
        if actual != expected:
            problems.append(f"{name} must be {expected}, got {actual}")
    if problems:
        raise RuntimeError("invalid formal baseline configuration:\n  " + "\n  ".join(problems))


def child_command(script, system, ap_range, args, run_dir, trace_path, campaign_id):
    command = [
        sys.executable, str(script),
        "--single-run",
        "--ranges", str(ap_range),
        "--systems", system,
        "--duration-s", str(args.duration_s),
        "--rate-rps", str(args.rate_rps),
        "--processing-delay-ms", str(args.processing_delay_ms),
        "--timeout-ms", str(args.timeout_ms),
        "--ack-timeout-ms", str(args.ack_timeout_ms),
        "--attempt-timeout-ms", str(args.attempt_timeout_ms),
        "--health-interval-ms", str(args.health_interval_ms),
        "--traffic-start-delay-s", str(args.traffic_start_delay_s),
        "--settle-seconds", str(args.settle_seconds),
        "--trace-profile", args.trace_profile,
        "--handoff-period-s", str(args.handoff_period_s),
        "--mobility-warmup-s", str(args.mobility_warmup_s),
        "--ndnsf-strategy", args.ndnsf_strategy,
        "--seed", str(args.seed),
        "--campaign-id", campaign_id,
        "--output-dir", str(run_dir),
        "--profile", args.profile,
    ]
    if getattr(args, "ndnsf_response_retry", False):
        command.append("--ndnsf-response-retry")
    if getattr(args, "ndnsf_response_fault_provider", ""):
        command.extend((
            "--ndnsf-response-fault-provider",
            str(args.ndnsf_response_fault_provider),
            "--ndnsf-response-fault-delay-ms",
            str(args.ndnsf_response_fault_delay_ms),
            "--ndnsf-standby-ack-delay-ms",
            str(args.ndnsf_standby_ack_delay_ms),
        ))
    if args.provider_scope:
        command.extend(("--provider-scope", args.provider_scope))
    if args.ap_layout:
        command.extend(("--ap-layout", args.ap_layout))
    if args.speed_mps is not None:
        command.extend(("--speed-mps", str(args.speed_mps)))
    if base_system_id(system) == "grpc" and args.grpc_no_health_routing:
        command.append("--grpc-no-health-routing")
    if getattr(args, "grpc_parallel", False):
        command.append("--grpc-parallel")
    if getattr(args, "block_network", False):
        command.append("--block-network")
    if trace_path:
        command.extend(("--trace-replay", str(trace_path)))
    if args.formal_baselines:
        command.append("--formal-cell")
    if args.smoke:
        command.append("--smoke-cell")
    return command


def build_campaign_plan(args, output_dir, ranges, systems, source_hashes):
    campaign_id = f"mobility-{uuid.uuid4().hex}"
    script = Path(__file__).resolve()
    traces_dir = output_dir / "traces"
    traces = {}
    horizon_s = (args.traffic_start_delay_s + args.duration_s +
                 args.timeout_ms / 1000.0 + 2.0)
    trace_systems = (*FORMAL_SYSTEMS, "ndnsf")
    if any(base_system_id(system) in trace_systems for system in systems):
        trace_interval_s = 0.1 if (args.smoke or args.include_ndnsf) else TRACE_INTERVAL_S
        trace_profile = "forced-failover-smoke" if args.smoke else args.trace_profile
        for ap_range in ranges:
            trace_path = traces_dir / f"range_{range_label(ap_range)}.csv"
            traces[float(ap_range)] = generate_mobility_trace(
                trace_path, ap_range, args.seed, horizon_s,
                interval_s=trace_interval_s, profile=trace_profile,
                handoff_period_s=args.handoff_period_s,
                mobility_warmup_s=args.mobility_warmup_s,
                measurement_start_s=args.traffic_start_delay_s,
                measurement_duration_s=args.duration_s)

    cells = []
    for ap_range in ranges:
        for system in systems:
            cell_id = f"range-{range_label(ap_range)}-{system}"
            run_dir = output_dir / "cells" / cell_id
            trace_path = None
            if base_system_id(system) in trace_systems:
                trace_path = Path(traces[float(ap_range)]["path"])
            command = child_command(
                script, system, ap_range, args, run_dir, trace_path,
                campaign_id)
            cells.append({
                "cell_id": cell_id,
                "system": system,
                "system_id": system,
                "range_m": float(ap_range),
                "output_dir": str(run_dir),
                "trace_path": str(trace_path) if trace_path else None,
                "trace_sha256": traces[float(ap_range)]["sha256"] if trace_path else None,
                "argv": command,
                "command": shlex.join(command),
                "command_sha256": sha256_text(shlex.join(command)),
                "source_hashes": source_hashes,
            })

    manifest = {
        "schema": CAMPAIGN_SCHEMA,
        "campaign_id": campaign_id,
        "output_dir": str(output_dir.resolve()),
        "created_at": utc_now(),
        "formal_baselines": bool(args.formal_baselines),
        "smoke": bool(args.smoke),
        "no_ndnsf_formal_cells": bool(
            args.formal_baselines and all(cell["system"] != "ndnsf" for cell in cells)),
        "configuration": {
            "profile": ACTIVE_PROFILE,
            "ap_layout": ACTIVE_AP_LAYOUT,
            "ap_positions_m": [list(position) for position in active_ap_positions()],
            "speed_mps": list(ACTIVE_SPEED_MPS),
            "systems": systems,
            "ranges_m": ranges,
            "duration_s": args.duration_s,
            "rate_rps": args.rate_rps,
            "processing_delay_ms": args.processing_delay_ms,
            "service_workers": max(1, int(getattr(args, "service_workers", 4))),
            "global_deadline_ms": args.timeout_ms,
            "attempt_timeout_ms": args.attempt_timeout_ms,
            "health_interval_ms": args.health_interval_ms,
            "grpc_no_health_routing": bool(args.grpc_no_health_routing),
            "grpc_parallel": bool(getattr(args, "grpc_parallel", False)),
            "provider_scope": configured_provider_nodes(args),
            "block_network": bool(getattr(args, "block_network", False)),
            "trace_profile": args.trace_profile,
            "handoff_period_s": args.handoff_period_s,
            "mobility_warmup_s": args.mobility_warmup_s,
            "ndnsf_strategy": args.ndnsf_strategy,
            "ndnsf_response_retry": bool(
                getattr(args, "ndnsf_response_retry", False)),
            "ndnsf_response_fault_provider": str(getattr(
                args, "ndnsf_response_fault_provider", "")).upper(),
            "ndnsf_response_fault_delay_ms": int(getattr(
                args, "ndnsf_response_fault_delay_ms", 0)),
            "ndnsf_standby_ack_delay_ms": int(getattr(
                args, "ndnsf_standby_ack_delay_ms", 0)),
            "traffic_start_delay_s": args.traffic_start_delay_s,
            "traffic_phase_tolerance_s": TRAFFIC_PHASE_TOLERANCE_S,
            "seed": args.seed,
            "providers": [
                {
                    "node": name,
                    "grpc_endpoint": PROVIDER_ENDPOINTS[name],
                    "nsc_prefix": PROVIDER_PREFIXES[name],
                }
                for name in provider_nodes()
            ],
            "campaign_repetitions_per_cell": 1,
            "campaign_cell_retries": 0,
        },
        "source_hashes": source_hashes,
        "runtime_provenance": runtime_provenance(systems),
        "ndnsf_nonexecution_evidence": (
            ndnsf_nonexecution_evidence()
            if all(cell["system"] != "ndnsf" for cell in cells) else None),
        "traces": [traces[key] for key in sorted(traces)],
        "cells": cells,
    }
    validate_campaign_manifest(manifest)
    return manifest


def validate_campaign_manifest(manifest):
    if manifest.get("schema") != CAMPAIGN_SCHEMA:
        raise RuntimeError("campaign manifest schema mismatch")
    cells = manifest.get("cells")
    if not isinstance(cells, list) or not cells:
        raise RuntimeError("campaign manifest has no cells")
    campaign_output = Path(manifest.get("output_dir", "")).resolve()
    if not manifest.get("output_dir"):
        raise RuntimeError("campaign manifest has no output_dir")
    identities = [(cell.get("range_m"), cell.get("system")) for cell in cells]
    if len(identities) != len(set(identities)):
        raise RuntimeError("campaign contains duplicate range/system cells")
    if len({cell.get("cell_id") for cell in cells}) != len(cells):
        raise RuntimeError("campaign contains duplicate cell IDs")
    service_workers = manifest.get("configuration", {}).get("service_workers", 4)
    if not isinstance(service_workers, int) or service_workers <= 0:
        raise RuntimeError("campaign service_workers must be a positive integer")
    for cell in cells:
        if cell.get("system") not in set(SUPPORTED_SYSTEMS):
            raise RuntimeError(f"invalid canonical system ID: {cell.get('system')}")
        if cell.get("system_id") != cell.get("system"):
            raise RuntimeError(f"non-canonical cell system_id: {cell.get('cell_id')}")
        if not isinstance(cell.get("argv"), list) or shlex.join(cell["argv"]) != cell.get("command"):
            raise RuntimeError(f"argv/command mismatch for {cell.get('cell_id')}")
        if sha256_text(cell.get("command", "")) != cell.get("command_sha256"):
            raise RuntimeError(f"command hash mismatch for {cell.get('cell_id')}")
        expected_run_dir = campaign_output / "cells" / cell["cell_id"]
        if Path(cell.get("output_dir", "")).resolve() != expected_run_dir:
            raise RuntimeError(f"cell output path escapes campaign: {cell.get('cell_id')}")
        if cell.get("source_hashes") != manifest.get("source_hashes"):
            raise RuntimeError(f"cell source hashes differ from campaign: {cell.get('cell_id')}")
        trace_path = cell.get("trace_path")
        if trace_path:
            if not Path(trace_path).is_file():
                raise RuntimeError(f"trace missing for {cell.get('cell_id')}")
            if sha256_file(trace_path) != cell.get("trace_sha256"):
                raise RuntimeError(f"trace hash mismatch for {cell.get('cell_id')}")
    if manifest.get("formal_baselines"):
        expected = {
            (ap_range, system)
            for ap_range in FORMAL_RANGES for system in FORMAL_SYSTEMS
        }
        if len(cells) != 6 or set(identities) != expected:
            raise RuntimeError("formal campaign must contain the exact six unique cells")
        configuration = manifest.get("configuration", {})
        frozen = {
            "systems": list(FORMAL_SYSTEMS),
            "ranges_m": list(FORMAL_RANGES),
            "duration_s": 60,
            "rate_rps": 5.0,
            "processing_delay_ms": 5,
            "global_deadline_ms": 5000,
            "attempt_timeout_ms": 200,
            "health_interval_ms": 200,
            "traffic_start_delay_s": 2.0,
            "seed": 20,
        }
        for key, expected_value in frozen.items():
            if configuration.get(key) != expected_value:
                raise RuntimeError(
                    f"formal manifest changed {key}: {configuration.get(key)!r}")
        if (configuration.get("campaign_repetitions_per_cell") != 1 or
                configuration.get("campaign_cell_retries") != 0):
            raise RuntimeError("formal campaign must be one-shot with zero retries")
        if not manifest.get("no_ndnsf_formal_cells"):
            raise RuntimeError("formal baseline manifest must contain no NDNSF cells")
        by_range = {}
        for cell in cells:
            by_range.setdefault(cell["range_m"], set()).add(cell["trace_sha256"])
        if any(len(hashes) != 1 for hashes in by_range.values()):
            raise RuntimeError("each range must share one byte-identical trace")


def write_campaign_csvs(output_dir, runs, cells):
    run_fields = [
        "cell_id", "system_id", "system_label", "range_m", "status", "returncode",
        "started_at", "completed_at", "elapsed_s", "command", "command_sha256",
        "output_dir",
    ]
    with (output_dir / "campaign-runs.csv").open("w", newline="") as raw:
        writer = csv.DictWriter(raw, fieldnames=run_fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(runs)

    cell_fields = [
        "cell_id", "system_id", "system_label", "range_m", "status", "sent",
        "success", "failures", "terminal_failures", "timeout",
        "success_rate", "actual_rps", "attempts", "failovers", "health_checks",
        "health_success", "client_window_health_checks",
        "client_window_health_success", "pre_measurement_health_checks",
        "pre_measurement_health_success", "health_directed_selections",
        "attempt_timeouts", "nacks", "late_callbacks", "mean_ms", "p50_ms", "p95_ms",
        "p99_ms", "handler_executions_observed",
        "server_handler_executions_snapshot_exact",
        "server_handler_executions_started_in_client_window_exact",
        "server_extra_executions_per_request_exact",
        "server_request_ids_with_multiple_executions_exact",
        "application_rpc_calls", "application_messages", "message_definition",
        "notification_interests", "notification_data", "input_interests",
        "input_data", "result_interests", "result_data",
        "traffic_launch_offset_s", "measurement_start_lateness_ms",
        "trace_sha256", "command_sha256", "runtime_commands_sha256",
        "evidence_manifest_sha256", "output_dir",
    ]
    with (output_dir / "campaign-cells.csv").open("w", newline="") as raw:
        writer = csv.DictWriter(raw, fieldnames=cell_fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(cells)


def source_hash_set_digest(source_hashes):
    return sha256_text(json.dumps(
        source_hashes, sort_keys=True, separators=(",", ":"), allow_nan=False))


def collect_cell_evidence(run_dir, cell, manifest):
    run_dir = Path(run_dir)
    excluded = {"cell.json", "evidence-hashes.json"}
    entries = []
    for path in sorted(run_dir.rglob("*")):
        if not path.is_file() or path.name in excluded or path.name.startswith("."):
            continue
        entries.append({
            "path": str(path.relative_to(run_dir)),
            "sha256": sha256_file(path),
            "size_bytes": path.stat().st_size,
        })
    evidence_path = run_dir / "evidence-hashes.json"
    write_json(evidence_path, {
        "schema": "ndnsf-mobility-cell-evidence-v1",
        "created_at": utc_now(),
        "campaign_id": manifest["campaign_id"],
        "cell_id": cell["cell_id"],
        "system_id": cell["system"],
        "range_m": cell["range_m"],
        "command_sha256": cell["command_sha256"],
        "trace_sha256": cell["trace_sha256"],
        "source_hash_set_sha256": source_hash_set_digest(manifest["source_hashes"]),
        "files": entries,
    })
    return evidence_path


def expected_system_label(system_id, configuration=None):
    strict_grpc = bool((configuration or {}).get("grpc_no_health_routing", False))
    parallel_grpc = bool((configuration or {}).get("grpc_parallel", False))
    scoped_count = len(configured_provider_nodes(configuration=configuration))
    single = system_id in SINGLE_PROVIDER_SYSTEMS
    base = base_system_id(system_id)
    return {
        "grpc": (
            f"gRPC-PAR-{scoped_count}"
            if parallel_grpc else (
                f"gRPC-SEQ-{scoped_count}"
                if strict_grpc else f"gRPC-HC-{scoped_count}")),
        "nsc": f"NSC-{scoped_count}",
        "ndnsf": "NDNSF",
    }[base] if not single else (
        f"gRPC-{scoped_count}" if base == "grpc" else f"NSC-{scoped_count}")


def validate_result_identity(cell, result, manifest):
    expected = {
        "cell_id": cell["cell_id"],
        "campaign_id": manifest["campaign_id"],
        "system_id": cell["system"],
        "system_label": expected_system_label(cell["system"], manifest["configuration"]),
        "range_m": cell["range_m"],
        "output_dir": cell["output_dir"],
        "command": cell["command"],
        "command_sha256": cell["command_sha256"],
        "trace_sha256": cell["trace_sha256"],
    }
    if not isinstance(result, dict):
        raise RuntimeError("cell result is not an object")
    for key, value in expected.items():
        if result.get(key) != value:
            raise RuntimeError(
                f"cell result {key} mismatch: {result.get(key)!r} != {value!r}")
    if result.get("status") not in {"passed", "failed", "interrupted"}:
        raise RuntimeError(f"invalid cell result status: {result.get('status')!r}")


def validate_run_record(cell, run, result, manifest):
    expected = {
        "cell_id": cell["cell_id"],
        "campaign_id": manifest["campaign_id"],
        "system_id": cell["system"],
        "system_label": expected_system_label(cell["system"], manifest["configuration"]),
        "range_m": cell["range_m"],
        "status": result["status"],
        "command": cell["command"],
        "command_sha256": cell["command_sha256"],
        "trace_sha256": cell["trace_sha256"],
        "output_dir": cell["output_dir"],
    }
    if not isinstance(run, dict):
        raise RuntimeError("cell run record is not an object")
    for key, value in expected.items():
        if run.get(key) != value:
            raise RuntimeError(
                f"cell run {key} mismatch: {run.get(key)!r} != {value!r}")
    if run["status"] == "passed" and run.get("returncode") != 0:
        raise RuntimeError("passed cell run must have returncode 0")
    if (run["status"] != "passed" and
            (not isinstance(result.get("error"), str) or not result["error"])):
        raise RuntimeError("non-passed cell result must record an error")


def required_evidence_paths(cell, result):
    required = {"cell-manifest.json"}
    if result["status"] != "passed":
        return required
    required.update({"summary.json", "mobility_trace.csv"})
    if base_system_id(cell["system"]) == "grpc":
        required.update({
            "runtime-commands.json", "grpc-client.log", "grpc-stats.log",
            "grpc-server-stats.json",
            *{f"grpc-server-{provider}.log" for provider in provider_nodes()},
            *{f"grpc-provider-stats-{provider}.json" for provider in provider_nodes()},
        })
    elif base_system_id(cell["system"]) == "nsc":
        required.update({
            "runtime-commands.json", "nsc-consumer.log",
            *{f"nsc-producer-{provider}.log" for provider in provider_nodes()},
        })
    return required


def verify_cell_evidence(run_dir, result, cell, manifest):
    run_dir = Path(run_dir).resolve()
    evidence_path = run_dir / "evidence-hashes.json"
    if (result.get("evidence_manifest_file") != str(evidence_path) or
            not evidence_path.is_file() or
            sha256_file(evidence_path) != result.get("evidence_manifest_sha256")):
        raise RuntimeError("cell evidence manifest missing or hash-mismatched")
    evidence = json.loads(evidence_path.read_text())
    if evidence.get("schema") != "ndnsf-mobility-cell-evidence-v1":
        raise RuntimeError("cell evidence schema mismatch")
    expected_identity = {
        "campaign_id": manifest["campaign_id"],
        "cell_id": cell["cell_id"],
        "system_id": cell["system"],
        "range_m": cell["range_m"],
        "command_sha256": cell["command_sha256"],
        "trace_sha256": cell["trace_sha256"],
        "source_hash_set_sha256": source_hash_set_digest(manifest["source_hashes"]),
    }
    for key, value in expected_identity.items():
        if evidence.get(key) != value:
            raise RuntimeError(f"cell evidence {key} mismatch")
    entries = evidence.get("files")
    if not isinstance(entries, list) or not entries:
        raise RuntimeError("cell evidence file list is empty or malformed")
    observed = set()
    observed_hashes = {}
    for entry in entries:
        if not isinstance(entry, dict):
            raise RuntimeError("cell evidence entry is not an object")
        relative = entry.get("path")
        if (not isinstance(relative, str) or not relative or "\\" in relative or
                Path(relative).is_absolute() or ".." in Path(relative).parts or
                Path(relative).as_posix() != relative):
            raise RuntimeError(f"invalid cell evidence path: {relative!r}")
        if relative in observed:
            raise RuntimeError(f"duplicate cell evidence path: {relative}")
        observed.add(relative)
        observed_hashes[relative] = entry.get("sha256")
        if (not isinstance(entry.get("size_bytes"), int) or
                entry["size_bytes"] < 0 or
                re.fullmatch(r"[0-9a-f]{64}", str(entry.get("sha256"))) is None):
            raise RuntimeError(f"invalid cell evidence metadata: {relative}")
        path = (run_dir / relative).resolve()
        try:
            path.relative_to(run_dir)
        except ValueError as error:
            raise RuntimeError("cell evidence path escapes run directory") from error
        if (not path.is_file() or sha256_file(path) != entry.get("sha256") or
                path.stat().st_size != entry.get("size_bytes")):
            raise RuntimeError(f"cell evidence mismatch: {relative}")
    missing = required_evidence_paths(cell, result) - observed
    if missing:
        raise RuntimeError(
            "cell evidence lacks required artifacts: " + ", ".join(sorted(missing)))
    eligible = {
        str(path.relative_to(run_dir))
        for path in run_dir.rglob("*")
        if path.is_file() and path.name not in {"cell.json", "evidence-hashes.json"}
        and not path.name.startswith(".")
    }
    if observed != eligible:
        raise RuntimeError("cell evidence inventory is incomplete or contains excluded files")
    if base_system_id(cell["system"]) == "grpc" and result["status"] == "passed":
        if observed_hashes.get("grpc-server-stats.json") != result.get("server_stats_sha256"):
            raise RuntimeError("gRPC Stats hash is not bound to cell evidence")


def validate_cell_result(cell, result, manifest):
    validate_result_identity(cell, result, manifest)
    if result["status"] != "passed":
        raise RuntimeError("full cell-result validation requires passed status")
    if cell["system"] == "ndnsf":
        configuration = manifest["configuration"]
        expected_sent = int(round(
            configuration["duration_s"] * configuration["rate_rps"]))
        sent = required_number(result, "sent", integer=True)
        accepted = required_number(result, "accepted", integer=True)
        completed = required_number(result, "completed", integer=True)
        success = required_number(result, "success", integer=True)
        timeout = required_number(result, "timeout", integer=True)
        bad_response = required_number(result, "bad_response", integer=True)
        attempts = required_number(result, "attempts", integer=True)
        if sent != expected_sent:
            raise RuntimeError(f"parent NDNSF sent validation failed: {sent} != {expected_sent}")
        if accepted != sent or completed != sent:
            raise RuntimeError("parent NDNSF terminal accounting failed")
        if success + timeout + bad_response != sent:
            raise RuntimeError("parent NDNSF outcome accounting failed")
        if attempts < success or attempts > len(provider_nodes()) * sent:
            raise RuntimeError("parent NDNSF execution accounting failed")
        mean_ms = required_number(result, "mean_ms")
        p50_ms = required_number(result, "p50_ms")
        p95_ms = required_number(result, "p95_ms")
        p99_ms = required_number(result, "p99_ms")
        if mean_ms < 0:
            raise RuntimeError("NDNSF mean latency must be non-negative")
        validate_latency_order(p50_ms, p95_ms, p99_ms)
        if result.get("trace_source") != cell.get("trace_path"):
            raise RuntimeError("NDNSF cell result trace source mismatch")
        lateness_ms = required_finite_number(
            result, "measurement_start_lateness_ms")
        if abs(lateness_ms) > manifest["configuration"]["traffic_phase_tolerance_s"] * 1000.0:
            raise RuntimeError("NDNSF measurement-start lateness exceeds tolerance")
        required_number(result, "measurement_start_monotonic_s")
        required_number(result, "measurement_start_target_monotonic_s")
        traffic_offset_s = required_number(result, "traffic_launch_offset_s")
        expected_delay = manifest["configuration"]["traffic_start_delay_s"]
        if abs(traffic_offset_s - expected_delay) > (
                manifest["configuration"]["traffic_phase_tolerance_s"]):
            raise RuntimeError("NDNSF traffic phase offset mismatch")
        runtime_commands_path = Path(cell["output_dir"]) / "runtime-commands.json"
        if (result.get("runtime_commands_file") != str(runtime_commands_path)
                or not runtime_commands_path.is_file()
                or sha256_file(runtime_commands_path)
                != result.get("runtime_commands_sha256")):
            raise RuntimeError("NDNSF runtime command evidence missing or hash-mismatched")
        return
    if result.get("summary_marker") not in {
            "GRPC_FAILOVER_RATE", "NSC_FAILOVER_SUMMARY"}:
        raise RuntimeError("cell lacks the exact failover summary marker")
    configuration = manifest["configuration"]
    expected_sent = int(round(
        configuration["duration_s"] * configuration["rate_rps"]))
    sent = required_number(result, "sent", integer=True)
    success = required_number(result, "success", integer=True)
    attempts = required_number(result, "attempts", integer=True)
    failovers = required_number(result, "failovers", integer=True)
    if sent != expected_sent:
        raise RuntimeError(f"parent sent validation failed: {sent} != {expected_sent}")
    if manifest.get("formal_baselines") and sent != 300:
        raise RuntimeError("formal cells must contain exactly 300 logical requests")
    parallel_grpc = (
        cell["system"] == "grpc" and
        bool(configuration.get("grpc_parallel", False)))
    provider_count = len(configured_provider_nodes(configuration=configuration))
    if not sent <= attempts <= provider_count * sent:
        raise RuntimeError("parent attempt validation failed")
    if parallel_grpc:
        if failovers != 0:
            raise RuntimeError("parallel gRPC must not report serial failovers")
        if required_number(result, "parallel_issued", integer=True) != attempts:
            raise RuntimeError("parallel gRPC issued-attempt accounting failed")
    elif failovers != attempts - sent:
        raise RuntimeError("parent failover validation failed")
    validate_latency_order(
        required_number(result, "p50_ms"),
        required_number(result, "p95_ms"),
        required_number(result, "p99_ms"))
    if required_number(result, "mean_ms") < 0:
        raise RuntimeError("mean latency must be non-negative")
    success_rate = required_number(result, "success_rate")
    actual_rps = required_number(result, "actual_rps")
    lateness_ms = required_finite_number(result, "measurement_start_lateness_ms")
    if success_rate > 100.0:
        raise RuntimeError("success_rate exceeds 100 percent")
    if abs(success_rate - (100.0 * success / sent if sent else 0.0)) > 0.01:
        raise RuntimeError("success_rate does not match terminal counts")
    if actual_rps > configuration["rate_rps"] + 1e-6:
        raise RuntimeError("actual_rps exceeds offered rate")
    if abs(lateness_ms) > configuration["traffic_phase_tolerance_s"] * 1000.0:
        raise RuntimeError("parent measurement-start lateness exceeds tolerance")
    required_number(result, "application_messages", integer=True)
    runtime_commands_path = Path(cell["output_dir"]) / "runtime-commands.json"
    if (result.get("runtime_commands_file") != str(runtime_commands_path) or
            not runtime_commands_path.is_file() or
            sha256_file(runtime_commands_path) != result.get("runtime_commands_sha256")):
        raise RuntimeError("runtime command evidence missing or hash-mismatched")
    if result.get("trace_source") != cell.get("trace_path"):
        raise RuntimeError("cell result trace source mismatch")
    if base_system_id(cell["system"]) == "grpc":
        failures = required_number(result, "failures", integer=True)
        if success + failures != sent:
            raise RuntimeError("parent gRPC terminal accounting failed")
        for key in (
                "application_rpc_calls", "health_checks", "health_success",
                "handler_executions_observed",
                "server_handler_executions_snapshot_exact",
                "server_handler_executions_started_in_client_window_exact",
                "server_extra_executions_per_request_exact",
                "server_request_ids_with_multiple_executions_exact"):
            required_number(result, key, integer=True)
        stats_path = Path(cell["output_dir"]) / "grpc-server-stats.json"
        if (result.get("server_stats_file") != str(stats_path) or
                not stats_path.is_file() or
                sha256_file(stats_path) != result.get("server_stats_sha256")):
            raise RuntimeError("gRPC exact Stats snapshot missing or hash-mismatched")
        snapshot = json.loads(stats_path.read_text())
        measurement_start_s = required_number(
            result, "measurement_start_monotonic_s")
        exact_stats = summarize_grpc_server_stats(
            snapshot, sent, measurement_start_s, configuration["duration_s"])
        for key, expected_value in exact_stats.items():
            if result.get(key) != expected_value:
                raise RuntimeError(f"gRPC Stats-derived result mismatch: {key}")
        if (exact_stats["server_handler_executions_snapshot_exact"] > attempts or
                exact_stats["server_unique_request_ids_executed"] > sent):
            raise RuntimeError("gRPC server execution counts exceed issued workload")
        for provider_id, provider_snapshot in snapshot["providers"].items():
            provider_path = (
                Path(cell["output_dir"]) /
                f"grpc-provider-stats-{provider_id}.json")
            if (not provider_path.is_file() or
                    json.loads(provider_path.read_text()) != provider_snapshot):
                raise RuntimeError(
                    f"gRPC Provider Stats snapshot mismatch: {provider_id}")
    else:
        terminal = required_number(result, "terminal_failures", integer=True)
        if success + terminal != sent:
            raise RuntimeError("parent NSC terminal accounting failed")
        for key in ("attempt_timeouts", "nacks", "late_callbacks"):
            required_number(result, key, integer=True)
    if manifest.get("smoke") and failovers < 1:
        raise RuntimeError("smoke cell did not exercise actual failover")


def validate_terminal_receipt(cell, receipt, manifest, run_dir):
    expected_manifest = {
        "schema": "ndnsf-mobility-baseline-cell-v1",
        "campaign_id": manifest["campaign_id"], **cell,
    }
    if not isinstance(receipt, dict) or receipt.get("manifest") != expected_manifest:
        raise RuntimeError(f"receipt manifest mismatch for {cell['cell_id']}")
    cell_manifest_path = Path(run_dir) / "cell-manifest.json"
    if (not cell_manifest_path.is_file() or
            json.loads(cell_manifest_path.read_text()) != expected_manifest):
        raise RuntimeError(f"on-disk cell manifest mismatch for {cell['cell_id']}")
    result = receipt.get("result")
    validate_result_identity(cell, result, manifest)
    run = receipt.get("run")
    validate_run_record(cell, run, result, manifest)
    if result["status"] == "passed":
        validate_cell_result(cell, result, manifest)
    verify_cell_evidence(run_dir, result, cell, manifest)
    return run, result


def write_campaign_state(output_dir, manifest, runs, results, status):
    write_campaign_csvs(output_dir, runs, results)
    report = {
        "schema": CAMPAIGN_SCHEMA,
        "status": status,
        "updated_at": utc_now(),
        "manifest": manifest,
        "runs": runs,
        "cells": results,
    }
    write_json(output_dir / "campaign-summary.json", report)
    return report


def execute_campaign(args, output_dir, manifest, resume=False):
    validate_campaign_manifest(manifest)
    manifest_path = output_dir / "campaign-manifest.json"
    if resume:
        if not manifest_path.is_file():
            raise RuntimeError("resume directory has no campaign-manifest.json")
        if json.loads(manifest_path.read_text()) != manifest:
            raise RuntimeError("resume manifest changed on disk")
    else:
        write_json(manifest_path, manifest)
    runs = []
    results = []
    interrupted = False
    for cell in manifest["cells"]:
        run_dir = Path(cell["output_dir"])
        receipt_path = run_dir / "cell.json"
        if resume and receipt_path.is_file():
            receipt = json.loads(receipt_path.read_text())
            run, result = validate_terminal_receipt(
                cell, receipt, manifest, run_dir)
            runs.append(run)
            results.append(result)
            continue
        if resume and run_dir.exists() and any(run_dir.iterdir()):
            cell_manifest = {
                "schema": "ndnsf-mobility-baseline-cell-v1",
                "campaign_id": manifest["campaign_id"], **cell,
            }
            cell_manifest_path = run_dir / "cell-manifest.json"
            if cell_manifest_path.is_file():
                if json.loads(cell_manifest_path.read_text()) != cell_manifest:
                    raise RuntimeError(
                        f"orphan cell manifest mismatch for {cell['cell_id']}")
            else:
                write_json(cell_manifest_path, cell_manifest)
            result = {
                "cell_id": cell["cell_id"],
                "campaign_id": manifest["campaign_id"],
                "system_id": cell["system"],
                "system_label": expected_system_label(
                    cell["system"], manifest["configuration"]),
                "range_m": cell["range_m"], "status": "interrupted",
                "error": "pre-existing non-terminal cell directory; not rerun",
                "trace_sha256": cell["trace_sha256"],
                "command": cell["command"],
                "command_sha256": cell["command_sha256"],
                "output_dir": cell["output_dir"],
            }
            run_record = {
                "cell_id": cell["cell_id"],
                "campaign_id": manifest["campaign_id"],
                "system_id": cell["system"],
                "system_label": expected_system_label(
                    cell["system"], manifest["configuration"]),
                "range_m": cell["range_m"], "status": "interrupted",
                "returncode": None, "started_at": None, "completed_at": utc_now(),
                "elapsed_s": None, "command": cell["command"],
                "command_sha256": cell["command_sha256"],
                "trace_sha256": cell["trace_sha256"],
                "output_dir": cell["output_dir"],
            }
            evidence_path = collect_cell_evidence(run_dir, cell, manifest)
            result["evidence_manifest_file"] = str(evidence_path)
            result["evidence_manifest_sha256"] = sha256_file(evidence_path)
            validate_result_identity(cell, result, manifest)
            validate_run_record(cell, run_record, result, manifest)
            verify_cell_evidence(run_dir, result, cell, manifest)
            write_json(receipt_path, {
                "manifest": cell_manifest, "run": run_record, "result": result})
            runs.append(run_record)
            results.append(result)
            write_campaign_state(output_dir, manifest, runs, results, "running")
            continue

        run_dir.mkdir(parents=True, exist_ok=False)
        cell_manifest = {
            "schema": "ndnsf-mobility-baseline-cell-v1",
            "campaign_id": manifest["campaign_id"],
            **cell,
        }
        write_json(run_dir / "cell-manifest.json", cell_manifest)
        log(f"Driver launching {cell['system']} range={cell['range_m']}m")
        started_at = utc_now()
        started_monotonic = time.monotonic()
        child = subprocess.Popen(cell["argv"])
        try:
            returncode = child.wait()
            run_status = "passed" if returncode == 0 else "failed"
        except KeyboardInterrupt:
            interrupted = True
            child.send_signal(signal.SIGINT)
            try:
                returncode = child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                returncode = child.wait()
            run_status = "interrupted"
        completed_at = utc_now()
        elapsed_s = time.monotonic() - started_monotonic
        run_record = {
            "cell_id": cell["cell_id"],
            "campaign_id": manifest["campaign_id"],
            "system_id": cell["system"],
            "system_label": expected_system_label(
                cell["system"], manifest["configuration"]),
            "range_m": cell["range_m"],
            "status": run_status,
            "returncode": returncode,
            "started_at": started_at,
            "completed_at": completed_at,
            "elapsed_s": round(elapsed_s, 6),
            "command": cell["command"],
            "command_sha256": cell["command_sha256"],
            "trace_sha256": cell["trace_sha256"],
            "output_dir": cell["output_dir"],
        }
        summary_path = run_dir / "summary.json"
        result = {
            "cell_id": cell["cell_id"],
            "campaign_id": manifest["campaign_id"],
            "system_id": cell["system"],
            "system_label": expected_system_label(
                cell["system"], manifest["configuration"]),
            "range_m": cell["range_m"],
            "status": run_status,
            "trace_sha256": cell["trace_sha256"],
            "command": cell["command"],
            "command_sha256": cell["command_sha256"],
            "output_dir": cell["output_dir"],
        }
        if run_status == "passed" and summary_path.is_file():
            try:
                payload = json.loads(summary_path.read_text())
                summaries = payload.get("summaries") if isinstance(payload, dict) else None
                if not isinstance(summaries, list) or len(summaries) != 1:
                    raise RuntimeError("cell summary must contain exactly one result")
                child_result = summaries[0]
                if child_result.get("system_id") != cell["system"]:
                    raise RuntimeError("child summary canonical system ID mismatch")
                result.update(child_result)
                result.update({
                    "cell_id": cell["cell_id"],
                    "campaign_id": manifest["campaign_id"],
                    "system_id": cell["system"],
                    "system_label": expected_system_label(
                        cell["system"], manifest["configuration"]),
                    "range_m": cell["range_m"], "status": "passed",
                    "trace_sha256": cell["trace_sha256"],
                    "command": cell["command"],
                    "command_sha256": cell["command_sha256"],
                    "output_dir": cell["output_dir"],
                })
                validate_cell_result(cell, result, manifest)
            except (KeyError, TypeError, ValueError, RuntimeError, json.JSONDecodeError) as error:
                result["status"] = "failed"
                result["error"] = f"parent validation failed: {error}"
                run_record["status"] = "failed"
        elif run_status == "interrupted":
            result["error"] = "campaign interrupted while this cell was active"
        else:
            result["status"] = "failed"
            result["error"] = (
                f"cell exited with {returncode}" if returncode else "summary.json missing")
            run_record["status"] = "failed"
        evidence_path = collect_cell_evidence(run_dir, cell, manifest)
        result["evidence_manifest_file"] = str(evidence_path)
        result["evidence_manifest_sha256"] = sha256_file(evidence_path)
        validate_result_identity(cell, result, manifest)
        validate_run_record(cell, run_record, result, manifest)
        verify_cell_evidence(run_dir, result, cell, manifest)
        write_json(receipt_path, {
            "manifest": cell_manifest, "run": run_record, "result": result})
        runs.append(run_record)
        results.append(result)
        write_campaign_state(output_dir, manifest, runs, results, "running")
        if interrupted:
            break

    if interrupted:
        status = "interrupted"
    else:
        status = "passed" if len(results) == len(manifest["cells"]) and all(
            item.get("status") == "passed" for item in results) else "failed"
    if args.smoke:
        def smoke_cell_ok(item):
            if item.get("smoke_ok") is not True:
                return False
            if item.get("system_id") == "ndnsf":
                return True
            if item.get("execution_mode") == "parallel-first-success":
                return item.get("parallel_issued", 0) >= item.get("sent", 0)
            return item.get("failovers", 0) >= 1

        smoke_ok = (status == "passed" and bool(results) and all(
            smoke_cell_ok(item) for item in results))
        status = "passed" if smoke_ok else ("interrupted" if interrupted else "failed")
        if smoke_ok:
            print("SMOKE_OK")
    report = write_campaign_state(output_dir, manifest, runs, results, status)
    report["completed_at"] = utc_now()
    write_json(output_dir / "campaign-summary.json", report)
    write_json(output_dir / "summary.json", {
        "summaries": results,
        "output_dir": str(output_dir),
    })
    print(json.dumps(report, indent=2))
    return 130 if interrupted else (0 if status == "passed" else 1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--profile", choices=("three-provider", "four-provider-single-ap",
                               "four-provider-multi-ap"),
        default="three-provider",
        help="provider/AP profile; four-provider-single-ap is the primary one-AP screen")
    parser.add_argument(
        "--ap-layout", choices=tuple(AP_LAYOUTS), default="",
        help="coverage geometry (defaults to the selected profile)")
    parser.add_argument(
        "--speed-mps", type=float, default=None,
        help="fixed provider speed for the selected profile")
    parser.add_argument(
        "--trace-profile", choices=("random-waypoint", "single-active-handoff"),
        default="random-waypoint",
        help="deterministic availability schedule used by paired cells")
    parser.add_argument(
        "--handoff-period-s", type=float, default=HANDOFF_PERIOD_S,
        help="active-Provider rotation period for single-active-handoff")
    parser.add_argument(
        "--mobility-warmup-s", type=float, default=0.0,
        help="deterministic RandomWaypoint burn-in before trace timestamp zero")
    parser.add_argument("--ranges", default="100,150,200")
    parser.add_argument("--systems", default="grpc,nsc")
    parser.add_argument(
        "--provider-scope", default="",
        help="comma-separated client target subset; use one provider for no-failover controls",
    )
    parser.add_argument("--duration-s", type=int, default=60)
    parser.add_argument("--rate-rps", type=float, default=5.0)
    parser.add_argument("--processing-delay-ms", type=int, default=5)
    parser.add_argument(
        "--service-workers", type=int, default=4,
        help="matched per-provider service worker/handler count for NDNSF and gRPC")
    parser.add_argument("--timeout-ms", type=int, default=5000)
    parser.add_argument("--ack-timeout-ms", type=int, default=None)
    parser.add_argument("--attempt-timeout-ms", type=int, default=None)
    parser.add_argument("--health-interval-ms", type=int, default=None)
    parser.add_argument(
        "--ndnsf-strategy",
        choices=("first-responding", "all-selected", "random-selection"),
        default="first-responding",
        help="NDNSF parallel selection policy for the mobility comparison")
    parser.add_argument(
        "--ndnsf-response-retry", action="store_true",
        help="enable bounded FirstResponding Response reselection using attempt-timeout-ms")
    parser.add_argument(
        "--ndnsf-response-fault-provider", choices=("A", "B", "C", "D"),
        default="", help="diagnostic Provider whose Response processing is delayed")
    parser.add_argument(
        "--ndnsf-response-fault-delay-ms", type=int, default=0,
        help="diagnostic processing delay for the response-fault Provider")
    parser.add_argument(
        "--ndnsf-standby-ack-delay-ms", type=int, default=0,
        help="diagnostic ACK delay for non-fault Providers")
    health_group = parser.add_mutually_exclusive_group()
    health_group.add_argument(
        "--grpc-no-health-routing", dest="grpc_no_health_routing",
        action="store_true", default=None,
        help="strict gRPC sequential baseline without proactive health routing")
    health_group.add_argument(
        "--grpc-health-routing", dest="grpc_no_health_routing",
        action="store_false",
        help="explicitly enable the experiment health-routing diagnostic")
    parser.add_argument(
        "--grpc-parallel", action="store_true",
        help="diagnostic gRPC first-success fan-out across all providers")
    parser.add_argument(
        "--block-network", action="store_true",
        help="drop ingress/egress packets for out-of-coverage providers instead of pausing them")
    parser.add_argument("--traffic-start-delay-s", type=float, default=2.0)
    parser.add_argument("--settle-seconds", type=int, default=8)
    parser.add_argument("--seed", type=int, default=20)
    parser.add_argument("--output-dir", default="")
    parser.add_argument(
        "--lock-file", default="", help=argparse.SUPPRESS)
    parser.add_argument(
        "--resume", default="",
        help="resume a campaign directory; terminal cells are never rerun")
    parser.add_argument("--formal-baselines", action="store_true",
                        help="run the fixed six-cell gRPC-HC-3/NSC-3 campaign")
    parser.add_argument("--preflight", "--dry-run", dest="preflight", action="store_true",
                        help="validate and print the six formal commands without MiniNDN")
    parser.add_argument("--smoke", action="store_true",
                        help="run a fixed 5-second, 200m two-client smoke campaign")
    parser.add_argument(
        "--include-ndnsf", action="store_true",
        help="include NDNSF as a paired smoke cell")
    parser.add_argument("--single-run", action="store_true",
                        help=argparse.SUPPRESS)
    parser.add_argument("--formal-cell", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--smoke-cell", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--trace-replay", default="", help=argparse.SUPPRESS)
    parser.add_argument("--campaign-id", default="", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.service_workers <= 0:
        parser.error("--service-workers must be positive")
    if args.ndnsf_response_fault_delay_ms < 0 or args.ndnsf_standby_ack_delay_ms < 0:
        parser.error("NDNSF response-fault delays must be non-negative")
    if args.ndnsf_response_fault_provider and not args.ndnsf_response_retry:
        parser.error("--ndnsf-response-fault-provider requires --ndnsf-response-retry")
    try:
        configure_profile(args.profile, args.ap_layout or None, args.speed_mps)
    except ValueError as error:
        parser.error(str(error))
    primary_single_ap = args.profile == "four-provider-single-ap"
    if args.ack_timeout_ms is None:
        args.ack_timeout_ms = 1000 if primary_single_ap else 200
    if args.attempt_timeout_ms is None:
        args.attempt_timeout_ms = 1000 if primary_single_ap else 200
    if args.health_interval_ms is None:
        args.health_interval_ms = 1000 if primary_single_ap else 200
    if args.grpc_no_health_routing is None:
        args.grpc_no_health_routing = primary_single_ap
    if args.handoff_period_s <= 0 or not math.isfinite(args.handoff_period_s):
        parser.error("--handoff-period-s must be finite and positive")
    if args.mobility_warmup_s < 0 or not math.isfinite(args.mobility_warmup_s):
        parser.error("--mobility-warmup-s must be finite and non-negative")
    global CAMPAIGN_LOCK
    if args.lock_file:
        CAMPAIGN_LOCK = Path(args.lock_file).resolve()

    if args.resume and (args.output_dir or args.single_run or args.preflight):
        parser.error("--resume cannot be combined with --output-dir, --single-run, or --preflight")
    if args.preflight:
        args.formal_baselines = True
    if args.smoke and args.formal_baselines:
        parser.error("--smoke and --formal-baselines are mutually exclusive")
    if args.smoke and not args.single_run:
        args.ranges = "200"
        args.systems = "ndnsf,grpc,nsc" if args.include_ndnsf else "grpc,nsc"
        args.duration_s = 5
        args.settle_seconds = min(args.settle_seconds, 1)
        # The deterministic forced-failover trace places the UCLA outage at
        # 2.4--4.4 s. Start the measured stream before that interval so the
        # smoke proof contains a real failed RPC and subsequent failover.
        args.traffic_start_delay_s = 2.0
        # One-second health state remains fresh across the deterministic outage,
        # forcing an actual failed service RPC before failover in the smoke proof.
        args.health_interval_ms = 1000
        args.mobility_warmup_s = 0.0
    if args.smoke_cell:
        args.smoke = True

    ranges = parse_csv_floats(args.ranges)
    systems = parse_csv_strings(args.systems)
    allowed_systems = set(SUPPORTED_SYSTEMS)
    if not ranges:
        parser.error("--ranges must not be empty")
    if not systems or any(system not in allowed_systems for system in systems):
        parser.error("--systems contains an unsupported system")
    if any(system in SINGLE_PROVIDER_SYSTEMS for system in systems):
        try:
            scoped = configured_provider_nodes(args)
        except RuntimeError as error:
            parser.error(str(error))
        if len(scoped) != 1:
            parser.error("single-provider systems require --provider-scope with exactly one provider")
    elif args.provider_scope:
        try:
            configured_provider_nodes(args)
        except RuntimeError as error:
            parser.error(str(error))
    if args.single_run and (len(ranges) != 1 or len(systems) != 1):
        parser.error("internal --single-run requires exactly one range and system")
    if args.formal_cell and systems[0] not in FORMAL_SYSTEMS:
        parser.error("a formal baseline cell cannot run NDNSF")

    if args.formal_baselines:
        ranges = list(FORMAL_RANGES)
        systems = list(FORMAL_SYSTEMS)
        validate_formal_configuration(args, ranges, systems)

    if args.single_run:
        output_dir = Path(args.output_dir).resolve()
        output_dir.mkdir(parents=True, exist_ok=True)
        unexpected = [
            path.name for path in output_dir.iterdir()
            if path.name != "cell-manifest.json"
        ]
        if unexpected:
            parser.error(
                "single-run output directory contains prior evidence: " +
                ", ".join(sorted(unexpected)))
        if args.formal_cell and not args.trace_replay:
            parser.error("formal baseline cell requires --trace-replay")
        summaries = []
        for ap_range in ranges:
            for system in systems:
                log(f"Running {system} AP range={ap_range}m")
                summary = run_one(system, ap_range, args, output_dir)
                summary["system_id"] = system
                summary.setdefault(
                    "system_label",
                    expected_system_label(system, {
                        "grpc_no_health_routing": args.grpc_no_health_routing,
                        "grpc_parallel": getattr(args, "grpc_parallel", False),
                        "provider_scope": configured_provider_nodes(args),
                    }))
                summary["range_m"] = ap_range
                summary["campaign_id"] = args.campaign_id or None
                summary["status"] = "passed"
                summaries.append(summary)
        report = {"summaries": summaries, "output_dir": str(output_dir)}
        write_json(output_dir / "summary.json", report)
        print(json.dumps(report, indent=2))
        return 0

    if args.resume:
        output_dir = Path(args.resume).resolve()
        manifest_path = output_dir / "campaign-manifest.json"
        if not manifest_path.is_file():
            parser.error(f"resume manifest not found: {manifest_path}")
        manifest = json.loads(manifest_path.read_text())
        validate_campaign_manifest(manifest)
        configuration = manifest["configuration"]
        args.formal_baselines = bool(manifest.get("formal_baselines"))
        args.smoke = bool(manifest.get("smoke"))
        args.duration_s = configuration["duration_s"]
        args.rate_rps = configuration["rate_rps"]
        args.processing_delay_ms = configuration["processing_delay_ms"]
        args.timeout_ms = configuration["global_deadline_ms"]
        args.attempt_timeout_ms = configuration["attempt_timeout_ms"]
        args.health_interval_ms = configuration["health_interval_ms"]
        args.grpc_no_health_routing = bool(
            configuration.get("grpc_no_health_routing", False))
        args.grpc_parallel = bool(configuration.get("grpc_parallel", False))
        args.provider_scope = ",".join(
            configuration.get("provider_scope", provider_nodes()))
        args.block_network = bool(configuration.get("block_network", False))
        args.trace_profile = configuration.get("trace_profile", "random-waypoint")
        args.handoff_period_s = float(
            configuration.get("handoff_period_s", HANDOFF_PERIOD_S))
        args.traffic_start_delay_s = configuration["traffic_start_delay_s"]
        args.seed = configuration["seed"]
        systems = list(configuration["systems"])
        current_sources = validate_preflight(systems)
        expected_sources = {
            item["path"]: item["sha256"] for item in manifest["source_hashes"]}
        actual_sources = {item["path"]: item["sha256"] for item in current_sources}
        if actual_sources != expected_sources:
            raise RuntimeError("resume refused because source or binary hashes changed")
        with campaign_lock(CAMPAIGN_LOCK, output_dir, manifest["campaign_id"]):
            return execute_campaign(args, output_dir, manifest, resume=True)

    source_hashes = validate_preflight(systems)
    output_dir = prepare_output_dir(args.output_dir)
    manifest = build_campaign_plan(args, output_dir, ranges, systems, source_hashes)
    if args.preflight:
        report = {
            "schema": CAMPAIGN_SCHEMA,
            "status": "preflight-ok",
            "manifest": manifest,
        }
        write_json(output_dir / "campaign-manifest.json", manifest)
        write_json(output_dir / "campaign-summary.json", report)
        for cell in manifest["cells"]:
            print(cell["command"])
        print("PREFLIGHT_OK")
        return 0
    with campaign_lock(CAMPAIGN_LOCK, output_dir, manifest["campaign_id"]):
        return execute_campaign(args, output_dir, manifest)


if __name__ == "__main__":
    setLogLevel("info")
    raise SystemExit(main())
