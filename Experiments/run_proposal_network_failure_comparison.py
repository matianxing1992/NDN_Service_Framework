#!/usr/bin/env python3
"""Run the matched MiniNDN network-loss comparison used by proposal slide 30."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import os
import random
import re
import signal
import statistics
import subprocess
import sys
import time
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
SYSTEMS = ("ndnsf", "nsc", "grpc")
SAFE_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
NDNSF_V01_COMMIT = "be9421f2db8499ba8a42ad1aed2cb42c3f0d81b6"
NDNSF_V01_TREE = "c9cc0c51ba4a74b855ed9eec84f5459260b12ee1"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def capture(command: list[str], cwd: Path) -> str:
    try:
        return subprocess.check_output(
            command, cwd=cwd, text=True, stderr=subprocess.STDOUT).strip()
    except Exception as exc:
        return f"unavailable: {exc}"


def git_identity(root: Path) -> dict:
    return {
        "commit": capture(["git", "rev-parse", "HEAD"], root),
        "tree": capture(["git", "rev-parse", "HEAD^{tree}"], root),
        "status_short": capture(["git", "status", "--short"], root),
    }


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = int(fraction * (len(ordered) - 1))
    return ordered[index]


def parse_key_values(line: str) -> dict[str, float | int]:
    result: dict[str, float | int] = {}
    for key, value in re.findall(r"([A-Za-z0-9_]+)=([0-9.]+)", line):
        result[key] = float(value) if "." in value else int(value)
    return result


def require_route(path: Path, prefix: str, cost: int = 37) -> None:
    if not path.exists():
        raise RuntimeError(f"route evidence is missing: {path}")
    lines = path.read_text(errors="replace").splitlines()
    if not any(prefix in line and f"cost={cost}" in line for line in lines):
        raise RuntimeError(
            f"route evidence does not contain {prefix} at cost {cost}: {path}")


def parse_ndnsf(cell_dir: Path, expected: int, warmup: int,
                deadline_ms: int, service_delay_ms: int) -> dict:
    request_path = cell_dir / "requests.csv"
    if not request_path.exists():
        raise RuntimeError("NDNSF requests.csv is missing")
    with request_path.open(newline="") as source:
        rows = list(csv.DictReader(source))
    required = expected + warmup
    if len(rows) != required:
        raise RuntimeError(
            f"NDNSF terminal row count {len(rows)} != required {required}")
    if any(not row.get("request_id") for row in rows):
        raise RuntimeError("NDNSF request_id is missing")
    request_ids = [row["request_id"] for row in rows]
    if len(set(request_ids)) != len(request_ids):
        raise RuntimeError("NDNSF request_id is duplicated")
    measured = sorted(rows, key=lambda row: row["request_id"])[warmup:warmup + expected]
    latencies = []
    late_after_deadline = 0
    for row in measured:
        if str(row.get("success", "0")) != "1":
            continue
        try:
            latency = float(row["latency_ms"])
        except (KeyError, TypeError, ValueError) as exc:
            raise RuntimeError(
                f"NDNSF successful request lacks numeric latency: {row['request_id']}") from exc
        if latency <= deadline_ms:
            latencies.append(latency)
        else:
            late_after_deadline += 1
    report = json.loads((cell_dir / "summary.json").read_text())
    raw_success = sum(str(row.get("success", "0")) == "1" for row in rows)
    if int(report["total_requests_sent"]) != required:
        raise RuntimeError("NDNSF native sent count does not match the contract")
    if int(report["total_successful_responses"]) != raw_success:
        raise RuntimeError("NDNSF CSV and native success counts disagree")
    if int(report["request_timeout_ms"]) != deadline_ms:
        raise RuntimeError("NDNSF native deadline does not match the contract")
    provider_log = (cell_dir / "provider-A.log").read_text(errors="replace")
    if f"serviceDelayMs={service_delay_ms}" not in provider_log:
        raise RuntimeError("NDNSF Provider service delay does not match the contract")
    require_route(
        cell_dir / "nlsr-convergence-after-wait-memphis-fib-list.txt",
        "/example/hello/provider/A")
    require_route(
        cell_dir / "nlsr-convergence-after-wait-ucla-fib-list.txt",
        "/example/hello/user")
    return {
        "sent": expected,
        "success": len(latencies),
        "failure": expected - len(latencies),
        "success_rate": len(latencies) / expected,
        "p50_latency_ms": percentile(latencies, 0.50),
        "p95_latency_ms": percentile(latencies, 0.95),
        "successful_latency_count": len(latencies),
        "late_after_deadline": late_after_deadline,
        "native_summary": str(cell_dir / "summary.json"),
        "warmup_excluded": warmup,
    }


def parse_nsc(cell_dir: Path, expected: int, deadline_ms: int,
              service_delay_ms: int, duration: int, warmup: int) -> dict:
    report = json.loads((cell_dir / "summary.json").read_text())
    summary = report["summaries"][0]
    sent = int(summary["count"])
    if sent != expected:
        raise RuntimeError(f"NSC count {sent} != expected {expected}")
    if int(report["request_deadline_ms"]) != deadline_ms or \
       int(summary["deadline_ms"]) != deadline_ms:
        raise RuntimeError("NSC native deadline does not match the contract")
    if int(report["service_delay_ms"]) != service_delay_ms:
        raise RuntimeError("NSC service delay does not match the contract")
    if float(report["duration_s"]) != float(duration) or \
       float(report["warmup_s"]) != float(warmup):
        raise RuntimeError("NSC duration or warmup does not match the contract")
    require_route(cell_dir / "memphis-fib-list.txt", "/muas/ucla")
    require_route(cell_dir / "ucla-fib-list.txt", "/muas/memphis")
    success = int(summary["success"])
    return {
        "sent": sent,
        "success": success,
        "failure": sent - success,
        "success_rate": success / sent,
        "p50_latency_ms": float(summary["p50_ms"]),
        "p95_latency_ms": float(summary["p95_ms"]),
        "successful_latency_count": success,
        "interest_timeout_callbacks": int(summary.get("timeout", 0)),
        "late_after_deadline": int(summary.get("late_after_deadline", 0)),
        "native_summary": str(cell_dir / "summary.json"),
    }


def parse_grpc(cell_dir: Path, expected: int, deadline_ms: int,
               service_delay_ms: int, duration: int, warmup: int) -> dict:
    rate_line = ""
    summary_line = ""
    for line in (cell_dir / "client.log").read_text(errors="replace").splitlines():
        if line.startswith("GRPC_CLIENT_RATE"):
            rate_line = line
        elif line.startswith("GRPC_CLIENT_SUMMARY"):
            summary_line = line
    if not rate_line or not summary_line:
        raise RuntimeError("gRPC rate or summary marker is missing")
    rate = parse_key_values(rate_line)
    latency = parse_key_values(summary_line)
    sent = int(rate["sent"])
    success = int(rate["success"])
    if sent != expected:
        raise RuntimeError(f"gRPC sent {sent} != expected {expected}")
    if int(latency["count"]) != success:
        raise RuntimeError("gRPC rate and latency success counts disagree")
    report = json.loads((cell_dir / "summary.json").read_text())
    if float(report["timeout_s"]) * 1000.0 != float(deadline_ms):
        raise RuntimeError("gRPC native deadline does not match the contract")
    if int(report["service_delay_ms"]) != service_delay_ms:
        raise RuntimeError("gRPC service delay does not match the contract")
    if float(report["duration_s"]) != float(duration) or \
       float(report["warmup_s"]) != float(warmup):
        raise RuntimeError("gRPC duration or warmup does not match the contract")
    if report["path"] != ["memphis", "csu", "ucla"] or \
       float(report["one_way_link_delay_ms"]) != 37.0:
        raise RuntimeError("gRPC path evidence does not match Memphis-CSU-UCLA")
    return {
        "sent": sent,
        "success": success,
        "failure": sent - success,
        "success_rate": success / sent,
        "p50_latency_ms": float(latency["p50_ms"]),
        "p95_latency_ms": float(latency["p95_ms"]),
        "successful_latency_count": success,
        "native_summary": str(cell_dir / "summary.json"),
    }


def build_command(system: str, topology: Path, cell_dir: Path,
                  ndnsf_root: Path, rate: float, duration: int,
                  warmup: int, deadline_ms: int, service_delay_ms: int) -> list[str]:
    expected = int(round(rate * duration))
    if system == "ndnsf":
        total_duration = duration + warmup
        return [
            sys.executable, str(ndnsf_root / "Experiments/NDNSF_NewAPI_Minindn_Perf.py"),
            "--topology-file", str(topology),
            "--user-node", "memphis", "--controller-node", "csu",
            "--provider-nodes", "ucla", "--providers", "1",
            "--workload-mode", "open-loop", "--rate-rps", str(rate),
            "--duration", str(total_duration), "--warmup", "0",
            "--request-timeout-ms", str(deadline_ms),
            "--timeout-ms", str(deadline_ms), "--ack-timeout-ms", "1000",
            "--drain-seconds", "10", "--strategy", "first-responding",
            "--service-delay-ms", str(service_delay_ms),
            "--max-outstanding", "512",
            "--max-requests", str(expected + int(round(rate * warmup))),
            "--handler-threads", "0", "--performance-mode",
            "--hybrid-message-crypto", "--nfd-log-level", "WARN",
            "--skip-post-run-diagnostics", "--output-dir", str(cell_dir),
        ]
    if system == "nsc":
        return [
            sys.executable, str(REPO / "Experiments/NSC_memphis_ucla_latency.py"),
            "--topology-file", str(topology), "--client-node", "memphis",
            "--server-node", "ucla", "--service-delay-ms", str(service_delay_ms),
            "--rate-series", str(rate), "--duration-s", str(duration),
            "--warmup-s", str(warmup), "--request-deadline-ms", str(deadline_ms),
            "--failure-probability", "0", "--epoch-ms", "10000", "--seed", "100",
            "--output-dir", str(cell_dir),
        ]
    return [
        sys.executable, str(REPO / "Experiments/gRPC_memphis_ucla_latency.py"),
        "--topology-file", str(topology), "--client-node", "memphis",
        "--server-node", "ucla", "--delay-ms", str(service_delay_ms),
        "--count", str(expected), "--rate-rps", str(rate),
        "--duration-s", str(duration), "--timeout-s", str(deadline_ms / 1000.0),
        "--warmup-s", str(warmup), "--server-workers", "32",
        "--failure-probability", "0", "--epoch-ms", "10000", "--seed", "100",
        "--output-dir", str(cell_dir),
    ]


def conflicting_processes() -> list[str]:
    output = capture(["ps", "-eo", "pid=,cmd="], REPO)
    patterns = (
        "NDNSF_NewAPI_Minindn_Perf.py", "NSC_memphis_ucla_latency.py",
        "gRPC_memphis_ucla_latency.py", "App_ServiceController", "App_User",
        "App_Provider", "/NDN_NSC/consumer", "/NDN_NSC/producer",
        "greeter_client.py", "greeter_server.py", "nfd --config", "nlsr -f",
    )
    return [line for line in output.splitlines()
            if any(pattern in line for pattern in patterns)]


def run_cell(command: list[str], cwd: Path, cell_dir: Path, timeout_s: int) -> dict:
    cell_dir.mkdir(parents=True, exist_ok=False)
    started = dt.datetime.now(dt.timezone.utc)
    started_monotonic = time.monotonic()
    command_record = {
        "command": command,
        "cwd": str(cwd),
        "started_utc": started.isoformat(),
    }
    (cell_dir / "cell-command.json").write_text(
        json.dumps(command_record, indent=2, sort_keys=True) + "\n")
    env = os.environ.copy()
    env["PATH"] = SAFE_PATH
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    timed_out = False
    with (cell_dir / "driver.log").open("w") as log:
        process = subprocess.Popen(
            command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT,
            text=True, start_new_session=True)
        next_progress = time.monotonic() + 30
        deadline = time.monotonic() + timeout_s
        while process.poll() is None:
            now = time.monotonic()
            if now >= deadline:
                timed_out = True
                os.killpg(process.pid, signal.SIGINT)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                break
            if now >= next_progress:
                elapsed = int(now - started_monotonic)
                print(f"CELL_PROGRESS pid={process.pid} elapsed_s={elapsed}", flush=True)
                next_progress = now + 30
            time.sleep(5)
        return_code = process.returncode
    ended = dt.datetime.now(dt.timezone.utc)
    return {
        **command_record,
        "ended_utc": ended.isoformat(),
        "elapsed_s": (ended - started).total_seconds(),
        "exit_code": return_code,
        "timed_out": timed_out,
    }


def bootstrap_ci(values: list[float], seed: int = 163) -> tuple[float, float]:
    if len(values) == 1:
        return values[0], values[0]
    rng = random.Random(seed)
    samples = []
    for _ in range(10000):
        draw = [rng.choice(values) for _ in values]
        samples.append(statistics.fmean(draw))
    samples.sort()
    return samples[249], samples[9749]


def write_aggregate(output_dir: Path, results: list[dict], systems: list[str],
                    losses: list[int], repetitions: int, deadline_ms: int) -> dict:
    rows = []
    for system in systems:
        for loss in losses:
            cells = [item for item in results
                     if item["system"] == system and item["loss_percent"] == loss]
            if len(cells) != repetitions or not all(item["valid"] for item in cells):
                raise RuntimeError(f"incomplete cell set for {system} loss={loss}")
            rates = [float(item["metrics"]["success_rate"]) for item in cells]
            successes = sum(int(item["metrics"]["success"]) for item in cells)
            sent = sum(int(item["metrics"]["sent"]) for item in cells)
            low, high = bootstrap_ci(rates, seed=163 + loss + len(system))
            p95s = [float(item["metrics"]["p95_latency_ms"])
                    for item in cells if item["metrics"]["p95_latency_ms"] is not None]
            rows.append({
                "system": system,
                "loss_percent": loss,
                "repetitions": repetitions,
                "sent": sent,
                "success": successes,
                "failure": sent - successes,
                "pooled_success_rate_pct": successes * 100.0 / sent,
                "mean_success_rate_pct": statistics.fmean(rates) * 100.0,
                "sample_sd_pct": statistics.stdev(rates) * 100.0 if len(rates) > 1 else 0.0,
                "bootstrap_ci95_low_pct": low * 100.0,
                "bootstrap_ci95_high_pct": high * 100.0,
                "median_cell_p95_latency_ms": statistics.median(p95s) if p95s else None,
            })
    aggregate = {
        "status": "complete",
        "primary_metric": f"completion within {deadline_ms} ms",
        "rows": rows,
    }
    (output_dir / "aggregate.json").write_text(
        json.dumps(aggregate, indent=2, sort_keys=True) + "\n")
    with (output_dir / "aggregate.csv").open("w", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    return aggregate


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ndnsf-root", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--systems", default=",".join(SYSTEMS))
    parser.add_argument("--losses", default="1,3,10")
    parser.add_argument("--cell-timeout", type=int, default=300)
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()

    if os.geteuid() != 0:
        raise SystemExit("run under sudo so MiniNDN can create namespaces")
    systems = [item for item in args.systems.split(",") if item]
    if any(item not in SYSTEMS for item in systems):
        raise SystemExit("unknown system")
    losses = [int(item) for item in args.losses.split(",") if item]
    if args.smoke:
        losses = [losses[0]]
        repetitions = 1
        rate, duration, warmup = 2.0, 3, 1
    else:
        repetitions = args.repetitions
        rate, duration, warmup = 10.0, 60, 5
    expected = int(round(rate * duration))
    deadline_ms = 5000
    service_delay_ms = 5
    output_dir = args.output_dir.resolve()
    if output_dir.exists():
        raise SystemExit(f"output directory already exists; cell replacement is forbidden: {output_dir}")
    output_dir.mkdir(parents=True, exist_ok=False)
    manifest_path = output_dir / "campaign-manifest.json"

    topologies = {
        loss: (REPO / f"Experiments/Topology/testbed(loss={loss}%).conf").resolve()
        for loss in losses
    }
    for topology in topologies.values():
        if not topology.exists():
            raise SystemExit(f"missing topology: {topology}")
    normalized_topologies = []
    for loss, topology in topologies.items():
        text = topology.read_text()
        configured_losses = [float(value) for value in re.findall(
            r"\bloss=([0-9]+(?:\.[0-9]+)?)", text)]
        if not configured_losses or any(value != float(loss) for value in configured_losses):
            raise SystemExit(f"topology loss does not match {loss}%: {topology}")
        normalized_topologies.append(re.sub(
            r"\bloss=[0-9]+(?:\.[0-9]+)?", "loss=<MATCHED>", text).strip())
    if len(set(normalized_topologies)) != 1:
        raise SystemExit("topologies differ in fields other than loss")
    binaries = [
        args.ndnsf_root / "build/examples/App_ServiceController",
        args.ndnsf_root / "build/examples/App_Provider",
        args.ndnsf_root / "build/examples/App_User",
        REPO / "Experiments/NDN_NSC/consumer",
        REPO / "Experiments/NDN_NSC/producer",
    ]
    for binary in binaries:
        if not binary.exists():
            raise SystemExit(f"missing binary: {binary}")
    conflicts = conflicting_processes()
    if conflicts:
        raise SystemExit("MiniNDN single-writer conflict:\n" + "\n".join(conflicts))

    relevant = [
        "Experiments/gRPC/greeter_client.py",
        "Experiments/gRPC_memphis_ucla_latency.py",
        "Experiments/NSC_memphis_ucla_latency.py",
        "Experiments/NDN_NSC/consumer.cpp",
        "Experiments/NDN_NSC/producer.cpp",
    ]
    current_diff = subprocess.check_output(
        ["git", "diff", "--binary", "--", *relevant], cwd=REPO)
    ndnsf_diff = subprocess.check_output(
        ["git", "diff", "--binary", "--",
        "Experiments/NDNSF_NewAPI_Minindn_Perf.py", "examples/App_Provider.cpp"],
        cwd=args.ndnsf_root)
    ndnsf_identity = git_identity(args.ndnsf_root)
    if ndnsf_identity["commit"] != NDNSF_V01_COMMIT or \
       ndnsf_identity["tree"] != NDNSF_V01_TREE:
        raise SystemExit(
            "NDNSF subject is not the exact May-20 v0.1 commit/tree: "
            f"{ndnsf_identity['commit']} {ndnsf_identity['tree']}")
    comparison_patch_path = output_dir / "comparison-fixture.patch"
    ndnsf_patch_path = output_dir / "ndnsf-v0.1-fixture.patch"
    comparison_patch_path.write_bytes(current_diff)
    ndnsf_patch_path.write_bytes(ndnsf_diff)
    source_paths = [
        Path(__file__).resolve(),
        REPO / "Experiments/gRPC/greeter_client.py",
        REPO / "Experiments/gRPC/greeter_server.py",
        REPO / "Experiments/gRPC_memphis_ucla_latency.py",
        REPO / "Experiments/NSC_memphis_ucla_latency.py",
        REPO / "Experiments/NDN_NSC/consumer.cpp",
        REPO / "Experiments/NDN_NSC/producer.cpp",
        args.ndnsf_root / "Experiments/NDNSF_NewAPI_Minindn_Perf.py",
        args.ndnsf_root / "examples/App_Provider.cpp",
    ]
    manifest = {
        "schema": 1,
        "status": "running",
        "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "mode": "smoke" if args.smoke else "formal",
        "contract": {
            "path": ["memphis", "csu", "ucla"],
            "rate_rps": rate,
            "measurement_duration_s": duration,
            "warmup_s": warmup,
            "measured_requests_per_cell": expected,
            "request_deadline_ms": deadline_ms,
            "service_delay_ms": service_delay_ms,
            "application_retries": 0,
            "transport_recovery": "native per system",
            "repetitions": repetitions,
            "loss_percentages": losses,
            "systems": systems,
            "cell_replacement": "forbidden",
        },
        "subjects": {
            "ndnsf_v0_1": ndnsf_identity,
            "comparison_repo": git_identity(REPO),
            "comparison_patch": {
                "path": str(comparison_patch_path),
                "sha256": sha256_bytes(current_diff),
            },
            "ndnsf_fixture_patch": {
                "path": str(ndnsf_patch_path),
                "sha256": sha256_bytes(ndnsf_diff),
            },
        },
        "sources": {str(path): sha256_file(path) for path in source_paths},
        "topologies": {str(loss): {"path": str(path), "sha256": sha256_file(path)}
                       for loss, path in topologies.items()},
        "binaries": {str(path): sha256_file(path) for path in binaries},
        "dependencies": {
            "python": sys.version,
            "minindn": capture([sys.executable, "-c",
                                "import minindn; print(getattr(minindn, '__version__', 'unknown'))"], REPO),
            "grpcio": capture([sys.executable, "-c", "import grpc; print(grpc.__version__)"], REPO),
            "ndn_cxx": capture(["pkg-config", "--modversion", "libndn-cxx"], REPO),
        },
        "cells": [],
        "limitations": [
            "MiniNDN reports a dummy ndn-cxx keychain patch; this is a loss/reliability comparison, not a cryptographic-overhead comparison.",
            "gRPC uses HTTP/2 over TCP, while NSC and NDNSF use NDN; native transport recovery semantics are intentionally retained.",
            "NSC signature-verification stubs are not security-equivalent to NDNSF.",
            "Kernel netem loss randomness is not externally seeded; independent repetitions and raw cell evidence are retained.",
        ],
    }
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")

    results: list[dict] = []
    for repetition in range(1, repetitions + 1):
        loss_order = losses[(repetition - 1) % len(losses):] + losses[:(repetition - 1) % len(losses)]
        system_order = systems[(repetition - 1) % len(systems):] + systems[:(repetition - 1) % len(systems)]
        for loss in loss_order:
            for system in system_order:
                cell_dir = output_dir / f"rep{repetition:02d}" / f"loss{loss:02d}" / system
                result_path = cell_dir / "cell-result.json"
                if result_path.exists():
                    result = json.loads(result_path.read_text())
                    results.append(result)
                    continue
                conflicts = conflicting_processes()
                if conflicts:
                    raise RuntimeError("single-writer conflict before cell:\n" + "\n".join(conflicts))
                command = build_command(
                    system, topologies[loss], cell_dir, args.ndnsf_root.resolve(),
                    rate, duration, warmup, deadline_ms, service_delay_ms)
                print(f"CELL_START rep={repetition} loss={loss} system={system}", flush=True)
                execution = run_cell(
                    command, args.ndnsf_root if system == "ndnsf" else REPO,
                    cell_dir, args.cell_timeout)
                result = {
                    "repetition": repetition,
                    "loss_percent": loss,
                    "system": system,
                    "execution": execution,
                    "valid": False,
                }
                if execution["exit_code"] == 0 and not execution["timed_out"]:
                    try:
                        if system == "ndnsf":
                            metrics = parse_ndnsf(
                                cell_dir, expected, int(round(rate * warmup)),
                                deadline_ms, service_delay_ms)
                        elif system == "nsc":
                            metrics = parse_nsc(
                                cell_dir, expected, deadline_ms, service_delay_ms,
                                duration, warmup)
                        else:
                            metrics = parse_grpc(
                                cell_dir, expected, deadline_ms, service_delay_ms,
                                duration, warmup)
                        result["metrics"] = metrics
                        result["valid"] = True
                    except Exception as exc:
                        result["parse_error"] = str(exc)
                result_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
                results.append(result)
                manifest["cells"].append({
                    "path": str(result_path), "valid": result["valid"],
                    "repetition": repetition, "loss_percent": loss, "system": system,
                })
                manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
                print(f"CELL_END rep={repetition} loss={loss} system={system} valid={result['valid']}", flush=True)

    if not all(item["valid"] for item in results):
        manifest["status"] = "incomplete"
        manifest["ended_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        return 2
    write_aggregate(
        output_dir, results, systems, losses, repetitions, deadline_ms)
    manifest["status"] = "complete"
    manifest["ended_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    if args.smoke:
        print("PROPOSAL_NETWORK_COMPARISON_SMOKE_OK", flush=True)
    else:
        print("PROPOSAL_NETWORK_COMPARISON_FORMAL_OK", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
