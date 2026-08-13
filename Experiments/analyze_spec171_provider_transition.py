#!/usr/bin/env python3
"""Analyze the registered Spec 171 Provider-transition replays."""

from __future__ import annotations

import argparse
import collections
import csv
import json
import math
from pathlib import Path
import re
import statistics


CELLS = (
    "ndnsf", "grpc-static-3", "grpc-preregistered-4",
    "nsc-static-3", "nsc-preregistered-4")
NDNSF_RESULT_RE = re.compile(
    r"INTERMITTENT_USER_REQUEST_RESULT request_index=(\d+) "
    r"requestId=(/[^ ]+) status=(\w+) latency_ms=([0-9.]+)"
    r"(?: published_monotonic_ms=([0-9.]+))?")
NDNSF_PROVIDER_D_RE = re.compile(
    r"state=4 .*?provider=/example/hello/provider/D .*?requestId=(/[^ ]+)")
GRPC_ATTEMPT_RE = re.compile(
    r"GRPC_FAILOVER_ATTEMPT request_id=(\d+) attempt=(\d+) "
    r"provider=(\w+) status=(\w+) latency_ms=([0-9.]+)")
GRPC_PUBLISHED_RE = re.compile(
    r"GRPC_REQUEST_PUBLISHED request_id=(\d+) monotonic_s=([0-9.]+)")
NSC_RESULT_RE = re.compile(
    r"NSC_REQUEST_RESULT request_id=(\d+) status=(\w+) "
    r"latency_ms=([0-9.]+) attempts=(\d+) provider=([^\s]+)"
    r"(?: published_monotonic_ms=([0-9.]+))?")
GRPC_TARGET_RE = re.compile(r"--target\s+([^=\s]+)=([^\s]+)")
NSC_PROVIDER_RE = re.compile(r"/muas/(ucla|wustl|uiuc|arizona)")
IP_RE = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}(?::\d+)?\b")
PROVIDERS = ("ucla", "wustl", "uiuc", "arizona")


def nearest_rank(values: list[float], probability: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, math.ceil(probability * len(ordered)) - 1)
    return ordered[min(index, len(ordered) - 1)]


def phase_for(index: int, rate_rps: float = 5.0, start_s: float = 4.0) -> str:
    trace_s = start_s + index / rate_rps
    # A trace epoch changes four independent provider gates sequentially.  A
    # request scheduled at the exact epoch can therefore observe a mixed state
    # and must not be credited to either adjacent steady-state window.
    if math.isclose(trace_s, 20.0, abs_tol=1e-9) or math.isclose(
            trace_s, 40.0, abs_tol=1e-9):
        return "transition_boundary"
    if trace_s < 20.0:
        return "initial"
    if trace_s < 40.0:
        return "overlap"
    return "post_retirement"


def load_gate_transitions(cell: Path) -> list[tuple[float, str, bool]]:
    transitions = []
    with (cell / "mobility_trace.csv").open(newline="") as stream:
        for row in csv.DictReader(stream):
            transitions.append((
                float(row["applied_monotonic_s"]),
                row["provider"],
                bool(int(row["in_range"])),
            ))
    return sorted(transitions)


def phase_for_actual(
        published_monotonic_s: float,
        transitions: list[tuple[float, str, bool]]) -> str:
    state: dict[str, bool] = {}
    for applied_s, provider, available in transitions:
        if applied_s > published_monotonic_s:
            break
        state[provider] = available
    if set(state) != set(PROVIDERS):
        return "transition_boundary"
    first_three = all(state[name] for name in PROVIDERS[:3])
    provider_d = state["arizona"]
    if first_three and not provider_d:
        return "initial"
    if first_three and provider_d:
        return "overlap"
    if not any(state[name] for name in PROVIDERS[:3]) and provider_d:
        return "post_retirement"
    return "transition_boundary"


def parse_ndnsf(cell: Path) -> dict[int, dict]:
    text = (cell / "ndnsf-user.log").read_text(errors="replace")
    requests = {
        int(match.group(1)): {
            "request_id": match.group(2),
            "success": match.group(3) == "SUCCESS",
            "latency_ms": float(match.group(4)),
            "attempts": 1,
            "provider_d": False,
            "published_monotonic_s": (
                float(match.group(5)) / 1000.0 if match.group(5) else None),
        }
        for match in NDNSF_RESULT_RE.finditer(text)
    }
    provider_d_text = (cell / "ndnsf-provider-D.log").read_text(errors="replace")
    provider_d_ids = set(NDNSF_PROVIDER_D_RE.findall(provider_d_text))
    for request in requests.values():
        request["provider_d"] = request["request_id"] in provider_d_ids
    return requests


def parse_grpc(cell: Path) -> dict[int, dict]:
    attempts: dict[int, list[dict]] = collections.defaultdict(list)
    text = (cell / "grpc-client.log").read_text(errors="replace")
    published = {
        int(match.group(1)): float(match.group(2))
        for match in GRPC_PUBLISHED_RE.finditer(text)
    }
    for match in GRPC_ATTEMPT_RE.finditer(text):
        attempts[int(match.group(1))].append({
            "attempt": int(match.group(2)),
            "provider": match.group(3),
            "status": match.group(4),
            "latency_ms": float(match.group(5)),
        })
    requests = {}
    for index, values in attempts.items():
        ordered = sorted(values, key=lambda value: value["attempt"])
        winner = next((value for value in ordered if value["status"] == "OK"), None)
        requests[index] = {
            "success": winner is not None,
            "latency_ms": sum(value["latency_ms"] for value in ordered),
            "attempts": len(ordered),
            "provider_d": bool(winner and winner["provider"] == "arizona"),
            "published_monotonic_s": published.get(index),
        }
    return requests


def parse_nsc(cell: Path) -> dict[int, dict]:
    text = (cell / "nsc-consumer.log").read_text(errors="replace")
    requests = {}
    for match in NSC_RESULT_RE.finditer(text):
        index = int(match.group(1)) - 1
        requests[index] = {
            "success": match.group(2) == "SUCCESS",
            "latency_ms": float(match.group(3)),
            "attempts": int(match.group(4)),
            "provider_d": match.group(5).endswith("/arizona"),
            "published_monotonic_s": (
                float(match.group(6)) / 1000.0 if match.group(6) else None),
        }
    return requests


def client_command_and_count(cell: Path, label: str) -> tuple[str, int]:
    runtime = json.loads((cell / "runtime-commands.json").read_text())
    command = str(runtime.get("user") or runtime.get("client") or "")
    if label == "ndnsf":
        forbidden = bool(
            IP_RE.search(command) or "--target" in command or
            "/example/hello/provider/" in command)
        return command, -1 if forbidden else 0
    if label.startswith("grpc"):
        return command, len(GRPC_TARGET_RE.findall(command))
    return command, len(set(NSC_PROVIDER_RE.findall(command)))


def summarize_requests(
        requests: dict[int, dict],
        transitions: list[tuple[float, str, bool]] | None = None) -> dict:
    if set(requests) != set(range(300)):
        missing = sorted(set(range(300)) - set(requests))
        raise ValueError(f"request markers do not cover 0..299; missing={missing[:10]}")
    phases = {}
    request_phases = {
        index: (
            phase_for_actual(request["published_monotonic_s"], transitions)
            if transitions and request.get("published_monotonic_s") is not None
            else phase_for(index))
        for index, request in requests.items()
    }
    for phase in ("initial", "transition_boundary", "overlap", "post_retirement"):
        selected = [
            request for index, request in requests.items()
            if request_phases[index] == phase]
        successful_latencies = [
            request["latency_ms"] for request in selected if request["success"]]
        phases[phase] = {
            "requests": len(selected),
            "success": sum(request["success"] for request in selected),
            "success_rate": (
                sum(request["success"] for request in selected) / len(selected)),
            "mean_success_latency_ms": (
                statistics.mean(successful_latencies)
                if successful_latencies else None),
            "p95_success_latency_ms": nearest_rank(successful_latencies, 0.95),
            "attempts_or_executions": sum(request["attempts"] for request in selected),
            "provider_d_successes": sum(
                request["success"] and request["provider_d"] for request in selected),
        }
    return phases


def analyze_replay(replay_root: Path) -> dict:
    parsers = {
        "ndnsf": parse_ndnsf,
        "grpc-static-3": parse_grpc,
        "grpc-preregistered-4": parse_grpc,
        "nsc-static-3": parse_nsc,
        "nsc-preregistered-4": parse_nsc,
    }
    result = {"replay": int(replay_root.name.split("-", 1)[1]), "cells": {}}
    for label in CELLS:
        cell = replay_root / label
        requests = parsers[label](cell)
        transitions = load_gate_transitions(cell)
        actual_phase_assignment = all(
            request.get("published_monotonic_s") is not None
            for request in requests.values())
        command, configured = client_command_and_count(cell, label)
        summary = json.loads((cell / "summary.json").read_text())["summaries"][0]
        success = int(summary["success"])
        if success != sum(request["success"] for request in requests.values()):
            raise ValueError(f"{label} marker/summary success mismatch")
        result["cells"][label] = {
            "configured_provider_count": configured,
            "client_command": command,
            "phase_assignment": (
                "actual_gate_state" if actual_phase_assignment else
                "nominal_index_with_transition_boundary_excluded"),
            "phases": summarize_requests(
                requests, transitions if actual_phase_assignment else None),
        }
    cells = result["cells"]
    result["passes"] = {
        "ndnsf_zero_provider_configuration": (
            cells["ndnsf"]["configured_provider_count"] == 0),
        "ndnsf_post_success_ge_95pct": (
            cells["ndnsf"]["phases"]["post_retirement"]["success_rate"] >= 0.95),
        "ndnsf_post_uses_provider_d": (
            cells["ndnsf"]["phases"]["post_retirement"]["provider_d_successes"] > 0),
        "grpc_static_post_fails_without_d": (
            cells["grpc-static-3"]["phases"]["post_retirement"]["success"] == 0 and
            cells["grpc-static-3"]["phases"]["post_retirement"]["provider_d_successes"] == 0),
        "nsc_static_post_fails_without_d": (
            cells["nsc-static-3"]["phases"]["post_retirement"]["success"] == 0 and
            cells["nsc-static-3"]["phases"]["post_retirement"]["provider_d_successes"] == 0),
        "grpc_preregistered_post_success_ge_95pct": (
            cells["grpc-preregistered-4"]["phases"]["post_retirement"]["success_rate"] >= 0.95),
        "nsc_preregistered_post_success_ge_95pct": (
            cells["nsc-preregistered-4"]["phases"]["post_retirement"]["success_rate"] >= 0.95),
    }
    result["passed"] = all(result["passes"].values())
    return result


def analyze(root: Path, output: Path) -> dict:
    replays = [analyze_replay(path) for path in sorted(root.glob("replay-*"))
               if (path / "COMPLETE").is_file()]
    if not replays:
        raise ValueError(f"no complete replays under {root}")
    sc014 = len(replays) >= 3 and all(replay["passed"] for replay in replays)
    report = {
        "schema": "spec171-provider-transition-analysis-v1",
        "replays": replays,
        "completed_replays": len(replays),
        "pilot_passed": replays[0]["passed"],
        "sc014_passed": sc014,
        "verdict": (
            "PROVIDER_DISCOVERY_EVIDENCE_PASSED" if sc014 else
            "PILOT_PASSED_REPETITIONS_REQUIRED" if replays[0]["passed"] else
            "PROVIDER_DISCOVERY_EVIDENCE_FAILED"),
    }
    output.mkdir(parents=True, exist_ok=True)
    (output / "transition-summary.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n")
    with (output / "transition-windows.csv").open("w", newline="") as stream:
        fields = [
            "replay", "cell", "configured_provider_count", "phase", "requests",
            "success", "success_rate", "mean_success_latency_ms",
            "p95_success_latency_ms", "attempts_or_executions",
            "provider_d_successes"]
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for replay in replays:
            for label, cell in replay["cells"].items():
                for phase, values in cell["phases"].items():
                    writer.writerow({
                        "replay": replay["replay"], "cell": label,
                        "configured_provider_count": cell["configured_provider_count"],
                        "phase": phase, **values})
    post = replays[0]["cells"]
    lines = [
        "# Spec 171 Provider-transition result",
        "",
        f"- Verdict: `{report['verdict']}`",
        f"- Completed independent process replays: {len(replays)}",
        "- Scope: client-side discovery of an already authorized and routed Provider.",
        "- gRPC/NSC can obtain equivalent behavior through an external resolver or configuration update.",
        "",
        "## Replay 1 steady post-retirement window",
        "",
        "| Cell | Configured Providers | Success | Provider-D successes | Mean successful latency | p95 successful latency |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for label in CELLS:
        cell = post[label]
        values = cell["phases"]["post_retirement"]
        mean = "--" if values["mean_success_latency_ms"] is None else f"{values['mean_success_latency_ms']:.2f} ms"
        p95 = "--" if values["p95_success_latency_ms"] is None else f"{values['p95_success_latency_ms']:.2f} ms"
        lines.append(
            f"| {label} | {cell['configured_provider_count']} | "
            f"{values['success']}/{values['requests']} | "
            f"{values['provider_d_successes']} | {mean} | {p95} |")
    (output / "README.md").write_text("\n".join(lines) + "\n")
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    report = analyze(args.root.resolve(), args.output.resolve())
    print(json.dumps({
        "verdict": report["verdict"],
        "completed_replays": report["completed_replays"],
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
