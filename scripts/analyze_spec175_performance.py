#!/usr/bin/env python3
"""Analyze the registered Spec175 G7 30-unit performance qualification."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import shlex
import statistics
from typing import Any, Iterable
from urllib.parse import quote


SCHEMA = "ndnsf-di-spec175-g7-analysis-v1"
SAMPLE_SCHEMA = "ndnsf-di-qwen-generation-sample-v1"
PREREQUISITE_SCHEMA = "ndnsf-di-spec175-gate-prerequisite-v1"
ROLES = (
    "/LLM/Pipeline/Stage/0",
    "/LLM/Pipeline/Stage/1",
    "/LLM/Pipeline/Stage/2",
)
BOOTSTRAP_SEED = 1750003
BOOTSTRAP_REPETITIONS = 10_000


def sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def load_json(path: Path, label: str, errors: list[str]) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        errors.append(f"{label} cannot be read: {exc}")
        return {}
    if not isinstance(value, dict):
        errors.append(f"{label} must be an object")
        return {}
    return value


def bound_file(bundle: Path, record: object, label: str,
               errors: list[str]) -> Path | None:
    if (not isinstance(record, dict) or set(record) != {"path", "sha256"}
            or not isinstance(record.get("path"), str)
            or not isinstance(record.get("sha256"), str)):
        errors.append(f"{label}: expected path/sha256 object")
        return None
    root = bundle.resolve()
    path = (root / record["path"]).resolve()
    try:
        path.relative_to(root)
    except ValueError:
        errors.append(f"{label}: path escapes sealed bundle")
        return None
    if not path.is_file() or sha256(path) != record["sha256"]:
        errors.append(f"{label}: missing or sha256 mismatch")
        return None
    return path


def finite(value: object, *, positive: bool = False) -> float | None:
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(parsed) or (parsed <= 0 if positive else parsed < 0):
        return None
    return parsed


def percentile(values: Iterable[float], fraction: float) -> float:
    ordered = sorted(float(value) for value in values)
    if not ordered:
        raise ValueError("percentile requires at least one value")
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def summary(values: Iterable[float]) -> dict[str, float | int]:
    rows = [float(value) for value in values]
    if not rows or any(not math.isfinite(value) or value < 0 for value in rows):
        raise ValueError("metric summary requires finite nonnegative values")
    return {
        "count": len(rows), "min": min(rows),
        "median": statistics.median(rows), "mean": statistics.fmean(rows),
        "p95": percentile(rows, 0.95), "p99": percentile(rows, 0.99),
        "max": max(rows),
    }


def bootstrap_median_ci(values: list[float]) -> tuple[float, float]:
    if not values:
        raise ValueError("bootstrap requires samples")
    rng = random.Random(BOOTSTRAP_SEED)
    count = len(values)
    medians = [statistics.median(
        values[rng.randrange(count)] for _ in range(count))
        for _ in range(BOOTSTRAP_REPETITIONS)]
    return percentile(medians, 0.025), percentile(medians, 0.975)


def marker_rows(text: str, marker: str) -> list[dict[str, str]]:
    rows = []
    for line in text.splitlines():
        position = line.find(marker)
        if position < 0:
            continue
        fields = {}
        for token in line[position + len(marker):].strip().split():
            if "=" in token:
                key, value = token.split("=", 1)
                fields[key] = value
        rows.append(fields)
    return rows


def read_jsonl(path: Path, label: str, errors: list[str]) -> list[dict[str, Any]]:
    rows = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        errors.append(f"{label} cannot be read: {exc}")
        return rows
    for index, line in enumerate(lines, 1):
        try:
            value = json.loads(line)
        except json.JSONDecodeError as exc:
            errors.append(f"{label}:{index} is invalid JSON: {exc}")
            continue
        if not isinstance(value, dict):
            errors.append(f"{label}:{index} is not an object")
            continue
        rows.append(value)
    return rows


def canonical_request_id(base: str, generation_id: str) -> str:
    body = base.strip()
    body = body[1:] if body.startswith("/") else body
    return "/" + quote(body, safe="-._~%") + "--sample--" + generation_id


def flag_value(path: Path, flag: str, errors: list[str]) -> str:
    try:
        argv = shlex.split(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        errors.append(f"cannot parse {path}: {exc}")
        return ""
    indexes = [index for index, token in enumerate(argv) if token == flag]
    if len(indexes) != 1 or indexes[0] + 1 >= len(argv):
        errors.append(f"{path.name}: expected one {flag}")
        return ""
    return argv[indexes[0] + 1]


def validate_prerequisites(paths: list[Path], candidate_id: str,
                           errors: list[str]) -> dict[str, dict[str, Any]]:
    expected = ("G5", "G6", "G6C")
    records: dict[str, dict[str, Any]] = {}
    identities = set()
    for gate, path in zip(expected, paths):
        value = load_json(path, f"{gate} prerequisite", errors)
        if (value.get("schemaVersion") != PREREQUISITE_SCHEMA
                or value.get("gate") != gate or value.get("status") != "PASS"
                or value.get("candidateId") != candidate_id):
            errors.append(f"{gate} prerequisite is not a candidate-bound PASS")
        identity = tuple(value.get(field) for field in (
            "sourceSealSha256", "sifSha256", "workloadSha256",
            "modelManifestSha256"))
        if any(not isinstance(item, str) or not item.startswith("sha256:")
               for item in identity):
            errors.append(f"{gate} prerequisite identity is incomplete")
        identities.add(identity)
        records[gate] = {
            "path": str(path), "sha256": sha256(path),
            "evidenceSha256": value.get("evidenceSha256"),
        }
    if len(identities) != 1:
        errors.append("G5/G6/G6C prerequisites identify different subjects")
    return records


def analyze(output_root: Path, bundle: Path,
            prerequisite_paths: list[Path]) -> dict[str, Any]:
    errors: list[str] = []
    manifest = load_json(
        bundle / "spec175-functional-manifest.json", "functional manifest", errors)
    if (manifest.get("schemaVersion")
            != "ndnsf-di-spec175-functional-bundle-v2"
            or manifest.get("gate") != "performance"):
        errors.append("functional manifest is not the frozen G7 bundle")
    candidate_id = str(manifest.get("candidateId", ""))
    prerequisites = validate_prerequisites(
        prerequisite_paths, candidate_id, errors)
    groups = manifest.get("processGroups")
    if not isinstance(groups, list) or len(groups) != 3:
        errors.append("G7 requires exactly three fresh process groups")
        groups = []

    measured: list[dict[str, Any]] = []
    cold: list[dict[str, Any]] = []
    component_values: dict[str, list[float]] = {}
    process_records = []
    prompt_values: dict[str, list[float]] = {}
    pooled_intervals: list[float] = []
    resource_samples: list[dict[str, Any]] = []
    resource_group_records: list[dict[str, Any]] = []
    seen_generation_ids: set[str] = set()

    for group_index, group in enumerate(groups):
        if not isinstance(group, dict):
            errors.append(f"process group {group_index} is malformed")
            continue
        group_id = str(group.get("id", ""))
        campaign_path = bound_file(
            bundle, group.get("generationCampaign"),
            f"{group_id} campaign", errors)
        user_path = bound_file(
            bundle, group.get("userArgs"), f"{group_id} user args", errors)
        campaign = load_json(
            campaign_path if campaign_path is not None else Path("/missing-campaign"),
            f"{group_id} campaign", errors)
        schedule = campaign.get("schedule")
        if (campaign.get("schemaVersion")
                != "ndnsf-di-qwen-generation-campaign-v2"
                or not isinstance(schedule, list) or len(schedule) != 11
                or sum(item.get("phase") == "cold" for item in schedule
                       if isinstance(item, dict)) != 1
                or sum(item.get("phase") == "measured" for item in schedule
                       if isinstance(item, dict)) != 10):
            errors.append(f"{group_id}: schedule is not one cold plus ten warm")
        prompts = {
            str(item.get("promptId")): item for item in campaign.get("prompts", [])
            if isinstance(item, dict)
        }
        group_root = output_root / "process-groups" / group_id
        rows = read_jsonl(
            group_root / "generation.jsonl", f"{group_id} generation", errors)
        if len(rows) != 11:
            errors.append(f"{group_id}: expected 11 unfiltered samples")
        base_request = flag_value(user_path, "--request-id", errors) \
            if user_path is not None else ""
        group_tps = []
        measured_request_ids: dict[str, int] = {}
        for index, row in enumerate(rows):
            scheduled = schedule[index] if index < len(schedule) else {}
            prompt_id = str(row.get("promptId", ""))
            generation_id = str(row.get("generationId", ""))
            expected = prompts.get(prompt_id, {}).get(
                "referenceGeneratedTokenIds", [])
            tokens = row.get("generatedTokenIds")
            if (row.get("schemaVersion") != SAMPLE_SCHEMA
                    or row.get("campaignId") != campaign.get("campaignId")
                    or not isinstance(scheduled, dict)
                    or row.get("phase") != scheduled.get("phase")
                    or row.get("repetition") != scheduled.get("repetition")
                    or prompt_id != scheduled.get("promptId")
                    or row.get("status") != "OK"
                    or row.get("exactReferenceMatch") is not True
                    or tokens != expected):
                errors.append(f"{group_id}: sample {index} failed exact oracle/schedule")
            steps = row.get("tokenSteps")
            metadata = (steps[0].get("metadata", {})
                        if isinstance(steps, list) and len(steps) == 1
                        and isinstance(steps[0], dict) else {})
            callback_timing = metadata.get("timingSummary", {}) \
                if isinstance(metadata, dict) else {}
            if (not isinstance(callback_timing, dict)
                    or callback_timing.get("eventCount") != len(tokens or [])
                    or callback_timing.get("attemptCount") != 1
                    or any(callback_timing.get(field) != 0 for field in (
                        "replacementCount", "staleEventCount",
                        "lineageRejectCount", "gapRejectCount",
                        "duplicateRejectCount", "callbackErrorCount"))):
                errors.append(
                    f"{group_id}: sample {index} stream retry/gap/duplicate evidence is invalid")
            if not generation_id or generation_id in seen_generation_ids:
                errors.append(f"{group_id}: generation identity is missing or reused")
            seen_generation_ids.add(generation_id)
            if row.get("phase") == "cold":
                cold.append(row)
                continue
            intervals = row.get("interTokenMs")
            ttft = finite(row.get("ttftMs"), positive=True)
            total = finite(row.get("totalMs"), positive=True)
            if (not isinstance(intervals, list) or len(intervals) < 2
                    or any(finite(value, positive=True) is None
                           for value in intervals)
                    or ttft is None or total is None):
                errors.append(f"{group_id}: measured sample {index} lacks real timing")
                continue
            interval_values = [float(value) for value in intervals]
            steady_intervals = interval_values[:-1]
            steady_tps = 1000.0 * len(steady_intervals) / sum(steady_intervals)
            invocation = {
                "processGroupId": group_id, "promptId": prompt_id,
                "generationId": generation_id, "ttftMs": ttft,
                "interTokenMs": interval_values,
                "interTokenP95Ms": percentile(interval_values, 0.95),
                "steadyStateTokensPerSecond": steady_tps,
                "totalMs": total,
            }
            measured.append(invocation)
            group_tps.append(steady_tps)
            prompt_values.setdefault(prompt_id, []).append(steady_tps)
            pooled_intervals.extend(interval_values)
            if base_request:
                measured_request_ids[canonical_request_id(
                    base_request, generation_id)] = len(tokens or [])

        provider_logs = sorted(group_root.glob("provider-*.log"))
        if len(provider_logs) != 3:
            errors.append(f"{group_id}: expected three Provider logs")
        seen_roles = set()
        for path in provider_logs:
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError as exc:
                errors.append(f"{path}: cannot read: {exc}")
                continue
            if "cpuFallback=true" in text:
                errors.append(f"{path.name}: CPU model fallback observed")
            timing_rows = marker_rows(text, "LLM_PIPELINE_QWEN_STAGE_TIMING")
            dataflow_rows = marker_rows(
                text, "LLM_PIPELINE_QWEN_EPOCH_DATAFLOW_TIMING")
            roles = {row.get("role") for row in timing_rows}
            if len(roles) != 1 or next(iter(roles), "") not in ROLES:
                errors.append(f"{path.name}: stage timing role is missing")
                continue
            role = next(iter(roles))
            seen_roles.add(role)
            relevant_timing = [row for row in timing_rows
                               if row.get("requestId") in measured_request_ids]
            relevant_dataflow = [row for row in dataflow_rows
                                 if row.get("requestId") in measured_request_ids]
            if not relevant_timing or not relevant_dataflow:
                errors.append(f"{path.name}: measured component timing is missing")
            for rows_with_fields, fields in (
                (relevant_timing, (
                    "compute_ms", "runner_total_ms", "decode_ms",
                    "serialize_ms")),
                (relevant_dataflow, (
                    "activationFetchMs", "activationPublishMs",
                    "feedbackWaitMs", "feedbackPublishMs", "samplingMs",
                    "eventPublishMs")),
            ):
                for field in fields:
                    values = [finite(row.get(field)) for row in rows_with_fields]
                    if not values or any(value is None for value in values):
                        errors.append(f"{path.name}: {field} evidence is incomplete")
                    else:
                        component_values.setdefault(
                            f"{role}:{field}", []).extend(
                                float(value) for value in values if value is not None)
            for request_id, token_count in measured_request_ids.items():
                request_timing = [row for row in relevant_timing
                                  if row.get("requestId") == request_id]
                request_dataflow = [row for row in relevant_dataflow
                                    if row.get("requestId") == request_id]
                try:
                    request_timing.sort(key=lambda row: int(row.get("epoch", "-1")))
                    request_dataflow.sort(key=lambda row: int(row.get("epoch", "-1")))
                    epochs = [int(row.get("epoch", "-1")) for row in request_timing]
                    dataflow_epochs = [int(row.get("epoch", "-1"))
                                       for row in request_dataflow]
                    prefixes = [int(row.get("prefix_token_count", "-1"))
                                for row in request_timing]
                except ValueError:
                    epochs = dataflow_epochs = prefixes = []
                if (len(request_timing) != token_count
                        or epochs != list(range(token_count))
                        or request_timing[0].get("phase") != "prefill"
                        or any(row.get("phase") != "decode"
                               for row in request_timing[1:])
                        or any(row.get("cpuFallback") not in {"0", "false"}
                               or row.get("state_storage") != "device"
                               or row.get("state_host_round_trip_bytes") != "0"
                               for row in request_timing)
                        or any(value <= 0 for value in prefixes)
                        or any(right < left for left, right in zip(
                            prefixes, prefixes[1:]))):
                    errors.append(
                        f"{path.name}: {request_id} stateful prefill/decode epochs are invalid")
                if (len(request_dataflow) != token_count
                        or dataflow_epochs != list(range(token_count))):
                    errors.append(
                        f"{path.name}: {request_id} dataflow epoch timing is invalid")
            handler = [row for row in marker_rows(
                text, "NDNSF_DI_PROVIDER_HANDLER_TIMING")
                if row.get("event") == "end"
                and row.get("session") in measured_request_ids]
            queue_values = [finite(row.get("queue_wait_ms")) for row in handler]
            if not queue_values or any(value is None for value in queue_values):
                errors.append(f"{path.name}: measured queue wait evidence is missing")
            else:
                component_values.setdefault(f"{role}:queueWaitMs", []).extend(
                    float(value) for value in queue_values if value is not None)
            cleanups = [row for row in marker_rows(
                text, "LLM_PIPELINE_QWEN_REQUEST_STATE_CLEANUP")
                if row.get("requestId") in measured_request_ids]
            cleanup_by_request = {
                row.get("requestId"): row for row in cleanups}
            if len(cleanups) != len(measured_request_ids):
                errors.append(f"{path.name}: request-state cleanup count mismatch")
            for request_id, token_count in measured_request_ids.items():
                cleanup = cleanup_by_request.get(request_id, {})
                try:
                    cleanup_valid = (
                        int(cleanup.get("generationEpochs", "-1")) == token_count
                        and int(cleanup.get("releasedStateBytes", "0")) > 0
                        and int(cleanup.get("remainingRequestStates", "-1")) == 0
                        and int(cleanup.get(
                            "managerRequestLocalEntries", "-1")) == 0)
                except ValueError:
                    cleanup_valid = False
                if not cleanup_valid:
                    errors.append(
                        f"{path.name}: {request_id} bounded cleanup is not proven")
            state_rows = [row for row in marker_rows(
                text, "LLM_PIPELINE_QWEN_STATE_RESIDENCY")
                if row.get("requestId") in measured_request_ids]
            if (not state_rows or any(row.get("storage") != "device"
                    or row.get("hostRoundTripBytes") != "0"
                    for row in state_rows)):
                errors.append(f"{path.name}: measured decode state left the GPU")
        if seen_roles != set(ROLES):
            errors.append(f"{group_id}: Provider component logs do not cover all roles")

        samples = read_jsonl(
            group_root / "resource-samples.jsonl", f"{group_id} resources", errors)
        if len(samples) < 2:
            errors.append(f"{group_id}: resource sampling is incomplete")
        if any(sample.get("gpuError") or sample.get("networkError")
               for sample in samples):
            errors.append(f"{group_id}: resource sampler reported an error")
        resource_samples.extend(samples)
        resource_record: dict[str, Any] = {
            "processGroupId": group_id, "sampleCount": len(samples)}
        if len(samples) >= 2:
            elapsed_ms = finite(
                float(samples[-1].get("monotonicMs", 0))
                - float(samples[0].get("monotonicMs", 0)), positive=True)
            cpu_delta_ns = (
                int(samples[-1].get("cpuTimeNs", 0))
                - int(samples[0].get("cpuTimeNs", 0)))
            if elapsed_ms is None or cpu_delta_ns < 0:
                errors.append(f"{group_id}: resource CPU clock is invalid")
            else:
                resource_record["elapsedMs"] = elapsed_ms
                resource_record["averageCpuCores"] = (
                    cpu_delta_ns / (elapsed_ms * 1_000_000.0))
            resource_record["networkRxByteDelta"] = max(
                0, int(samples[-1].get("networkRxBytes", 0))
                - int(samples[0].get("networkRxBytes", 0)))
            resource_record["networkTxByteDelta"] = max(
                0, int(samples[-1].get("networkTxBytes", 0))
                - int(samples[0].get("networkTxBytes", 0)))
        resource_group_records.append(resource_record)
        process_records.append({
            "processGroupId": group_id,
            "measuredCount": len(group_tps),
            "steadyStateTokensPerSecond": (
                summary(group_tps) if group_tps else None),
            "resourceSampleCount": len(samples),
        })

    if len(cold) != 3 or len(measured) != 30:
        errors.append("G7 requires exactly 3 excluded cold and 30 measured units")
    tps_values = [float(row["steadyStateTokensPerSecond"]) for row in measured]
    ttft_values = [float(row["ttftMs"]) for row in measured]
    total_values = [float(row["totalMs"]) for row in measured]
    tps_summary = summary(tps_values) if tps_values else None
    inter_summary = summary(pooled_intervals) if pooled_intervals else None
    bootstrap = bootstrap_median_ci(tps_values) if tps_values else (None, None)
    component_summary = {
        name: summary(values) for name, values in sorted(component_values.items())
        if values
    }
    gpu_rows = [gpu for sample in resource_samples
                for gpu in sample.get("gpus", []) if isinstance(gpu, dict)]
    gpu_ids = {str(row.get("uuid", "")) for row in gpu_rows}
    if len(gpu_ids) != 3:
        errors.append("G7 resource evidence does not cover exactly three GPUs")
    resource_summary = {
        "sampleCount": len(resource_samples),
        "gpuUuids": sorted(gpu_ids),
        "gpuUtilizationPercent": (
            summary(float(row["utilizationPercent"]) for row in gpu_rows)
            if gpu_rows else None),
        "gpuMemoryUsedMiB": (
            summary(float(row["memoryUsedMiB"]) for row in gpu_rows)
            if gpu_rows else None),
        "gpuPowerDrawW": (
            summary(float(row["powerDrawW"]) for row in gpu_rows)
            if gpu_rows and all("powerDrawW" in row for row in gpu_rows)
            else None),
        "processCount": (
            summary(float(row.get("processCount", 0))
                    for row in resource_samples)
            if resource_samples else None),
        "rssBytes": (
            summary(float(row.get("rssBytes", 0)) for row in resource_samples)
            if resource_samples else None),
        "networkScope": "compute-node-non-loopback",
        "networkRxByteDelta": sum(int(row.get("networkRxByteDelta", 0))
                                  for row in resource_group_records),
        "networkTxByteDelta": sum(int(row.get("networkTxByteDelta", 0))
                                  for row in resource_group_records),
        "averageCpuCores": (
            summary(float(row["averageCpuCores"])
                    for row in resource_group_records
                    if "averageCpuCores" in row)
            if any("averageCpuCores" in row for row in resource_group_records)
            else None),
        "perProcessGroup": resource_group_records,
    }
    correctness_pass = not errors
    performance_pass = bool(
        correctness_pass and tps_summary and inter_summary
        and float(tps_summary["median"]) >= 20.0
        and float(inter_summary["p95"]) <= 75.0)
    verdict = (
        "PERFORMANCE_PASS" if performance_pass else
        "FUNCTIONAL_PASS_PERFORMANCE_MISS" if correctness_pass else "FAIL")
    return {
        "schemaVersion": SCHEMA,
        "status": "PASS" if correctness_pass else "FAIL",
        "gate": "G7", "verdict": verdict,
        "candidateId": candidate_id,
        "thresholds": {
            "medianWarmSteadyStateTokensPerSecond": 20.0,
            "p95WarmInterTokenMs": 75.0,
        },
        "counts": {"coldExcluded": len(cold), "warmMeasured": len(measured),
                   "failures": len(errors)},
        "steadyStateTokensPerSecond": tps_summary,
        "ttftMs": summary(ttft_values) if ttft_values else None,
        "totalMs": summary(total_values) if total_values else None,
        "pooledInterTokenMs": inter_summary,
        "bootstrapMedianTokensPerSecond95Ci": {
            "seed": BOOTSTRAP_SEED, "resamples": BOOTSTRAP_REPETITIONS,
            "lower": bootstrap[0], "upper": bootstrap[1]},
        "perProcess": process_records,
        "perPromptTokensPerSecond": {
            prompt: summary(values) for prompt, values in sorted(prompt_values.items())},
        "componentTimingsMs": component_summary,
        "resources": resource_summary,
        "prerequisites": prerequisites,
        "invocations": measured,
        "errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument("--bundle", required=True, type=Path)
    parser.add_argument("--g5-manifest", required=True, type=Path)
    parser.add_argument("--g6-manifest", required=True, type=Path)
    parser.add_argument("--g6c-manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = analyze(
        args.output_root.expanduser().resolve(), args.bundle.expanduser().resolve(),
        [args.g5_manifest.expanduser().resolve(),
         args.g6_manifest.expanduser().resolve(),
         args.g6c_manifest.expanduser().resolve()])
    output = args.output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({
        "status": result["status"], "verdict": result["verdict"],
        "warmMeasured": result["counts"]["warmMeasured"],
        "errors": result["errors"]}, sort_keys=True))
    return 0 if result["status"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
