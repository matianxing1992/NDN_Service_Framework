#!/usr/bin/env python3
"""Analyze the preregistered, boundary-safe Spec 171 opportunity holdout."""

from __future__ import annotations

import argparse
import collections
import csv
import json
import math
from pathlib import Path
import random
import re
import statistics


PROVIDERS = ("ucla", "wustl", "uiuc", "arizona")
LABELS = dict(zip("ABCD", PROVIDERS))
SYSTEMS = ("ndnsf", "grpc", "nsc")
NDNSF_RESULT_RE = re.compile(
    r"INTERMITTENT_USER_REQUEST_RESULT request_index=(\d+) "
    r"requestId=(/[^ ]+) status=(\w+) latency_ms=([0-9.]+) "
    r"published_monotonic_ms=([0-9.]+)")
NDNSF_STATE_RE = re.compile(
    r"NDNSF_SELECTION_STATUS.*?state=(\d+).*?"
    r"provider=/example/hello/provider/([A-D]).*?requestId=(/[^ ]+)")
GRPC_PUBLISHED_RE = re.compile(
    r"GRPC_REQUEST_PUBLISHED request_id=(\d+) monotonic_s=([0-9.]+)")
GRPC_ATTEMPT_RE = re.compile(
    r"GRPC_FAILOVER_ATTEMPT request_id=(\d+) attempt=(\d+) "
    r"provider=(\w+) status=(\w+) latency_ms=([0-9.]+)")
NSC_RESULT_RE = re.compile(
    r"NSC_REQUEST_RESULT request_id=(\d+) status=(\w+) "
    r"latency_ms=([0-9.]+) attempts=(\d+) provider=([^\s]+) "
    r"published_monotonic_ms=([0-9.]+)")
TARGET_RE = re.compile(r"--target\s+([^=\s]+)=([^\s]+)")


def nearest_rank(values: list[float], probability: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1,
                       max(0, math.ceil(probability * len(ordered)) - 1))]


def paired_bootstrap_mean(
        values: list[float], *, seed: int = 171,
        repetitions: int = 20000) -> dict:
    if not values:
        raise ValueError("paired bootstrap requires at least one seed")
    rng = random.Random(seed)
    draws = sorted(statistics.mean(rng.choices(values, k=len(values)))
                   for _ in range(repetitions))
    return {
        "mean": statistics.mean(values),
        "ci95_low": draws[math.floor(0.025 * repetitions)],
        "ci95_high": draws[math.ceil(0.975 * repetitions) - 1],
        "seed": seed,
        "repetitions": repetitions,
        "per_seed": values,
    }


def load_gate_trace(path: Path) -> dict:
    events = []
    epochs: dict[float, list[float]] = collections.defaultdict(list)
    with path.open(newline="") as stream:
        for row in csv.DictReader(stream):
            applied = float(row["applied_monotonic_s"])
            trace_s = float(row["time_s"])
            events.append((applied, row["provider"], bool(int(row["in_range"]))))
            epochs[trace_s].append(applied)
    if not events:
        raise ValueError(f"empty applied gate trace: {path}")
    return {
        "events": sorted(events),
        "transition_windows": [
            (min(values), max(values)) for values in epochs.values()
            if len(values) == len(PROVIDERS)
        ],
    }


def gate_state_at(trace: dict, published_s: float) -> dict[str, bool] | None:
    if any(start <= published_s <= end
           for start, end in trace["transition_windows"]):
        return None
    state = {}
    for applied_s, provider, available in trace["events"]:
        if applied_s > published_s:
            break
        state[provider] = available
    return state if set(state) == set(PROVIDERS) else None


def classify(state: dict[str, bool] | None, initial_provider: str) -> str:
    if state is None:
        return "TRANSITION_BOUNDARY"
    if not any(state.values()):
        return "NONE_REACHABLE"
    if state[initial_provider]:
        return "INITIAL_REACHABLE"
    return "SWITCH_REQUIRED"


def load_summary(cell: Path) -> dict:
    summaries = json.loads((cell / "summary.json").read_text())["summaries"]
    if len(summaries) != 1:
        raise ValueError(f"expected one terminal summary in {cell}")
    return summaries[0]


def parse_target_order(cell: Path) -> list[str]:
    command = str(json.loads((cell / "runtime-commands.json").read_text())["client"])
    order = [match.group(1) for match in TARGET_RE.finditer(command)]
    if order != list(PROVIDERS):
        raise ValueError(f"unexpected gRPC endpoint order in {cell}: {order}")
    return order


def parse_ndnsf(cell: Path) -> dict[int, dict]:
    text = (cell / "ndnsf-user.log").read_text(errors="replace")
    requests = {
        int(match.group(1)): {
            "request_id": match.group(2),
            "success": match.group(3) == "SUCCESS",
            "terminal_status": match.group(3),
            "latency_ms": float(match.group(4)),
            "published_monotonic_s": float(match.group(5)) / 1000.0,
            "attempts_or_executions": 0,
            "selected_provider": None,
            "failed_attempt_time_ms": None,
        }
        for match in NDNSF_RESULT_RE.finditer(text)
    }
    by_id = {item["request_id"]: item for item in requests.values()}
    execution_providers: dict[str, set[str]] = collections.defaultdict(set)
    for path in cell.glob("ndnsf-provider-*.log"):
        for match in NDNSF_STATE_RE.finditer(path.read_text(errors="replace")):
            state, label, request_id = int(match.group(1)), match.group(2), match.group(3)
            if request_id not in by_id:
                continue
            provider = LABELS[label]
            if state == 1 and by_id[request_id]["selected_provider"] is None:
                by_id[request_id]["selected_provider"] = provider
            elif state == 3:
                execution_providers[request_id].add(provider)
    for request_id, item in by_id.items():
        item["attempts_or_executions"] = len(execution_providers[request_id])
    return requests


def parse_grpc(cell: Path) -> dict[int, dict]:
    text = (cell / "grpc-client.log").read_text(errors="replace")
    published = {int(m.group(1)): float(m.group(2))
                 for m in GRPC_PUBLISHED_RE.finditer(text)}
    attempts: dict[int, list[dict]] = collections.defaultdict(list)
    for match in GRPC_ATTEMPT_RE.finditer(text):
        attempts[int(match.group(1))].append({
            "attempt": int(match.group(2)), "provider": match.group(3),
            "status": match.group(4), "latency_ms": float(match.group(5))})
    requests = {}
    for request_id, values in attempts.items():
        ordered = sorted(values, key=lambda item: item["attempt"])
        winner = next((item for item in ordered if item["status"] == "OK"), None)
        before = ordered if winner is None else ordered[:ordered.index(winner)]
        requests[request_id] = {
            "success": winner is not None,
            "terminal_status": "SUCCESS" if winner else "FAILURE",
            "latency_ms": sum(item["latency_ms"] for item in ordered),
            "published_monotonic_s": published.get(request_id),
            "attempts_or_executions": len(ordered),
            "selected_provider": winner["provider"] if winner else None,
            "failed_attempt_time_ms": sum(item["latency_ms"] for item in before),
        }
    return requests


def parse_nsc(cell: Path) -> dict[int, dict]:
    text = (cell / "nsc-consumer.log").read_text(errors="replace")
    return {
        int(match.group(1)) - 1: {
            "success": match.group(2) == "SUCCESS",
            "terminal_status": match.group(2),
            "latency_ms": float(match.group(3)),
            "published_monotonic_s": float(match.group(6)) / 1000.0,
            "attempts_or_executions": int(match.group(4)),
            "selected_provider": match.group(5).rsplit("/", 1)[-1],
            "failed_attempt_time_ms": None,
        }
        for match in NSC_RESULT_RE.finditer(text)
    }


def analyze(root: Path, output: Path) -> dict:
    rows = []
    per_seed = []
    seed_dirs = sorted(root.glob("seed-*"), key=lambda p: int(p.name.split("-")[1]))
    if [int(path.name.split("-")[1]) for path in seed_dirs] != list(range(72, 82)):
        raise ValueError("holdout must contain exactly seeds 72--81")
    for seed_dir in seed_dirs:
        seed = int(seed_dir.name.split("-")[1])
        condition = seed_dir / "range-100-speed-2p0"
        cells = {system: condition / system for system in SYSTEMS}
        parsers = {"ndnsf": parse_ndnsf, "grpc": parse_grpc, "nsc": parse_nsc}
        requests = {system: parsers[system](cell) for system, cell in cells.items()}
        traces = {system: load_gate_trace(cell / "mobility_trace.csv")
                  for system, cell in cells.items()}
        summaries = {system: load_summary(cell) for system, cell in cells.items()}
        order = parse_target_order(cells["grpc"])
        for system in SYSTEMS:
            if set(requests[system]) != set(range(300)):
                raise ValueError(f"seed {seed} {system} request markers do not cover 0..299")
            if sum(item["success"] for item in requests[system].values()) != int(
                    summaries[system]["success"]):
                raise ValueError(f"seed {seed} {system} success reconciliation failed")

        switch_indexes = []
        disagreement = 0
        transition = 0
        for index in range(300):
            initial = order[index % len(order)]
            states = {system: gate_state_at(
                traces[system], requests[system][index]["published_monotonic_s"])
                for system in SYSTEMS}
            classes = {system: classify(states[system], initial) for system in SYSTEMS}
            if "TRANSITION_BOUNDARY" in classes.values():
                paired_class = "TRANSITION_BOUNDARY"
                transition += 1
            elif len(set(classes.values())) != 1:
                paired_class = "STATE_DISAGREEMENT"
                disagreement += 1
            else:
                paired_class = next(iter(classes.values()))
                if paired_class == "SWITCH_REQUIRED":
                    switch_indexes.append(index)
            for system in SYSTEMS:
                state = states[system]
                rows.append({
                    "seed": seed, "logical_request_id": index, "system": system,
                    "initial_provider": initial,
                    "publication_monotonic_s": requests[system][index]["published_monotonic_s"],
                    "applied_gate_state": "" if state is None else ";".join(
                        name for name in PROVIDERS if state[name]),
                    "system_opportunity_state": classes[system],
                    "paired_opportunity_state": paired_class,
                    **{key: value for key, value in requests[system][index].items()
                       if key != "published_monotonic_s"},
                })

        seed_result = {
            "seed": seed, "switch_required": len(switch_indexes),
            "transition_boundary": transition, "state_disagreement": disagreement,
        }
        for system in SYSTEMS:
            selected = [requests[system][index] for index in switch_indexes]
            successful = [item["latency_ms"] for item in selected if item["success"]]
            seed_result[system] = {
                "success": sum(item["success"] for item in selected),
                "requests": len(selected),
                "p95_success_latency_ms": nearest_rank(successful, 0.95),
                "mean_success_latency_ms": statistics.mean(successful) if successful else None,
                "attempts_or_executions": sum(
                    item["attempts_or_executions"] for item in selected),
            }
        per_seed.append(seed_result)

    paired = {}
    for baseline in ("grpc", "nsc"):
        differences = [
            item["ndnsf"]["p95_success_latency_ms"] -
            item[baseline]["p95_success_latency_ms"]
            for item in per_seed
            if item["switch_required"] > 0 and
            item["ndnsf"]["p95_success_latency_ms"] is not None and
            item[baseline]["p95_success_latency_ms"] is not None
        ]
        paired[baseline] = paired_bootstrap_mean(differences)
    complete = len(per_seed) == 10 and all(
        item["switch_required"] > 0 for item in per_seed)
    confirmed = complete and all(value["ci95_high"] < 0 for value in paired.values())
    report = {
        "schema": "spec171-opportunity-holdout-v1",
        "seeds": list(range(72, 82)), "systems": list(SYSTEMS),
        "per_seed": per_seed, "paired_ndnsf_minus_baseline_p95_ms": paired,
        "switch_required_requests": sum(item["switch_required"] for item in per_seed),
        "transition_boundary_requests": sum(item["transition_boundary"] for item in per_seed),
        "state_disagreement_requests": sum(item["state_disagreement"] for item in per_seed),
        "verdict": ("HOLDOUT_CONFIRMS_CONDITIONAL_END_TO_END_ADVANTAGE"
                    if confirmed else "HOLDOUT_DOES_NOT_CONFIRM_CONDITIONAL_ADVANTAGE"),
    }
    output.mkdir(parents=True, exist_ok=True)
    (output / "holdout-summary.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n")
    with (output / "holdout-requests.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader(); writer.writerows(rows)
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    report = analyze(args.root.resolve(), args.output.resolve())
    print(json.dumps({
        "verdict": report["verdict"],
        "switch_required_requests": report["switch_required_requests"],
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
