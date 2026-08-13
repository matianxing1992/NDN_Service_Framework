#!/usr/bin/env python3
"""Analyze Spec 171 request opportunities without exposing trace state to clients.

The frozen 100 m campaign predates per-request user-result markers for NDNSF
and NSC.  Consequently this analyzer uses the exact gRPC logical-request
latency, NDNSF's Provider-side Request-to-Selection lifecycle cost, and labels
NDNSF Response publication latency as a proxy.  It never presents that proxy
as user-observed end-to-end latency.
"""

from __future__ import annotations

import argparse
import bisect
import collections
import csv
import datetime as dt
import json
import math
from pathlib import Path
import random
import re
import statistics
from typing import Iterable, Mapping, Sequence


PROVIDERS = ("ucla", "wustl", "uiuc", "arizona")
LABEL_TO_PROVIDER = dict(zip("ABCD", PROVIDERS))
OPPORTUNITY_STATES = (
    "NONE_REACHABLE", "INITIAL_REACHABLE", "SWITCH_REQUIRED")

GRPC_ATTEMPT_RE = re.compile(
    r"GRPC_FAILOVER_ATTEMPT request_id=(\d+) attempt=(\d+) "
    r"provider=(\w+) status=(\w+) latency_ms=([0-9.]+)")
NDNSF_STATE_RE = re.compile(
    r"^(\d+(?:\.\d+)?) .*?NDNSF_SELECTION_STATUS.*?state=(\d+)"
    r".*?provider=/example/hello/provider/([A-D]).*?requestId=(/[^ ]+)")
NDNSF_TIMEOUT_RE = re.compile(
    r"user timeout timestampMs=\d+ requestId=(/[^ ]+)")
TARGET_RE = re.compile(r"--target\s+([^=\s]+)=([^\s]+)")


def nearest_rank(values: Sequence[float], probability: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, math.ceil(probability * len(ordered)))
    return ordered[min(rank - 1, len(ordered) - 1)]


def request_timestamp(request_id: str) -> float:
    token = request_id.lstrip("/").split("-", 1)[0]
    parsed = dt.datetime.strptime(token, "%Y%m%dT%H%M%S.%f")
    return parsed.replace(tzinfo=dt.timezone.utc).timestamp()


def parse_target_order(runtime_commands: Path) -> list[str]:
    payload = json.loads(runtime_commands.read_text())
    command = str(payload.get("client", ""))
    order = [match.group(1) for match in TARGET_RE.finditer(command)]
    if not order:
        raise ValueError(f"no --target entries in {runtime_commands}")
    return order


def load_trace(path: Path) -> tuple[list[float], dict[float, dict[str, bool]]]:
    states: dict[float, dict[str, bool]] = {}
    with path.open(newline="") as stream:
        for row in csv.DictReader(stream):
            instant = round(float(row["time_s"]), 6)
            states.setdefault(instant, {})[row["provider"]] = (
                int(row["in_range"]) != 0)
    if not states:
        raise ValueError(f"empty trace: {path}")
    for instant, state in states.items():
        missing = set(PROVIDERS) - set(state)
        if missing:
            raise ValueError(
                f"trace {path} time {instant} misses Providers {sorted(missing)}")
    return sorted(states), states


def trace_state_at(
        times: Sequence[float], states: Mapping[float, Mapping[str, bool]],
        instant: float) -> Mapping[str, bool]:
    index = max(0, bisect.bisect_right(times, instant) - 1)
    return states[times[index]]


def classify_opportunity(
        state: Mapping[str, bool], initial_provider: str) -> str:
    if not any(state.values()):
        return "NONE_REACHABLE"
    if state.get(initial_provider, False):
        return "INITIAL_REACHABLE"
    return "SWITCH_REQUIRED"


def load_summary(cell: Path) -> dict:
    payload = json.loads((cell / "summary.json").read_text())
    summaries = payload.get("summaries", [])
    if len(summaries) != 1:
        raise ValueError(f"expected one summary in {cell / 'summary.json'}")
    return summaries[0]


def parse_grpc_requests(cell: Path) -> dict[int, dict]:
    attempts: dict[int, list[dict]] = collections.defaultdict(list)
    text = (cell / "grpc-client.log").read_text(errors="replace")
    for match in GRPC_ATTEMPT_RE.finditer(text):
        attempts[int(match.group(1))].append({
            "attempt": int(match.group(2)),
            "provider": match.group(3),
            "status": match.group(4),
            "latency_ms": float(match.group(5)),
        })
    result: dict[int, dict] = {}
    for request_id, request_attempts in attempts.items():
        ordered = sorted(request_attempts, key=lambda item: item["attempt"])
        success = next(
            (item for item in ordered if item["status"] == "OK"), None)
        before_success = ordered if success is None else ordered[:ordered.index(success)]
        result[request_id] = {
            "terminal_status": "SUCCESS" if success else "FAILURE",
            "user_latency_ms": (
                sum(item["latency_ms"] for item in ordered)
                if success else None),
            "failed_attempt_time_ms": sum(
                item["latency_ms"] for item in before_success),
            "attempts_or_provider_executions": len(ordered),
            "selected_provider": success["provider"] if success else None,
            "first_attempt_status": ordered[0]["status"] if ordered else None,
            "failure_stage": None if success else (
                ordered[-1]["status"] if ordered else "NO_ATTEMPT"),
        }
    return result


def parse_ndnsf_requests(cell: Path) -> tuple[list[str], dict[str, dict]]:
    events: dict[str, dict] = collections.defaultdict(lambda: {
        "selection_ms": None,
        "response_publish_ms": None,
        "execution_providers": set(),
        "selected_provider": None,
    })
    for path in sorted(cell.glob("ndnsf-provider-*.log")):
        for line in path.read_text(errors="replace").splitlines():
            match = NDNSF_STATE_RE.search(line)
            if match is None:
                continue
            timestamp = float(match.group(1))
            state = int(match.group(2))
            provider = LABEL_TO_PROVIDER[match.group(3)]
            request_id = match.group(4)
            elapsed_ms = (timestamp - request_timestamp(request_id)) * 1000.0
            record = events[request_id]
            if state == 1 and record["selection_ms"] is None:
                record["selection_ms"] = elapsed_ms
                record["selected_provider"] = provider
            elif state == 3:
                record["execution_providers"].add(provider)
            elif state == 4:
                if record["response_publish_ms"] is None:
                    record["response_publish_ms"] = elapsed_ms

    user_text = (cell / "ndnsf-user.log").read_text(errors="replace")
    timeouts = set(NDNSF_TIMEOUT_RE.findall(user_text))
    request_ids = sorted(set(events) | timeouts, key=request_timestamp)
    result: dict[str, dict] = {}
    for request_id in request_ids:
        event = events[request_id]
        success = event["response_publish_ms"] is not None
        result[request_id] = {
            "terminal_status": "SUCCESS" if success else "TIMEOUT",
            "user_latency_ms": None,
            "pre_execution_switch_cost_ms": event["selection_ms"],
            "response_publish_latency_proxy_ms": event["response_publish_ms"],
            "failed_attempt_time_ms": None,
            "attempts_or_provider_executions": len(event["execution_providers"]),
            "selected_provider": event["selected_provider"],
            "first_attempt_status": None,
            "failure_stage": None if success else "NO_RESPONSE_OBSERVED",
        }
    return request_ids, result


def paired_bootstrap_mean(
        differences: Sequence[float], *, seed: int, repetitions: int) -> dict:
    if not differences:
        raise ValueError("paired bootstrap requires at least one difference")
    rng = random.Random(seed)
    draws = [
        statistics.mean(rng.choices(differences, k=len(differences)))
        for _ in range(repetitions)
    ]
    draws.sort()
    return {
        "mean": statistics.mean(differences),
        "ci95_low": draws[max(0, math.floor(0.025 * repetitions))],
        "ci95_high": draws[min(repetitions - 1, math.ceil(0.975 * repetitions) - 1)],
        "repetitions": repetitions,
        "seed": seed,
    }


def _float_or_blank(value: object) -> object:
    return "" if value is None else value


def analyze(
        campaign_root: Path, output_dir: Path, *, range_m: int,
        speed_token: str, rate_rps: float, bootstrap_seed: int,
        bootstrap_repetitions: int) -> dict:
    rows: list[dict] = []
    per_seed: list[dict] = []
    seed_dirs = sorted(campaign_root.glob("seed-*"), key=lambda path: int(
        path.name.split("-", 1)[1]))
    if not seed_dirs:
        raise ValueError(f"no seed directories under {campaign_root}")

    for seed_dir in seed_dirs:
        seed = int(seed_dir.name.split("-", 1)[1])
        condition = seed_dir / f"range-{range_m}-speed-{speed_token}"
        ndnsf_cell = condition / "ndnsf"
        grpc_cell = condition / "grpc"
        nsc_cell = condition / "nsc"
        for cell in (ndnsf_cell, grpc_cell, nsc_cell):
            if not cell.is_dir():
                raise ValueError(f"missing paired cell: {cell}")

        trace_times, trace_states = load_trace(condition / "trace.csv")
        endpoint_order = parse_target_order(grpc_cell / "runtime-commands.json")
        if endpoint_order != list(PROVIDERS):
            raise ValueError(
                f"unexpected endpoint order {endpoint_order} in {grpc_cell}")
        nd_ids, nd_requests = parse_ndnsf_requests(ndnsf_cell)
        grpc_requests = parse_grpc_requests(grpc_cell)
        summaries = {
            "ndnsf": load_summary(ndnsf_cell),
            "grpc": load_summary(grpc_cell),
            "nsc": load_summary(nsc_cell),
        }
        request_count = int(summaries["grpc"]["sent"])
        if len(nd_ids) != request_count or len(grpc_requests) != request_count:
            raise ValueError(
                f"seed {seed} request reconciliation failed: "
                f"NDNSF={len(nd_ids)} gRPC={len(grpc_requests)} expected={request_count}")
        if sum(item["terminal_status"] == "SUCCESS"
               for item in nd_requests.values()) != int(summaries["ndnsf"]["success"]):
            raise ValueError(f"seed {seed} NDNSF success reconciliation failed")
        if sum(item["terminal_status"] == "SUCCESS"
               for item in grpc_requests.values()) != int(summaries["grpc"]["success"]):
            raise ValueError(f"seed {seed} gRPC success reconciliation failed")

        grpc_latencies = [
            item["user_latency_ms"] for item in grpc_requests.values()
            if item["user_latency_ms"] is not None]
        reconstructed_p95 = nearest_rank(grpc_latencies, 0.95)
        if reconstructed_p95 is None or abs(
                reconstructed_p95 - float(summaries["grpc"]["p95_ms"])) > 5.0:
            raise ValueError(
                f"seed {seed} gRPC p95 reconstruction mismatch: "
                f"{reconstructed_p95} vs {summaries['grpc']['p95_ms']}")

        start_s = float(summaries["grpc"].get("traffic_launch_offset_s", 4.0))
        classifications: list[tuple[str, str, str]] = []
        for request_index in range(request_count):
            publication_trace_s = start_s + request_index / rate_rps
            state = trace_state_at(trace_times, trace_states, publication_trace_s)
            initial_provider = endpoint_order[request_index % len(endpoint_order)]
            opportunity = classify_opportunity(state, initial_provider)
            reachable = ";".join(name for name in PROVIDERS if state[name])
            classifications.append((opportunity, initial_provider, reachable))

            common = {
                "condition": f"range-{range_m}-speed-{speed_token}",
                "seed": seed,
                "logical_request_id": request_index,
                "publication_trace_s": publication_trace_s,
                "initial_provider": initial_provider,
                "reachable_providers": reachable,
                "opportunity_state": opportunity,
            }
            nd_record = dict(common, system="ndnsf", **nd_requests[nd_ids[request_index]])
            grpc_record = dict(common, system="grpc-seq-4", **grpc_requests[request_index])
            nsc_record = dict(
                common, system="nsc-4",
                terminal_status="NOT_AVAILABLE_IN_FROZEN_LOG",
                user_latency_ms=None,
                pre_execution_switch_cost_ms=None,
                response_publish_latency_proxy_ms=None,
                failed_attempt_time_ms=None,
                attempts_or_provider_executions=None,
                selected_provider=None,
                first_attempt_status=None,
                failure_stage="PER_REQUEST_MARKERS_NOT_RETAINED")
            rows.extend((nd_record, grpc_record, nsc_record))

        switch_indexes = [
            index for index, item in enumerate(classifications)
            if item[0] == "SWITCH_REQUIRED"]
        nd_switch = [nd_requests[nd_ids[index]] for index in switch_indexes]
        grpc_switch = [grpc_requests[index] for index in switch_indexes]
        nd_costs = [
            item["pre_execution_switch_cost_ms"] for item in nd_switch
            if item["pre_execution_switch_cost_ms"] is not None]
        grpc_costs = [
            item["failed_attempt_time_ms"] for item in grpc_switch
            if item["terminal_status"] == "SUCCESS"]
        per_seed.append({
            "seed": seed,
            "request_count": request_count,
            "opportunity_counts": dict(collections.Counter(
                item[0] for item in classifications)),
            "switch_required": len(switch_indexes),
            "ndnsf_success": sum(
                item["terminal_status"] == "SUCCESS" for item in nd_switch),
            "grpc_success": sum(
                item["terminal_status"] == "SUCCESS" for item in grpc_switch),
            "ndnsf_selection_cost_p95_ms": nearest_rank(nd_costs, 0.95),
            "grpc_failed_attempt_cost_p95_ms": nearest_rank(grpc_costs, 0.95),
            "grpc_first_attempt_deadline_fraction": (
                sum(item["first_attempt_status"] == "DEADLINE_EXCEEDED"
                    for item in grpc_switch) / len(grpc_switch)
                if grpc_switch else None),
        })

    paired_seeds = [item for item in per_seed if (
        item["switch_required"] > 0 and
        item["ndnsf_selection_cost_p95_ms"] is not None and
        item["grpc_failed_attempt_cost_p95_ms"] is not None)]
    differences = [
        item["grpc_failed_attempt_cost_p95_ms"] -
        item["ndnsf_selection_cost_p95_ms"] for item in paired_seeds]
    bootstrap = paired_bootstrap_mean(
        differences, seed=bootstrap_seed, repetitions=bootstrap_repetitions)
    verdict = (
        "CONDITIONAL_SWITCHING_COST_REDUCTION"
        if len(paired_seeds) >= 5 and bootstrap["ci95_low"] > 0
        else "NO_DEMONSTRATED_SWITCHING_COST_REDUCTION")
    report = {
        "schema": "spec171-opportunity-analysis-v1",
        "campaign_root": str(campaign_root.resolve()),
        "range_m": range_m,
        "speed_token": speed_token,
        "rate_rps": rate_rps,
        "provider_order": list(PROVIDERS),
        "ndnsf_user_latency_available": False,
        "ndnsf_latency_proxy": "request-id timestamp to Provider RESPONSE_PUBLISHED",
        "switching_cost_definition": {
            "ndnsf": "request-id timestamp to selected Provider SELECTION_RECEIVED",
            "grpc": "sum of failed RPC attempt durations before the successful RPC",
        },
        "per_seed": per_seed,
        "paired_seed_p95_reduction_ms": bootstrap,
        "verdict": verdict,
    }

    output_dir.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "condition", "seed", "logical_request_id", "system",
        "publication_trace_s", "initial_provider", "reachable_providers",
        "opportunity_state", "terminal_status", "user_latency_ms",
        "pre_execution_switch_cost_ms", "response_publish_latency_proxy_ms",
        "failed_attempt_time_ms", "attempts_or_provider_executions",
        "selected_provider", "first_attempt_status", "failure_stage"]
    with (output_dir / "opportunity-requests.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: _float_or_blank(row.get(key)) for key in fieldnames})
    (output_dir / "opportunity-summary.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n")

    total_switch = sum(item["switch_required"] for item in per_seed)
    nd_success = sum(item["ndnsf_success"] for item in per_seed)
    grpc_success = sum(item["grpc_success"] for item in per_seed)
    lines = [
        "# Spec 171 Provider-switch opportunity analysis",
        "",
        f"- Verdict: `{verdict}`",
        f"- `SWITCH_REQUIRED` requests: {total_switch}",
        f"- NDNSF / gRPC successes in those requests: {nd_success} / {grpc_success}",
        "- NDNSF metric: Request timestamp to selected Provider receiving Selection.",
        "- gRPC metric: time spent in failed sequential attempts before success.",
        "- NDNSF end-to-end per-request latency is unavailable in the frozen logs; "
        "Response publication is retained only as a labelled proxy.",
        "- Unconditional success and user-latency results remain primary.",
        "",
        "## Paired seed result",
        "",
        f"Mean seed-p95 reduction: {bootstrap['mean']:.2f} ms "
        f"(95% paired bootstrap CI {bootstrap['ci95_low']:.2f} to "
        f"{bootstrap['ci95_high']:.2f} ms; n={len(paired_seeds)} seeds).",
        "",
        "The result supports only a conditional tail-cost claim. It does not "
        "show an unconditional success-rate advantage, and it does not claim "
        "that gRPC cannot use an external resolver or health system.",
    ]
    (output_dir / "README.md").write_text("\n".join(lines) + "\n")
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("campaign_root", type=Path)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--range-m", type=int, default=100)
    parser.add_argument("--speed-token", default="2p0")
    parser.add_argument("--rate-rps", type=float, default=5.0)
    parser.add_argument("--bootstrap-seed", type=int, default=171)
    parser.add_argument("--bootstrap-repetitions", type=int, default=20000)
    args = parser.parse_args()
    report = analyze(
        args.campaign_root, args.output_dir, range_m=args.range_m,
        speed_token=args.speed_token, rate_rps=args.rate_rps,
        bootstrap_seed=args.bootstrap_seed,
        bootstrap_repetitions=args.bootstrap_repetitions)
    print(json.dumps({
        "verdict": report["verdict"],
        "output_dir": str(args.output_dir.resolve()),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
