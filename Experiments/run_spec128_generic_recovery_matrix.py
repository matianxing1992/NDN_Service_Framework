#!/usr/bin/env python3
"""Freeze, execute once, and analyze the 16-cell Spec 128 confirmation."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any, Iterable

from scipy.stats import beta


ROOT = Path(__file__).resolve().parents[1]
EXAMPLES = ROOT / "examples/python/live_stream"
sys.path.insert(0, str(EXAMPLES))
from workload_common import TrafficUtilityReport, build_workload_manifest

CELL_LAUNCHER = ROOT / "Experiments/NDNSF_LiveStream_Generality_Minindn.py"
SPEC127_EVIDENCE = tuple(sorted((ROOT / "results").glob("spec127-*")))
FROZEN_SOURCES = tuple(Path(value) for value in (
    "ndn-service-framework/Stream.hpp",
    "ndn-service-framework/Stream.cpp",
    "pythonWrapper/src/ndnsf/_ndnsf.cpp",
    "pythonWrapper/ndnsf/streaming.py",
    "examples/python/live_stream/workload_common.py",
    "examples/python/live_stream/workload_provider.py",
    "examples/python/live_stream/workload_consumer.py",
    "Experiments/NDNSF_LiveStream_Generality_Minindn.py",
    "Experiments/run_spec128_generic_recovery_matrix.py",
))
METRIC_SCHEMA = (
    "initialPayloadInterests", "retryPayloadInterests",
    "initialFuturePayloadInterests", "retryFuturePayloadInterests",
    "providerInitialFutureInterests", "providerInitialFutureHits",
    "providerRetryFutureInterests", "providerRetryFutureHits",
    "retrySuccesses", "retryExhaustions", "retrySuppressions",
    "retrySuppressionReasons",
    "timeouts", "nacks", "declaredRecoveryCapacity", "recoveryAttempts",
    "recoveredSources", "recoveryExhaustions", "terminalSkips",
    "mappingDataResponses", "mappingNewDataResponses", "mappingNewDataRatio",
    "payloadInterests", "payloadInterestOverheadRatio",
    "providerFutureInterests", "providerFutureHits", "providerFutureHitRatio",
    "completeSamples", "measurementCoverage", "tailMaximumStallMs",
    "publicationToDeliveryMs", "duplicates", "partialSamples",
    "outOfOrderSamples", "invalidItems",
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def frozen_cells() -> list[dict[str, Any]]:
    cells: list[dict[str, Any]] = []
    for workload, scheme, recovery_capacity in (
            ("periodic-sensor", "none", 0),
            ("variable-multisegment", "gf256-two-repair", 2)):
        profiles = (("zero-loss", 1), ("multi-loss-retry", 5),
                    ("capacity-plus-one", 2))
        for profile, repetitions in profiles:
            for repetition in range(1, repetitions + 1):
                if profile == "zero-loss":
                    loss, delay, jitter, reorder = 0.0, 1.0, 0.0, 0.0
                elif profile == "multi-loss-retry":
                    loss, delay, jitter, reorder = 1.0, 20.0, 10.0, 25.0
                else:
                    # A deliberately severe, frozen safety boundary. Exact
                    # capacity arithmetic is covered deterministically in Core;
                    # these cells verify fail-closed behavior and later progress.
                    loss, delay, jitter, reorder = 10.0, 20.0, 10.0, 25.0
                cells.append({
                    "id": f"{workload}-{profile}-run-{repetition:02d}",
                    "workloadId": workload, "networkProfile": profile,
                    "repetition": repetition, "recoveryScheme": scheme,
                    "declaredRecoveryCapacity": recovery_capacity,
                    "lossPercent": loss, "delayMs": delay, "jitterMs": jitter,
                    "reorderPercent": reorder,
                    "reorderCorrelationPercent": 50.0 if reorder else 0.0,
                    "reorderGap": 5 if reorder else 0,
                    "maximumAttemptsPerCursor": 3,
                    "automaticRetry": False, "rerunAllowed": False,
                })
    return cells


def digest_path(path: Path) -> str:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except PermissionError:
        completed = subprocess.run(
            ["sudo", "-n", "sha256sum", "--", str(path)], text=True,
            capture_output=True, check=True)
        return completed.stdout.split()[0]


def hash_files(paths: Iterable[Path]) -> dict[str, str]:
    result = {}
    for path in paths:
        if not path.is_file():
            raise RuntimeError(f"missing frozen source: {path}")
        result[str(path.relative_to(ROOT))] = digest_path(path)
    return result


def hash_spec127_evidence() -> dict[str, str]:
    if not SPEC127_EVIDENCE:
        raise RuntimeError("missing immutable Spec 127 evidence")
    result = {}
    for evidence in SPEC127_EVIDENCE:
        if not evidence.is_dir():
            continue
        for path in sorted(value for value in evidence.rglob("*") if value.is_file()):
            result[str(path.relative_to(ROOT))] = digest_path(path)
    return result


def workload_hashes() -> dict[str, str]:
    return {name: build_workload_manifest(name).digest for name in
            ("periodic-sensor", "variable-multisegment")}


def command_for(cell: dict[str, Any], output: Path) -> list[str]:
    return [
        "sudo", "-n", "-E", "timeout", "230s", sys.executable,
        str(CELL_LAUNCHER), "--campaign-label", "spec128",
        "--recovery-scheme", cell["recoveryScheme"],
        "--workload", cell["workloadId"], "--output", str(output),
        "--loss-percent", f"{cell['lossPercent']:g}",
        "--delay-ms", f"{cell['delayMs']:g}",
        "--jitter-ms", f"{cell['jitterMs']:g}",
        "--reorder-percent", f"{cell['reorderPercent']:g}",
        "--reorder-correlation-percent",
        f"{cell['reorderCorrelationPercent']:g}",
        "--reorder-gap", str(cell["reorderGap"]),
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
    if (os.geteuid() if euid is None else int(euid)) != 0:
        raise PermissionError("run the campaign as root for single-writer ownership")


def owner_processes(process_table: str, current_pid: int | None = None,
                    ignored_pids: set[int] | None = None) -> list[str]:
    ignored = set(ignored_pids or ()) | {
        os.getpid() if current_pid is None else int(current_pid)}
    tokens = ("NDNSF_LiveStream_Generality_Minindn.py",
              "run_spec128_generic_recovery_matrix.py", "minindn")
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
    pn = provider.get("nativeStatus") or {}
    cn = consumer.get("nativeStatus") or {}
    utility = TrafficUtilityReport(
        payload_interests=int(cn.get("payload_interests") or 0),
        necessary_source_repair_items=int(
            provider.get("necessarySourceRepairItems") or 0),
        mapping_interests=int(cn.get("mapping_interests") or 0),
        mapping_data_responses=int(cn.get("mapping_data_responses") or 0),
        mapping_new_data_responses=int(cn.get("mapping_new_data_responses") or 0),
        mapping_bytes=int(cn.get("mapping_bytes") or 0),
        retry_attempts=int(cn.get("retry_attempts") or 0),
        timeouts=int(cn.get("timeouts") or 0), nacks=int(cn.get("nacks") or 0),
        provider_future_interests=int(pn.get("provider_future_interests") or 0),
        provider_future_hits=int(pn.get("provider_future_hits") or 0),
        initial_payload_interests=int(cn.get("initial_payload_interests") or 0),
        retry_payload_interests=int(cn.get("retry_payload_interests") or 0),
        initial_future_payload_interests=int(
            cn.get("initial_future_payload_interests") or 0),
        retry_future_payload_interests=int(
            cn.get("retry_future_payload_interests") or 0),
        retry_successes=int(cn.get("retry_successes") or 0),
        retry_exhaustions=int(cn.get("retry_exhaustions") or 0),
        retry_suppressions=int(cn.get("retry_suppressions") or 0),
        declared_recovery_capacity=int(cn.get("declared_recovery_capacity") or 0),
        recovery_attempts=int(cn.get("recovery_attempts") or 0),
        recovered_sources=int(cn.get("recovered") or 0),
        recovery_exhaustions=int(cn.get("recovery_exhaustions") or 0),
        provider_initial_future_interests=int(
            pn.get("provider_initial_future_interests") or 0),
        provider_initial_future_hits=int(pn.get("provider_initial_future_hits") or 0),
        provider_retry_future_interests=int(
            pn.get("provider_retry_future_interests") or 0),
        provider_retry_future_hits=int(pn.get("provider_retry_future_hits") or 0),
    ).to_dict()
    expected = int(consumer.get("expectedMeasuredSamples") or 600)
    complete = int(consumer.get("completeMeasuredSamples") or 0)
    receipts = [value for value in consumer.get("receipts", [])
                if value.get("phase") == "measured"]
    times = [int(value["completed_timestamp_us"]) / 1000.0 for value in receipts]
    tail = [value for value in times if times and value >= times[-1] - 10000.0]
    stall = max((b - a for a, b in zip(tail, tail[1:])), default=None)
    latencies = [float(value) for value in
                 consumer.get("measuredPublicationToDeliveryMs", [])]
    p95, p99 = percentile(latencies, .95), percentile(latencies, .99)
    skips = sum(int(value) for value in
                (consumer.get("skipsByReason") or {}).values())
    capacity = cell["networkProfile"] == "capacity-plus-one"
    qdisc = raw.get("effectiveQdiscBeforeApps") or {}
    qdisc_proof = all("netem" in json.dumps(qdisc.get(node, {}))
                      for node in ("provider", "consumer"))
    common = {
        "qdiscProof": qdisc_proof,
        "noDuplicatePartialOutOfOrder": all(
            int(consumer.get(key) or 0) == 0 for key in
            ("duplicates", "partialSamples", "outOfOrderSamples", "invalidItems")),
        "declaredCapacity": utility["declaredRecoveryCapacity"] ==
            cell["declaredRecoveryCapacity"],
    }
    if capacity:
        checks = {**common,
            "providerCompleted": raw.get("providerReturnCode") == 0,
            "terminalSkipObserved": skips > 0,
            "laterProgress": complete > 0,
            "failClosed": int(consumer.get("invalidItems") or 0) == 0,
        }
    else:
        workload = cell["workloadId"]
        impaired = cell["networkProfile"] != "zero-loss"
        initial_den = utility["providerInitialFutureInterests"]
        initial_ratio = (utility["providerInitialFutureHits"] / initial_den
                         if initial_den else None)
        checks = {**common,
            "processAndApplication": return_code == 0 and raw.get("passed") is True,
            "completeCoverage": complete / expected >=
                (.999 if workload == "periodic-sensor" else .99),
            "tailContinuity": stall is not None and stall <=
                (200.0 if workload == "periodic-sensor" else 1000.0),
            "latencyP95": p95 is not None and p95 <=
                (200.0 if workload == "periodic-sensor" else 250.0),
            "payloadOverhead": utility["payloadInterestOverheadRatio"] is not None
                and utility["payloadInterestOverheadRatio"] <=
                (.25 if impaired else .15),
            "mappingNovelty": utility["mappingNewDataRatio"] is not None
                and utility["mappingNewDataRatio"] >= .90,
            "futureHitRatio": (utility["providerFutureHitRatio"] is not None and
                utility["providerFutureHitRatio"] >=
                (.95 if impaired or workload == "variable-multisegment" else .99)),
            "initialFutureHitRatio": impaired or
                (initial_ratio is not None and initial_ratio >=
                 (.95 if workload == "variable-multisegment" else .99)),
            "boundedRetry": utility["retryPayloadInterests"] <=
                2 * utility["initialPayloadInterests"],
        }
        if workload == "variable-multisegment":
            checks["latencyP99"] = p99 is not None and p99 <= 500.0
    result = {
        "schemaVersion": "spec128-generic-recovery-run-v1", "cell": cell,
        "trafficCounterScope": "full-run-including-warmup",
        "returnCode": return_code, "automaticRetry": False,
        "rerunAllowed": False, "expectedSamples": expected,
        "completeSamples": complete,
        "measurementCoverage": complete / expected if expected else None,
        "duplicates": int(consumer.get("duplicates") or 0),
        "partialSamples": int(consumer.get("partialSamples") or 0),
        "outOfOrderSamples": int(consumer.get("outOfOrderSamples") or 0),
        "invalidItems": int(consumer.get("invalidItems") or 0),
        "terminalSkips": skips, "skipsByReason": consumer.get("skipsByReason") or {},
        "retrySuppressionReasons": consumer.get("retrySuppressionReasons") or {},
        "tailMaximumStallMs": stall,
        "publicationToDeliveryMs": {"samples": len(latencies),
            "p50": percentile(latencies, .50), "p95": p95, "p99": p99},
        **utility, "checks": checks, "accepted": all(checks.values()),
    }
    result["unavailableMetrics"] = [name for name in METRIC_SCHEMA
                                    if name not in result or result[name] is None]
    return result


def exact_interval(successes: int, count: int) -> list[float] | None:
    if count == 0:
        return None
    return [0.0 if successes == 0 else float(beta.ppf(
        .025, successes, count - successes + 1)),
        1.0 if successes == count else float(beta.ppf(
        .975, successes + 1, count - successes))]


def aggregate(runs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result = []
    for workload in ("periodic-sensor", "variable-multisegment"):
        for profile, required in (("zero-loss", 1), ("multi-loss-retry", 4),
                                  ("capacity-plus-one", 2)):
            group = [run for run in runs if run["cell"]["workloadId"] == workload
                     and run["cell"]["networkProfile"] == profile]
            successes = sum(bool(run["accepted"]) for run in group)
            result.append({"workloadId": workload, "networkProfile": profile,
                "runCount": len(group), "acceptedRuns": successes,
                "requiredAcceptedRuns": required,
                "clopperPearson95": exact_interval(successes, len(group))
                    if profile == "multi-loss-retry" else None,
                "passed": successes >= required})
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--duration-seconds", type=int, default=60)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.duration_seconds != 60:
        raise SystemExit("Spec 128 cells require exactly 60 measured seconds")
    root = args.output_root.resolve()
    if "spec127" in root.name.lower():
        raise SystemExit("Spec 127 destinations are immutable")
    if root.exists():
        raise SystemExit(f"refusing existing Spec 128 output: {root}")
    if not args.dry_run:
        try:
            validate_live_invoker()
        except PermissionError as error:
            raise SystemExit(str(error))
        if shutil.disk_usage(ROOT).free < 8 * 1024 ** 3:
            raise SystemExit("less than 8 GiB free for frozen campaign")
    cells = frozen_cells()
    root.mkdir(parents=True)
    sources_before = hash_files(ROOT / value for value in FROZEN_SOURCES)
    history_before = hash_spec127_evidence()
    workloads = workload_hashes()
    entries = []
    for cell in cells:
        output = root / cell["id"]
        frozen = dict(cell, outputPath=str(output))
        entries.append({"cell": frozen, "command": command_for(cell, output),
                        "invocationCount": 0})
    for name, value in (
            ("campaign-manifest.json", entries),
            ("source-hashes-before.json", sources_before),
            ("spec127-hashes-before.json", history_before),
            ("workload-hashes.json", workloads),
            ("metrics-schema.json", {"fields": METRIC_SCHEMA})):
        (root / name).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                                 encoding="utf-8")
    if args.dry_run:
        summary = {"status": "DRY_RUN", "runCount": 16, "runs": entries,
            "sourceHashes": sources_before, "spec127Hashes": history_before,
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
            entry["invocationCount"] += 1
            (root / "invocation-ledger.json").write_text(
                json.dumps(entries, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            started = utc_now()
            cell_id = entry["cell"]["id"]
            stdout_path = root / f"{cell_id}.launcher.stdout"
            stderr_path = root / f"{cell_id}.launcher.stderr"
            with stdout_path.open("w", encoding="utf-8") as stdout, \
                    stderr_path.open("w", encoding="utf-8") as stderr:
                completed = subprocess.run(entry["command"], cwd=ROOT,
                                           stdout=stdout, stderr=stderr, check=False)
            output = Path(entry["cell"]["outputPath"])
            raw_path = output / "summary.json"
            raw = json.loads(raw_path.read_text(encoding="utf-8")) \
                if raw_path.exists() else {"passed": False, "error": "missing-summary"}
            run = analyze_cell_summary(entry["cell"], raw, completed.returncode)
            run.update({"startedAt": started, "endedAt": utc_now(),
                        "command": entry["command"], "invocationCount": 1,
                        "launcherStdout": str(stdout_path),
                        "launcherStderr": str(stderr_path)})
            output.mkdir(parents=True, exist_ok=True)
            (output / "run-summary.json").write_text(
                json.dumps(run, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            runs.append(run)
            with (root / "campaign-runs.csv").open("w", newline="",
                                                     encoding="utf-8") as output_csv:
                fields = ("cellId", "workloadId", "networkProfile", "repetition",
                          "returnCode", "completeSamples", "initialPayloadInterests",
                          "retryPayloadInterests", "mappingDataResponses",
                          "mappingNewDataResponses", "timeouts", "nacks",
                          "providerInitialFutureInterests", "providerInitialFutureHits",
                          "providerRetryFutureInterests", "providerRetryFutureHits",
                          "recoveryAttempts", "recoveredSources", "terminalSkips",
                          "accepted")
                writer = csv.DictWriter(output_csv, fieldnames=fields)
                writer.writeheader()
                for value in runs:
                    writer.writerow({"cellId": value["cell"]["id"],
                        "workloadId": value["cell"]["workloadId"],
                        "networkProfile": value["cell"]["networkProfile"],
                        "repetition": value["cell"]["repetition"],
                        **{key: value.get(key) for key in fields[4:]}})
    finally:
        lock.unlink(missing_ok=True)
    sources_after = hash_files(ROOT / value for value in FROZEN_SOURCES)
    history_after = hash_spec127_evidence()
    treatments = aggregate(runs)
    passed = len(runs) == 16 and sources_before == sources_after and \
        history_before == history_after and all(value["passed"] for value in treatments)
    summary = {"schemaVersion": "spec128-generic-recovery-campaign-v1",
        "status": "PASS" if passed else "FAIL", "runCount": len(runs),
        "expectedRunCount": 16, "automaticRetry": False,
        "sourceUnchanged": sources_before == sources_after,
        "spec127EvidenceUnchanged": history_before == history_after,
        "workloadHashes": workloads, "treatments": treatments, "runs": runs}
    (root / "source-hashes-after.json").write_text(
        json.dumps(sources_after, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (root / "spec127-hashes-after.json").write_text(
        json.dumps(history_after, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (root / "campaign-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
