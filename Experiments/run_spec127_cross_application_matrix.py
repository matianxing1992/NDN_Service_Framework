#!/usr/bin/env python3
"""Freeze, execute once, and analyze the Spec 127 two-workload matrix."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any, Iterable

from scipy.stats import beta


ROOT = Path(__file__).resolve().parents[1]
EXAMPLES = ROOT / "examples/python/live_stream"
sys.path.insert(0, str(EXAMPLES))
from workload_common import TrafficUtilityReport, build_workload_manifest

CELL_LAUNCHER = ROOT / "Experiments/NDNSF_LiveStream_Generality_Minindn.py"
HISTORICAL_EVIDENCE = (
    ROOT / "results/spec125-adaptive-sample-atomic-20260719-confirm06",
    ROOT / "results/spec126-loss-reorder-20260720-confirmation07",
)
FROZEN_SOURCES = tuple(Path(value) for value in (
    "ndn-service-framework/Stream.hpp",
    "ndn-service-framework/Stream.cpp",
    "pythonWrapper/src/ndnsf/_ndnsf.cpp",
    "pythonWrapper/ndnsf/streaming.py",
    "examples/python/live_stream/workload_common.py",
    "examples/python/live_stream/workload_provider.py",
    "examples/python/live_stream/workload_consumer.py",
    "Experiments/NDNSF_LiveStream_Generality_Minindn.py",
    "Experiments/run_spec127_cross_application_matrix.py",
))


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def frozen_cells() -> list[dict[str, Any]]:
    cells = []
    for workload in ("periodic-sensor", "variable-multisegment"):
        profiles = (("zero-loss", 1), ("combined", 5))
        for profile, repetitions in profiles:
            for repetition in range(1, repetitions + 1):
                combined = profile == "combined"
                cells.append({
                    "id": f"{workload}-{profile}-run-{repetition:02d}",
                    "workloadId": workload, "networkProfile": profile,
                    "repetition": repetition,
                    "lossPercent": 1.0 if combined else 0.0,
                    "delayMs": 20.0 if combined else 1.0,
                    "jitterMs": 10.0 if combined else 0.0,
                    "reorderPercent": 25.0 if combined else 0.0,
                    "reorderCorrelationPercent": 50.0 if combined else 0.0,
                    "reorderGap": 5 if combined else 0,
                    "automaticRetry": False, "rerunAllowed": False,
                })
    return cells


def hash_files(paths: Iterable[Path]) -> dict[str, str]:
    result = {}
    for path in paths:
        if not path.is_file():
            raise RuntimeError(f"missing frozen source: {path}")
        result[str(path.relative_to(ROOT))] = digest_path(path)
    return result


def digest_path(path: Path) -> str:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except PermissionError:
        completed = subprocess.run(
            ["sudo", "-n", "sha256sum", "--", str(path)], text=True,
            capture_output=True, check=True)
        return completed.stdout.split()[0]


def hash_evidence(roots: Iterable[Path] = HISTORICAL_EVIDENCE) -> dict[str, str]:
    result = {}
    for root in roots:
        if not root.is_dir():
            raise RuntimeError(f"missing historical evidence: {root}")
        for path in sorted(value for value in root.rglob("*") if value.is_file()):
            result[str(path.relative_to(ROOT))] = digest_path(path)
    return result


def workload_hashes() -> dict[str, str]:
    return {name: build_workload_manifest(name).digest for name in
            ("periodic-sensor", "variable-multisegment")}


def command_for(cell: dict[str, Any], output: Path) -> list[str]:
    return [
        "sudo", "-n", "-E", "timeout", "230s", sys.executable,
        str(CELL_LAUNCHER), "--workload", cell["workloadId"],
        "--output", str(output), "--loss-percent", f"{cell['lossPercent']:g}",
        "--delay-ms", f"{cell['delayMs']:g}", "--jitter-ms",
        f"{cell['jitterMs']:g}", "--reorder-percent",
        f"{cell['reorderPercent']:g}", "--reorder-correlation-percent",
        f"{cell['reorderCorrelationPercent']:g}", "--reorder-gap",
        str(cell["reorderGap"]),
    ]


def process_ancestry(pid: int | None = None) -> set[int]:
    current = os.getpid() if pid is None else int(pid)
    result = set()
    while current > 1 and current not in result:
        result.add(current)
        try:
            fields = Path(f"/proc/{current}/stat").read_text(
                encoding="utf-8").split()
            current = int(fields[3])
        except (FileNotFoundError, IndexError, ValueError):
            break
    return result


def validate_live_invoker(euid: int | None = None) -> None:
    effective = os.geteuid() if euid is None else int(euid)
    if effective != 0:
        raise PermissionError(
            "run the campaign as root so one owner can write every cell artifact")


def owner_processes(process_table: str, current_pid: int | None = None,
                    ignored_pids: set[int] | None = None) -> list[str]:
    pid = os.getpid() if current_pid is None else current_pid
    ignored = set(ignored_pids or ()) | {pid}
    tokens = ("NDNSF_LiveStream_Generality_Minindn.py",
              "run_spec127_cross_application_matrix.py", "minindn")
    result = []
    for line in process_table.splitlines():
        fields = line.split(None, 1)
        if fields and fields[0].isdigit() and int(fields[0]) in ignored:
            continue
        if any(token in line for token in tokens):
            result.append(line)
    return result


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1,
                       max(0, int(round((len(ordered) - 1) * fraction))))]


def analyze_cell_summary(cell: dict[str, Any], raw: dict[str, Any],
                         return_code: int) -> dict[str, Any]:
    provider = raw.get("providerStatus") or {}
    consumer = raw.get("consumerStatus") or {}
    # Traffic is deliberately a full-run cohort. Boundary snapshots cannot
    # causally pair a warm-up future Interest with Data produced just inside
    # measurement; subtracting cumulative counters can therefore yield more
    # hits than Interests. The fixed 5 s warm-up is identical in every cell.
    provider_native = provider.get("nativeStatus") or {}
    consumer_native = consumer.get("nativeStatus") or {}
    utility = TrafficUtilityReport(
        payload_interests=int(consumer_native.get("payload_interests") or 0),
        necessary_source_repair_items=int(
            provider.get("necessarySourceRepairItems", 0) or 0),
        mapping_interests=int(consumer_native.get("mapping_interests") or 0),
        mapping_data_responses=int(
            consumer_native.get("mapping_data_responses") or 0),
        mapping_new_data_responses=int(
            consumer_native.get("mapping_new_data_responses") or 0),
        mapping_bytes=int(consumer_native.get("mapping_bytes") or 0),
        retry_attempts=int(consumer_native.get("retry_attempts") or 0),
        timeouts=int(consumer_native.get("timeouts") or 0),
        nacks=int(consumer_native.get("nacks") or 0),
        provider_future_interests=int(
            provider_native.get("provider_future_interests") or 0),
        provider_future_hits=int(provider_native.get("provider_future_hits") or 0),
    ).to_dict()
    latencies = [float(value) for value in
                 consumer.get("measuredPublicationToDeliveryMs",
                              consumer.get("publicationToDeliveryMs", []))]
    expected = int(consumer.get("expectedMeasuredSamples") or 600)
    complete = int(consumer.get("completeMeasuredSamples") or 0)
    receipts = [value for value in consumer.get("receipts", [])
                if value.get("phase") == "measured"]
    times = [int(value["completed_timestamp_us"]) / 1000.0 for value in receipts]
    tail = [value for value in times if times and value >= times[-1] - 10000.0]
    maximum_stall = max((right - left for left, right in zip(tail, tail[1:])),
                        default=None)
    impaired = cell["networkProfile"] == "combined"
    workload = cell["workloadId"]
    future_ratio = utility["providerFutureHitRatio"]
    overhead = utility["payloadInterestOverheadRatio"]
    mapping_ratio = utility["mappingNewDataRatio"]
    p95, p99 = percentile(latencies, .95), percentile(latencies, .99)
    continuity_limit = 200.0 if workload == "periodic-sensor" else 1000.0
    checks = {
        "processAndApplication": return_code == 0 and raw.get("passed") is True,
        "completeCoverage": complete / expected >= (
            .999 if workload == "periodic-sensor" else .99),
        "noDuplicatePartialOutOfOrder": all(int(consumer.get(key) or 0) == 0
            for key in ("duplicates", "partialSamples", "outOfOrderSamples")),
        "tailContinuity": maximum_stall is not None and
            maximum_stall <= continuity_limit,
        "latencyP95": p95 is not None and p95 <= (
            200.0 if workload == "periodic-sensor" else 250.0),
        "latencyP99": workload == "periodic-sensor" or
            (p99 is not None and p99 <= 500.0),
        "payloadOverhead": overhead is not None and overhead <=
            (.25 if impaired else .15),
        "mappingNovelty": mapping_ratio is not None and mapping_ratio >= .90,
        "futureInterestObserved": utility["providerFutureInterests"] > 0,
        "futureHitRatio": future_ratio is not None and future_ratio >=
            (.95 if impaired else .99),
    }
    return {
        "schemaVersion": "spec127-generality-run-v1", "cell": cell,
        "trafficCounterScope": "full-run-including-warmup",
        "returnCode": return_code, "automaticRetry": False,
        "rerunAllowed": False, "expectedSamples": expected,
        "completeSamples": complete,
        "duplicates": int(consumer.get("duplicates") or 0),
        "partialSamples": int(consumer.get("partialSamples") or 0),
        "outOfOrderSamples": int(consumer.get("outOfOrderSamples") or 0),
        "recoveredSamples": int(consumer.get("recoveredSamples") or 0),
        "skipsByReason": consumer.get("skipsByReason") or {},
        "tailMaximumStallMs": maximum_stall,
        "measurementCoverage": complete / expected if expected else None,
        "publicationToDeliveryMs": {"samples": len(latencies),
            "p50": percentile(latencies, .50), "p95": p95, "p99": p99},
        **utility, "checks": checks, "accepted": all(checks.values()),
    }


def exact_interval(successes: int, count: int) -> list[float]:
    alpha = .05
    return [0.0 if successes == 0 else float(beta.ppf(
        alpha / 2, successes, count - successes + 1)),
        1.0 if successes == count else float(beta.ppf(
            1 - alpha / 2, successes + 1, count - successes))]


def aggregate(runs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result = []
    for workload in ("periodic-sensor", "variable-multisegment"):
        for profile, required in (("zero-loss", 1), ("combined", 4)):
            group = [run for run in runs if run["cell"]["workloadId"] == workload
                     and run["cell"]["networkProfile"] == profile]
            successes = sum(bool(run["accepted"]) for run in group)
            result.append({"workloadId": workload, "networkProfile": profile,
                "runCount": len(group), "acceptedRuns": successes,
                "requiredAcceptedRuns": required,
                "clopperPearson95": exact_interval(successes, len(group))
                if group else None, "passed": successes >= required})
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--duration-seconds", type=int, default=60)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.duration_seconds != 60:
        raise SystemExit("Spec 127 cells require exactly 60 measured seconds")
    root = args.output_root.resolve()
    if root.exists():
        raise SystemExit(f"refusing existing Spec 127 output: {root}")
    if not args.dry_run:
        try:
            validate_live_invoker()
        except PermissionError as error:
            raise SystemExit(str(error))
    root.mkdir(parents=True)
    cells = frozen_cells()
    sources_before = hash_files(ROOT / value for value in FROZEN_SOURCES)
    history_before = hash_evidence()
    workloads = workload_hashes()
    entries = []
    for cell in cells:
        output = root / cell["id"]
        frozen = dict(cell)
        frozen["outputPath"] = str(output)
        entries.append({"cell": frozen, "command": command_for(cell, output)})
    (root / "campaign-manifest.json").write_text(
        json.dumps(entries, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (root / "source-hashes-before.json").write_text(
        json.dumps(sources_before, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (root / "history-hashes-before.json").write_text(
        json.dumps(history_before, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (root / "workload-hashes.json").write_text(
        json.dumps(workloads, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.dry_run:
        summary = {"status": "DRY_RUN", "runCount": 12, "runs": entries,
                   "sourceHashes": sources_before, "historyHashes": history_before,
                   "workloadHashes": workloads, "automaticRetry": False}
        (root / "campaign-summary.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return 0
    table = subprocess.run(
        ["ps", "-eo", "pid,ppid,stat,etimes,user,cmd"], text=True,
        capture_output=True, check=True).stdout
    owners = owner_processes(table, ignored_pids=process_ancestry())
    if owners:
        raise SystemExit("concurrent MiniNDN/campaign owner:\n" + "\n".join(owners))
    lock = root / ".campaign.lock"
    descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
    os.write(descriptor, f"pid={os.getpid()} started={utc_now()}\n".encode())
    os.close(descriptor)
    runs = []
    try:
        for entry in entries:
            started = utc_now()
            cell_id = entry["cell"]["id"]
            launcher_stdout = root / f"{cell_id}.launcher.stdout"
            launcher_stderr = root / f"{cell_id}.launcher.stderr"
            with launcher_stdout.open("w", encoding="utf-8") as stdout, \
                    launcher_stderr.open("w", encoding="utf-8") as stderr:
                completed = subprocess.run(
                    entry["command"], cwd=ROOT, check=False,
                    stdout=stdout, stderr=stderr)
            output = Path(entry["cell"]["outputPath"])
            raw_path = output / "summary.json"
            raw = json.loads(raw_path.read_text(encoding="utf-8")) \
                if raw_path.exists() else {"passed": False, "error": "missing-summary"}
            run = analyze_cell_summary(entry["cell"], raw, completed.returncode)
            run["startedAt"], run["endedAt"] = started, utc_now()
            run["command"] = entry["command"]
            run["launcherStdout"] = str(launcher_stdout)
            run["launcherStderr"] = str(launcher_stderr)
            output.mkdir(parents=True, exist_ok=True)
            (output / "run-summary.json").write_text(
                json.dumps(run, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            runs.append(run)
            with (root / "campaign-runs.csv").open("w", newline="", encoding="utf-8") as f:
                fields = ("cellId", "workloadId", "networkProfile", "repetition",
                          "returnCode", "completeSamples", "payloadInterests",
                          "mappingInterests", "mappingDataResponses",
                          "mappingNewDataResponses", "retryAttempts", "timeouts",
                          "nacks", "providerFutureInterests", "providerFutureHits",
                          "accepted")
                writer = csv.DictWriter(f, fieldnames=fields)
                writer.writeheader()
                for value in runs:
                    writer.writerow({"cellId": value["cell"]["id"],
                        "workloadId": value["cell"]["workloadId"],
                        "networkProfile": value["cell"]["networkProfile"],
                        "repetition": value["cell"]["repetition"],
                        **{key: value.get(key) for key in fields[4:]}})
    finally:
        lock.unlink(missing_ok=True)
    sources_after, history_after = hash_files(
        ROOT / value for value in FROZEN_SOURCES), hash_evidence()
    treatments = aggregate(runs)
    passed = len(runs) == 12 and sources_before == sources_after and \
        history_before == history_after and all(value["passed"] for value in treatments)
    summary = {"schemaVersion": "spec127-generality-campaign-v1",
        "status": "PASS" if passed else "FAIL", "runCount": len(runs),
        "expectedRunCount": 12, "automaticRetry": False,
        "sourceUnchanged": sources_before == sources_after,
        "historicalEvidenceUnchanged": history_before == history_after,
        "workloadHashes": workloads, "treatments": treatments, "runs": runs}
    (root / "campaign-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
