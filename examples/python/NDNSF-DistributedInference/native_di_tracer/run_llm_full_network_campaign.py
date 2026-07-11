#!/usr/bin/env python3
"""Run MiniNDN full-network LLM layout comparisons."""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any


REPO = Path(__file__).resolve().parents[4]
HARNESS = REPO / "Experiments/NDNSF_DI_NativeTracer_Minindn.py"
MODES = ["greedy", "proportional"]
CAMPAIGN_PROFILE_FIELDS = {
    "out": "out_root",
    "llm_planner_mode": "modes",
    "provider_check_timeout": "provider_check_timeout",
    "role_execution_delay_ms": "role_execution_delay_ms",
    "llm_stage_execution_delay_scale": "stage_execution_delay_scale",
    "target_rps": "target_rps",
    "open_loop_duration_s": "open_loop_duration_s",
    "open_loop_driver_mode": "open_loop_driver_mode",
    "submission_spacing_ms": "submission_spacing_ms",
    "runtime_v1_context_tokens": "runtime_v1_context_tokens",
    "runtime_v1_generated_tokens": "runtime_v1_generated_tokens",
    "runtime_v1_prefix_id": "runtime_v1_prefix_id",
    "core_trace": "core_trace",
}


def parse_workloads(raw: str) -> list[tuple[str, int, int]]:
    workloads: list[tuple[str, int, int]] = []
    for item in raw.split(","):
        text = item.strip()
        if not text:
            continue
        parts = text.split(":")
        if len(parts) == 1:
            concurrency = int(parts[0])
            label = f"c{concurrency}"
            requests = concurrency
        elif len(parts) == 2:
            label = parts[0]
            concurrency = int(parts[1])
            requests = concurrency
        elif len(parts) == 3:
            label = parts[0]
            requests = int(parts[1])
            concurrency = int(parts[2])
        else:
            raise SystemExit(
                "--workloads entries must be concurrency, label:concurrency, "
                "or label:requests:concurrency")
        if requests <= 0 or concurrency <= 0:
            raise SystemExit("--workloads requests and concurrency must be positive")
        if concurrency > requests:
            raise SystemExit("--workloads concurrency cannot exceed requests")
        workloads.append((label, requests, concurrency))
    if not workloads:
        raise SystemExit("--workloads must contain at least one workload")
    return workloads


def load_json_file(path: str) -> dict[str, Any]:
    if not path:
        return {}
    with Path(path).expanduser().open(encoding="utf-8") as fh:
        return json.load(fh)


def native_tracer_section(payload: dict[str, Any]) -> dict[str, Any]:
    profile = payload.get("profile") if isinstance(payload.get("profile"), dict) else payload
    distributed = profile.get("distributed_inference", {})
    native = distributed.get("native_tracer", {})
    if not isinstance(native, dict):
        return {}
    return native if native.get("enabled", False) else {}


def runtime_profile_defaults(runtime_profile: str, runtime_resolved: str) -> dict[str, Any]:
    defaults: dict[str, Any] = {}
    for source in [runtime_profile, runtime_resolved]:
        section = native_tracer_section(load_json_file(source))
        for key, dest in CAMPAIGN_PROFILE_FIELDS.items():
            if key not in section:
                continue
            value = section[key]
            if dest == "out_root":
                defaults[dest] = str(Path(str(value)) / "campaign")
            elif dest == "modes":
                defaults[dest] = str(value)
            else:
                defaults[dest] = value
    return defaults


def default_value(defaults: dict[str, Any], key: str, fallback):
    return defaults.get(key, fallback)


def parse_target_rps_series(raw: str, fallback: float) -> list[float]:
    if not raw.strip():
        return [fallback]
    rates: list[float] = []
    for item in raw.split(","):
        text = item.strip()
        if not text:
            continue
        rate = float(text)
        if rate <= 0.0:
            raise SystemExit("--target-rps-series values must be positive")
        rates.append(rate)
    if not rates:
        raise SystemExit("--target-rps-series must contain at least one positive rate")
    return rates


def rate_label(rate: float) -> str:
    text = ("%g" % rate).replace(".", "p")
    return f"r{text}"


def percentile_nearest_rank(values: list[float], percentile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    rank = max(1, math.ceil((percentile / 100.0) * len(ordered)))
    return ordered[min(rank, len(ordered)) - 1]


def stats(values: list[float]) -> dict[str, float | int]:
    if not values:
        return {
            "count": 0,
            "mean": 0.0,
            "stddev": 0.0,
            "p50": 0.0,
            "p95": 0.0,
            "min": 0.0,
            "max": 0.0,
        }
    return {
        "count": len(values),
        "mean": round(statistics.mean(values), 3),
        "stddev": round(statistics.stdev(values), 3) if len(values) > 1 else 0.0,
        "p50": round(statistics.median(values), 3),
        "p95": round(percentile_nearest_rank(values, 95.0), 3),
        "min": round(min(values), 3),
        "max": round(max(values), 3),
    }


def provider_metric(summary: dict[str, Any], provider: str, path: list[str]) -> float:
    current: Any = summary.get(provider, {})
    for key in path:
        if not isinstance(current, dict):
            return 0.0
        current = current.get(key, 0.0)
    try:
        return float(current)
    except (TypeError, ValueError):
        return 0.0


def clean_dir(path: Path) -> None:
    if path.exists():
        subprocess.run(["sudo", "-n", "rm", "-rf", str(path)], check=True)
    path.mkdir(parents=True, exist_ok=True)


def parse_user_execution_from_log(log_path: Path) -> dict[str, Any]:
    prefix = "NDNSF_DI_NATIVE_TRACER_USER_EXECUTION "
    if not log_path.exists():
        return {}
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(prefix):
            return json.loads(line[len(prefix):])
    return {}


def run_harness(mode: str,
                workload_label: str,
                run_index: int,
                out_root: Path,
                requests: int,
                concurrency: int,
                provider_check_timeout: int,
                role_execution_delay_ms: float,
                stage_execution_delay_scale: float,
                target_rps: float,
                open_loop_duration_s: float,
                open_loop_driver_mode: str,
                runtime_profile: str,
                runtime_resolved: str,
                extra_args: list[str]) -> dict[str, Any]:
    run_dir = out_root / mode / workload_label / f"run-{run_index:02d}"
    clean_dir(run_dir)
    command = [
        "sudo", "-n", "python3", str(HARNESS),
        "--policy-bundle", "llm-proportional",
        "--assignment", "llm-proportional",
        "--llm-planner-mode", mode,
        "--full-network",
        "--requests", str(requests),
        "--concurrency", str(concurrency),
        "--provider-check-timeout", str(provider_check_timeout),
        "--role-execution-delay-ms", str(role_execution_delay_ms),
        "--llm-stage-execution-delay-scale", str(stage_execution_delay_scale),
        "--out", str(run_dir),
        *extra_args,
    ]
    if runtime_profile:
        command.extend(["--runtime-profile", runtime_profile])
    if runtime_resolved:
        command.extend(["--runtime-resolved", runtime_resolved])
    if open_loop_duration_s > 0.0:
        command.extend([
            "--target-rps", str(target_rps),
            "--open-loop-duration-s", str(open_loop_duration_s),
            "--open-loop-driver-mode", open_loop_driver_mode,
        ])
    completed = subprocess.run(command, cwd=str(REPO), check=False)
    summary_path = run_dir / "summary.json"
    if not summary_path.exists():
        raise RuntimeError(f"harness did not write summary: {summary_path}")
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    user = summary.get("userExecution", {})
    if user.get("status") in ("", "gated", None):
        fallback_user = parse_user_execution_from_log(run_dir / "logs" / "user-driver.log")
        if fallback_user:
            user = fallback_user
    optimization = summary.get("optimizationEvidence", {})
    runtime_v1 = summary.get("runtimeV1", {})
    provider_utilization = summary.get("providerUtilization", {})
    planner_prediction = optimization.get("prediction", {}) if isinstance(optimization, dict) else {}
    return {
        "mode": mode,
        "workload": workload_label,
        "run": run_index,
        "requests": requests,
        "concurrency": concurrency,
        "targetRps": round(float(user.get("targetRps", target_rps)), 6),
        "openLoopDurationS": round(float(user.get("openLoopDurationS", open_loop_duration_s)), 3),
        "openLoopDriverMode": str(user.get("mode", open_loop_driver_mode)),
        "scheduledRequestCount": int(user.get("scheduledRequestCount", requests)),
        "submittedCount": int(user.get("submittedCount", user.get("successCount", 0))),
        "localBackpressureCount": int(user.get("localBackpressureCount", 0)),
        "offeredRps": round(float(user.get("offeredRps", 0.0)), 6),
        "harnessReturncode": completed.returncode,
        "status": summary.get("status", ""),
        "securityBootstrap": summary.get("securityBootstrap", {}).get("status", ""),
        "userExecution": user.get("status", ""),
        "dependencyExecution": summary.get("dependencyExecution", {}).get("status", ""),
        "failureReason": summary.get("failureReason", user.get("error", "")),
        "successCount": int(user.get("successCount", 0)),
        "failureCount": int(user.get("failureCount", requests)),
        "elapsedMs": round(float(user.get("elapsedMs", 0.0)), 3),
        "makespanMs": round(float(user.get("makespanMs", 0.0)), 3),
        "meanMs": round(float(user.get("meanMs", 0.0)), 3),
        "p50Ms": round(float(user.get("p50Ms", 0.0)), 3),
        "p95Ms": round(float(user.get("p95Ms", 0.0)), 3),
        "throughputRps": round(float(user.get("throughputRps", 0.0)), 6),
        "planId": optimization.get("planId", ""),
        "layerAllocationJson": json.dumps(
            optimization.get("layerAllocation", {}),
            sort_keys=True,
            separators=(",", ":")),
        "predictedBottleneckProvider": optimization.get("predictedBottleneckProvider", ""),
        "maxPredictedUtilization": round(
            float(optimization.get("maxPredictedUtilization", 0.0)), 6),
        "predictionLimitKind": optimization.get("predictionLimitKind", ""),
        "runtimeV1PlanId": runtime_v1.get("planId", "") if isinstance(runtime_v1, dict) else "",
        "runtimeV1ContextTokens": int(runtime_v1.get("contextTokens", 0)) if isinstance(runtime_v1, dict) else 0,
        "runtimeV1CacheProvider": runtime_v1.get("cacheProvider", "") if isinstance(runtime_v1, dict) else "",
        "runtimeV1TimeToFirstTokenMs": round(
            float(runtime_v1.get("timeToFirstTokenMs", 0.0)), 3) if isinstance(runtime_v1, dict) else 0.0,
        "runtimeV1InterTokenMs": round(
            float(runtime_v1.get("interTokenMs", 0.0)), 3) if isinstance(runtime_v1, dict) else 0.0,
        "runtimeV1AllocationMatchesPolicy": str(
            runtime_v1.get("allocationMatchesPolicy", "")) if isinstance(runtime_v1, dict) else "",
        "plannerPredictionJson": json.dumps(
            planner_prediction,
            sort_keys=True,
            separators=(",", ":")),
        "providerUtilizationJson": json.dumps(
            provider_utilization,
            sort_keys=True,
            separators=(",", ":")),
        "resultDir": str(run_dir),
    }


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        raise RuntimeError("cannot write empty campaign CSV")
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def summarize(rows: list[dict[str, Any]]) -> dict[str, Any]:
    by_key: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for row in rows:
        by_key.setdefault((str(row["mode"]), str(row["workload"])), []).append(row)
    summary: dict[str, Any] = {}
    for (mode, workload), items in sorted(by_key.items()):
        successful = [item for item in items if item["status"] == "SUCCESS"]
        throughputs = [float(item["throughputRps"]) for item in successful]
        observed_success_rps = [float(item["throughputRps"]) for item in items]
        observed_p50 = [float(item["p50Ms"]) for item in items]
        observed_p95 = [float(item["p95Ms"]) for item in items]
        offered = [float(item["offeredRps"]) for item in items]
        scheduled_count = sum(int(item["scheduledRequestCount"]) for item in items)
        success_count = sum(int(item["successCount"]) for item in items)
        summary[f"{mode}/{workload}"] = {
            "mode": mode,
            "workload": workload,
            "runs": len(items),
            "successfulRuns": len(successful),
            "successCount": success_count,
            "failureCount": sum(int(item["failureCount"]) for item in items),
            "scheduledRequestCount": scheduled_count,
            "submittedCount": sum(int(item["submittedCount"]) for item in items),
            "localBackpressureCount": sum(int(item["localBackpressureCount"]) for item in items),
            "successRate": round(success_count / scheduled_count, 6) if scheduled_count else 0.0,
            "offeredRps": stats(offered),
            "meanMs": stats([float(item["meanMs"]) for item in successful]),
            "p50Ms": stats([float(item["p50Ms"]) for item in successful]),
            "p95Ms": stats([float(item["p95Ms"]) for item in successful]),
            "observedP50Ms": stats(observed_p50),
            "observedP95Ms": stats(observed_p95),
            "throughputRps": stats(throughputs),
            "observedSuccessRps": stats(observed_success_rps),
            "maxStableRps": round(max(throughputs), 6) if throughputs else 0.0,
            "maxObservedSuccessRps": (
                round(max(observed_success_rps), 6) if observed_success_rps else 0.0),
            "layerAllocations": sorted(set(
                str(item["layerAllocationJson"]) for item in items)),
            "plannerPrediction": summarize_planner_prediction(items),
            "providerUtilization": summarize_provider_utilization(items),
            "resultDirs": [str(item["resultDir"]) for item in items],
        }
    return summary


def summarize_planner_prediction(items: list[dict[str, Any]]) -> dict[str, Any]:
    samples: list[dict[str, Any]] = []
    for item in items:
        raw = str(item.get("plannerPredictionJson", "{}"))
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError:
            parsed = {}
        if isinstance(parsed, dict) and parsed:
            samples.append(parsed)
    if not samples:
        return {}
    provider_names = sorted({
        provider
        for sample in samples
        for provider in sample.get("providerLoad", {}).keys()
    })
    provider_load: dict[str, Any] = {}
    for provider in provider_names:
        provider_samples = [
            sample.get("providerLoad", {}).get(provider, {})
            for sample in samples
            if isinstance(sample.get("providerLoad", {}).get(provider, {}), dict)
        ]
        provider_load[provider] = {
            "samples": len(provider_samples),
            "roles": sorted({
                role
                for sample in provider_samples
                for role in sample.get("roles", [])
            }),
            "layerCount": stats([
                float(sample.get("layerCount", 0.0)) for sample in provider_samples
            ]),
            "estimatedServiceMsPerRequest": stats([
                float(sample.get("estimatedServiceMsPerRequest", 0.0))
                for sample in provider_samples
            ]),
            "predictedUtilization": stats([
                float(sample.get("predictedUtilization", 0.0))
                for sample in provider_samples
            ]),
            "queueRisks": sorted({
                str(sample.get("predictedQueueRisk", ""))
                for sample in provider_samples
                if str(sample.get("predictedQueueRisk", ""))
            }),
        }
    return {
        "samples": len(samples),
        "targetRps": sorted({
            round(float(sample.get("targetRps", 0.0)), 6)
            for sample in samples
        }),
        "bottleneckProviders": sorted({
            str(sample.get("bottleneckProvider", ""))
            for sample in samples
            if str(sample.get("bottleneckProvider", ""))
        }),
        "maxPredictedUtilization": stats([
            float(sample.get("maxPredictedUtilization", 0.0))
            for sample in samples
        ]),
        "limitKinds": sorted({
            str(sample.get("limitKind", ""))
            for sample in samples
            if str(sample.get("limitKind", ""))
        }),
        "providerLoad": provider_load,
    }


def summarize_provider_utilization(items: list[dict[str, Any]]) -> dict[str, Any]:
    provider_samples: dict[str, list[dict[str, Any]]] = {}
    for item in items:
        raw = str(item.get("providerUtilizationJson", "{}"))
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError:
            parsed = {}
        if not isinstance(parsed, dict):
            continue
        for provider, provider_summary in parsed.items():
            if isinstance(provider_summary, dict):
                provider_samples.setdefault(str(provider), []).append(provider_summary)

    result: dict[str, Any] = {}
    for provider, samples in sorted(provider_samples.items()):
        result[provider] = {
            "runs": len(samples),
            "roleEventCount": sum(int(sample.get("roleEventCount", 0)) for sample in samples),
            "uniqueSessionCount": sum(int(sample.get("uniqueSessionCount", 0)) for sample in samples),
            "observedRoles": sorted({
                role
                for sample in samples
                for role in sample.get("observedRoles", [])
            }),
            "estimatedUtilization": stats([
                provider_metric({provider: sample}, provider, ["estimatedUtilization"])
                for sample in samples
            ]),
            "busyHandlerMs": stats([
                provider_metric({provider: sample}, provider, ["busyHandlerMs"])
                for sample in samples
            ]),
            "queueWaitMeanMs": stats([
                provider_metric({provider: sample}, provider, ["queueWaitMs", "mean"])
                for sample in samples
            ]),
            "queueWaitMaxMs": stats([
                provider_metric({provider: sample}, provider, ["queueWaitMs", "max"])
                for sample in samples
            ]),
            "inputFetchWaitMeanMs": stats([
                provider_metric({provider: sample}, provider, ["inputFetchWaitMs", "mean"])
                for sample in samples
            ]),
            "handlerMeanMs": stats([
                provider_metric({provider: sample}, provider, ["handlerMs", "mean"])
                for sample in samples
            ]),
            "totalMeanMs": stats([
                provider_metric({provider: sample}, provider, ["totalMs", "mean"])
                for sample in samples
            ]),
            "capacityMax": {
                "activeWorkers": max(
                    int(sample.get("capacityMax", {}).get("activeWorkers", 0))
                    for sample in samples),
                "readyQueue": max(
                    int(sample.get("capacityMax", {}).get("readyQueue", 0))
                    for sample in samples),
                "waitingInputs": max(
                    int(sample.get("capacityMax", {}).get("waitingInputs", 0))
                    for sample in samples),
                "pendingWork": max(
                    int(sample.get("capacityMax", {}).get("pendingWork", 0))
                    for sample in samples),
            },
        }
    return result


def main(argv: list[str] | None = None) -> int:
    pre_parser = argparse.ArgumentParser(add_help=False)
    pre_parser.add_argument("--runtime-profile", default="",
                            help="Load campaign defaults from an NDNSF runtime profile JSON")
    pre_parser.add_argument("--runtime-resolved", default="",
                            help="Load campaign defaults from a runtime doctor resolved JSON")
    pre_args, _ = pre_parser.parse_known_args(argv)
    profile_defaults = runtime_profile_defaults(
        pre_args.runtime_profile,
        pre_args.runtime_resolved)

    parser = argparse.ArgumentParser(description=__doc__, parents=[pre_parser])
    parser.add_argument("--out-root", default=default_value(profile_defaults, "out_root", ""))
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--workloads", default="c1:1:1")
    parser.add_argument("--modes", default=default_value(profile_defaults, "modes", "greedy,proportional"))
    parser.add_argument("--provider-check-timeout", type=int,
                        default=default_value(profile_defaults, "provider_check_timeout", 60))
    parser.add_argument("--role-execution-delay-ms", type=float,
                        default=default_value(profile_defaults, "role_execution_delay_ms", 0.0))
    parser.add_argument("--stage-execution-delay-scale", type=float,
                        default=default_value(profile_defaults, "stage_execution_delay_scale", 1.0))
    parser.add_argument("--target-rps", type=float,
                        default=default_value(profile_defaults, "target_rps", 0.0),
                        help="Open-loop offered request rate when --open-loop-duration-s is set")
    parser.add_argument("--target-rps-series", default="",
                        help="Comma-separated open-loop offered rates; overrides --target-rps loop list")
    parser.add_argument("--open-loop-duration-s", type=float,
                        default=default_value(profile_defaults, "open_loop_duration_s", 0.0),
                        help="Run each workload as fixed-rate open-loop for this many seconds")
    parser.add_argument("--open-loop-driver-mode",
                        choices=["child", "threaded"],
                        default=default_value(profile_defaults, "open_loop_driver_mode", "threaded"),
                        help="User driver implementation for open-loop workloads")
    parser.add_argument("--submission-spacing-ms", type=int,
                        default=default_value(profile_defaults, "submission_spacing_ms", 0))
    parser.add_argument("--runtime-v1-context-tokens", type=int,
                        default=default_value(profile_defaults, "runtime_v1_context_tokens", 1024))
    parser.add_argument("--runtime-v1-generated-tokens", type=int,
                        default=default_value(profile_defaults, "runtime_v1_generated_tokens", 32))
    parser.add_argument("--runtime-v1-prefix-id",
                        default=default_value(profile_defaults, "runtime_v1_prefix_id", ""))
    parser.add_argument("--core-trace", action="store_true",
                        default=bool(default_value(profile_defaults, "core_trace", False)))
    args = parser.parse_args(argv)
    if not args.out_root:
        raise SystemExit("--out-root is required unless provided by --runtime-profile/--runtime-resolved")
    if args.runs <= 0:
        raise SystemExit("--runs must be positive")
    if args.provider_check_timeout <= 0:
        raise SystemExit("--provider-check-timeout must be positive")
    if args.submission_spacing_ms < 0:
        raise SystemExit("--submission-spacing-ms must be non-negative")
    if args.runtime_v1_context_tokens <= 0:
        raise SystemExit("--runtime-v1-context-tokens must be positive")
    if args.runtime_v1_generated_tokens < 0:
        raise SystemExit("--runtime-v1-generated-tokens must be non-negative")
    if args.role_execution_delay_ms < 0.0:
        raise SystemExit("--role-execution-delay-ms must be non-negative")
    if args.stage_execution_delay_scale < 0.0:
        raise SystemExit("--stage-execution-delay-scale must be non-negative")
    if args.target_rps < 0.0:
        raise SystemExit("--target-rps must be non-negative")
    if args.open_loop_duration_s < 0.0:
        raise SystemExit("--open-loop-duration-s must be non-negative")
    if args.open_loop_duration_s > 0.0 and args.target_rps <= 0.0:
        if not args.target_rps_series.strip():
            raise SystemExit("--open-loop-duration-s requires --target-rps or --target-rps-series")

    modes = [item.strip() for item in args.modes.split(",") if item.strip()]
    invalid_modes = sorted(set(modes) - set(MODES))
    if invalid_modes:
        raise SystemExit(f"unsupported mode(s): {','.join(invalid_modes)}")
    workloads = parse_workloads(args.workloads)
    target_rates = parse_target_rps_series(args.target_rps_series, args.target_rps)
    if args.open_loop_duration_s <= 0.0 and len(target_rates) != 1:
        raise SystemExit("--target-rps-series requires --open-loop-duration-s")
    out_root = Path(args.out_root).resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    extra_args: list[str] = []
    if args.core_trace:
        extra_args.append("--core-trace")
    if args.submission_spacing_ms > 0:
        extra_args.extend(["--submission-spacing-ms", str(args.submission_spacing_ms)])
    extra_args.extend([
        "--runtime-v1-context-tokens", str(args.runtime_v1_context_tokens),
        "--runtime-v1-generated-tokens", str(args.runtime_v1_generated_tokens),
    ])
    if args.runtime_v1_prefix_id:
        extra_args.extend(["--runtime-v1-prefix-id", args.runtime_v1_prefix_id])

    rows: list[dict[str, Any]] = []
    for mode in modes:
        for workload_label, requests, concurrency in workloads:
            for target_rps in target_rates:
                effective_requests = requests
                effective_label = workload_label
                if args.open_loop_duration_s > 0.0:
                    needed = int(math.ceil(target_rps * args.open_loop_duration_s))
                    effective_requests = max(requests, needed)
                    effective_label = f"{workload_label}-{rate_label(target_rps)}"
                for run_index in range(1, args.runs + 1):
                    rows.append(run_harness(
                        mode,
                        effective_label,
                        run_index,
                        out_root,
                        effective_requests,
                        concurrency,
                        args.provider_check_timeout,
                        args.role_execution_delay_ms,
                        args.stage_execution_delay_scale,
                        target_rps,
                        args.open_loop_duration_s,
                        args.open_loop_driver_mode,
                        args.runtime_profile,
                        args.runtime_resolved,
                        extra_args,
                    ))

    rows_csv = out_root / "llm-full-network-campaign-runs.csv"
    summary_json = out_root / "llm-full-network-campaign-summary.json"
    write_csv(rows_csv, rows)
    summary = {
        "runs": args.runs,
        "workloads": [
            {"label": label, "requests": requests, "concurrency": concurrency}
            for label, requests, concurrency in workloads
        ],
        "modes": modes,
        "roleExecutionDelayMs": args.role_execution_delay_ms,
        "stageExecutionDelayScale": args.stage_execution_delay_scale,
        "targetRps": args.target_rps,
        "targetRpsSeries": target_rates,
        "openLoopDurationS": args.open_loop_duration_s,
        "openLoopDriverMode": args.open_loop_driver_mode,
        "runtimeV1ContextTokens": args.runtime_v1_context_tokens,
        "runtimeV1GeneratedTokens": args.runtime_v1_generated_tokens,
        "runtimeV1PrefixId": args.runtime_v1_prefix_id,
        "runtimeProfile": {
            "profile": args.runtime_profile,
            "resolved": args.runtime_resolved,
        },
        "rowsCsv": str(rows_csv),
        "summary": summarize(rows),
    }
    summary_json.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n",
                            encoding="utf-8")
    print("NDNSF_DI_LLM_FULL_NETWORK_CAMPAIGN_OK")
    print(json.dumps(summary["summary"], indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
