#!/usr/bin/env python3
"""Submit a real NDNSF collaboration request for /Inference/NativeTracer."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import os
import secrets
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Optional

from ndnsf import ServiceUser


SERVICE = "/Inference/NativeTracer"
GROUP = "/NDNSF-DI/Tracer/group"
CONTROLLER = "/NDNSF-DI/Tracer/controller"
USER = "/NDNSF-DI/Tracer/user"


def encode_tensor_bundle() -> bytes:
    payload = bytearray(b"NDITB001")
    payload += struct.pack("<I", 1)
    name = b"images"
    payload += struct.pack("<I", len(name)) + name
    payload += struct.pack("<I", 1)  # Float32
    shape = [1, 3, 2, 2]
    payload += struct.pack("<I", len(shape))
    for dim in shape:
        payload += struct.pack("<q", dim)
    values = [float(i) / 10.0 for i in range(12)]
    data = struct.pack("<" + "f" * len(values), *values)
    payload += struct.pack("<Q", len(data)) + data
    return bytes(payload)


def load_service_plan(path: Path, service: str) -> dict:
    plan = json.loads(path.read_text(encoding="utf-8"))
    return next(item for item in plan["services"] if item["service"] == service)


def sample_service_plan(service: str) -> dict:
    return {
        "service": service,
        "roles": ["/Backbone", "/Head/Shard/0", "/Head/Shard/1", "/Merge"],
        "dependencies": [
            {
                "producers": ["/Backbone"],
                "consumers": ["/Head/Shard/0"],
                "keyScope": "backbone-to-head0",
                "topicPrefix": "/activation",
                "required": True,
            },
            {
                "producers": ["/Backbone"],
                "consumers": ["/Head/Shard/1"],
                "keyScope": "backbone-to-head1",
                "topicPrefix": "/activation",
                "required": True,
            },
            {
                "producers": ["/Head/Shard/0"],
                "consumers": ["/Merge"],
                "keyScope": "head0-to-merge",
                "topicPrefix": "/activation",
                "required": True,
            },
            {
                "producers": ["/Head/Shard/1"],
                "consumers": ["/Merge"],
                "keyScope": "head1-to-merge",
                "topicPrefix": "/activation",
                "required": True,
            },
        ],
    }


def collaboration_roles(service_plan: dict, service: str) -> list[dict]:
    return [
        {
            "role": role,
            "service": service,
            "min_providers": 1,
            "max_providers": 1,
        }
        for role in service_plan["roles"]
    ]


def collaboration_dependencies(service_plan: dict) -> list[dict]:
    deps = []
    for dep in service_plan.get("dependencies", []):
        deps.append({
            "producers": list(dep.get("producers", [])),
            "consumers": list(dep.get("consumers", [])),
            "key_scope": str(dep["keyScope"]),
            "topic_prefix": str(dep.get("topicPrefix", "/activation")),
            "required": bool(dep.get("required", True)),
        })
    return deps


def key_scopes_and_role_scopes(service_plan: dict) -> tuple[dict[str, list[str]], dict[str, list[str]]]:
    key_scopes: dict[str, list[str]] = {}
    role_scopes: dict[str, list[str]] = {role: [] for role in service_plan["roles"]}
    for dep in service_plan.get("dependencies", []):
        scope = str(dep["keyScope"])
        roles = list(dep.get("producers", [])) + list(dep.get("consumers", []))
        key_scopes[scope] = roles
        for role in roles:
            role_scopes.setdefault(role, []).append(scope)
    return key_scopes, role_scopes


def publish_scope_keys(user: ServiceUser, service: str, key_scopes: dict[str, list[str]]) -> dict[str, str]:
    scope_key_data_names: dict[str, str] = {}
    for scope in key_scopes:
        result = user.publish_encrypted_large_data(
            service,
            secrets.token_bytes(32),
            object_label=f"native-tracer-scope-key-{scope}",
            freshness_ms=60000,
        )
        if not result.success:
            raise RuntimeError(f"scope key publish failed for {scope}: {result.error}")
        scope_key_data_names[scope] = result.encrypted_data_name
    return scope_key_data_names


def percentile_nearest_rank(values: list[float], percentile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    rank = max(1, math.ceil((percentile / 100.0) * len(ordered)))
    return float(ordered[min(rank, len(ordered)) - 1])


def summarize_workload(results: list[dict], makespan_ms: float, service: str, concurrency: int) -> dict:
    latencies = [float(item.get("elapsedMs", 0.0)) for item in results]
    successes = [item for item in results if item.get("status") == "executed"]
    return {
        "status": "executed" if len(successes) == len(results) else "failed",
        "service": service,
        "requestCount": len(results),
        "concurrency": concurrency,
        "successCount": len(successes),
        "failureCount": len(results) - len(successes),
        "responseStatus": len(successes) == len(results),
        "payloadBytes": int(sum(int(item.get("payloadBytes", 0)) for item in results)),
        "elapsedMs": makespan_ms,
        "makespanMs": makespan_ms,
        "meanMs": (sum(latencies) / len(latencies)) if latencies else 0.0,
        "p50Ms": percentile_nearest_rank(latencies, 50.0),
        "p95Ms": percentile_nearest_rank(latencies, 95.0),
        "minMs": min(latencies) if latencies else 0.0,
        "maxMs": max(latencies) if latencies else 0.0,
        "throughputRps": (len(successes) / (makespan_ms / 1000.0)) if makespan_ms > 0 else 0.0,
        "error": "; ".join(
            str(item.get("error", ""))
            for item in results
            if item.get("status") != "executed" and item.get("error")
        ),
        "requests": results,
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the NativeTracer user driver")
    parser.add_argument("--plan", default="")
    parser.add_argument("--service", default=SERVICE)
    parser.add_argument("--group", default=GROUP)
    parser.add_argument("--controller", default=CONTROLLER)
    parser.add_argument("--user", default=USER)
    parser.add_argument("--trust-schema", default="examples/trust-schema.conf")
    parser.add_argument("--ack-timeout-ms", type=int, default=1200)
    parser.add_argument("--timeout-ms", type=int, default=20000)
    parser.add_argument("--permission-wait-ms", type=int, default=2500)
    parser.add_argument("--requests", type=int, default=1,
                        help="Number of closed-loop collaboration requests to submit")
    parser.add_argument("--concurrency", type=int, default=1,
                        help="Maximum outstanding collaboration requests")
    parser.add_argument("--submission-spacing-ms", type=int, default=0,
                        help="Delay between child request submissions in concurrent mode")
    parser.add_argument("--burst-admission-providers", default="",
                        help=("Comma-separated provider names used to seed "
                              "per-child burst admission bias"))
    parser.add_argument("--worker-child", action="store_true",
                        help=argparse.SUPPRESS)
    parser.add_argument("--request-index", type=int, default=1,
                        help=argparse.SUPPRESS)
    parser.add_argument("--scope-key-data-names-json", default="",
                        help=argparse.SUPPRESS)
    parser.add_argument("--dry-run", action="store_true")
    return parser


def run_one_request(user: ServiceUser,
                    args,
                    roles: list[dict],
                    key_scopes: dict[str, list[str]],
                    dependencies: list[dict],
                    scope_key_data_names: dict[str, str],
                    role_scopes: dict[str, list[str]],
                    index: int) -> dict:
    start = time.perf_counter()
    try:
        response = user.request_collaboration(
            args.service,
            encode_tensor_bundle(),
            roles=roles,
            key_scopes=key_scopes,
            dependencies=dependencies,
            scope_key_data_names=scope_key_data_names,
            role_scopes=role_scopes,
            ack_timeout_ms=args.ack_timeout_ms,
            timeout_ms=args.timeout_ms,
        )
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        return {
            "status": "executed" if response.status else "failed",
            "service": args.service,
            "requestIndex": index,
            "requestCount": args.requests,
            "concurrency": args.concurrency,
            "responseStatus": bool(response.status),
            "payloadBytes": len(response.payload),
            "error": response.error,
            "elapsedMs": elapsed_ms,
        }
    except Exception as exc:
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        return {
            "status": "failed",
            "service": args.service,
            "requestIndex": index,
            "requestCount": args.requests,
            "concurrency": args.concurrency,
            "responseStatus": False,
            "payloadBytes": 0,
            "error": str(exc),
            "elapsedMs": elapsed_ms,
        }


def run_async_requests(user: ServiceUser,
                       args,
                       roles: list[dict],
                       key_scopes: dict[str, list[str]],
                       dependencies: list[dict],
                       scope_key_data_names: dict[str, str],
                       role_scopes: dict[str, list[str]]) -> list[dict]:
    condition = threading.Condition()
    starts: dict[int, float] = {}
    results: dict[int, dict] = {}
    state = {
        "next": 1,
        "inFlight": 0,
        "completed": 0,
    }

    def record_result(index: int, response_status: bool, payload: bytes, error: str) -> None:
        elapsed_ms = (time.perf_counter() - starts.get(index, time.perf_counter())) * 1000.0
        result = {
            "status": "executed" if response_status else "failed",
            "service": args.service,
            "requestIndex": index,
            "requestCount": args.requests,
            "concurrency": args.concurrency,
            "responseStatus": bool(response_status),
            "payloadBytes": len(payload),
            "error": error,
            "elapsedMs": elapsed_ms,
        }
        print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " + json.dumps(result, sort_keys=True), flush=True)
        with condition:
            results[index] = result
            state["inFlight"] -= 1
            state["completed"] += 1
            submit_locked()
            condition.notify_all()

    def submit_one_locked(index: int) -> None:
        starts[index] = time.perf_counter()
        state["inFlight"] += 1
        print(
            "NDNSF_DI_NATIVE_TRACER_USER_SUBMIT "
            + json.dumps({
                "requestIndex": index,
                "requestCount": args.requests,
                "concurrency": args.concurrency,
            }, sort_keys=True),
            flush=True,
        )

        def on_response(response) -> None:
            record_result(index, bool(response.status), bytes(response.payload), str(response.error))

        def on_timeout(request_id: str) -> None:
            record_result(index, False, b"", "timeout: " + str(request_id))

        try:
            user.request_collaboration_async(
                args.service,
                encode_tensor_bundle(),
                roles=roles,
                key_scopes=key_scopes,
                dependencies=dependencies,
                scope_key_data_names=scope_key_data_names,
                role_scopes=role_scopes,
                on_response=on_response,
                on_timeout=on_timeout,
                ack_timeout_ms=args.ack_timeout_ms,
                timeout_ms=args.timeout_ms,
            )
        except Exception as exc:
            state["inFlight"] -= 1
            results[index] = {
                "status": "failed",
                "service": args.service,
                "requestIndex": index,
                "requestCount": args.requests,
                "concurrency": args.concurrency,
                "responseStatus": False,
                "payloadBytes": 0,
                "error": str(exc),
                "elapsedMs": (time.perf_counter() - starts[index]) * 1000.0,
            }
            state["completed"] += 1

    def submit_locked() -> None:
        while (state["inFlight"] < args.concurrency and
               state["next"] <= args.requests):
            index = state["next"]
            state["next"] += 1
            submit_one_locked(index)

    deadline = time.perf_counter() + (
        ((args.timeout_ms + 3000) / 1000.0) *
        max(1, math.ceil(args.requests / max(1, args.concurrency))) + 10.0)
    print(
        "NDNSF_DI_NATIVE_TRACER_USER_ASYNC_WAIT "
        + json.dumps({
            "requestCount": args.requests,
            "concurrency": args.concurrency,
            "deadlineSeconds": round(deadline - time.perf_counter(), 3),
        }, sort_keys=True),
        flush=True,
    )
    with condition:
        submit_locked()
        while state["completed"] < args.requests and time.perf_counter() < deadline:
            condition.wait(timeout=0.1)

    for index in range(1, args.requests + 1):
        if index not in results:
            result = {
                "status": "failed",
                "service": args.service,
                "requestIndex": index,
                "requestCount": args.requests,
                "concurrency": args.concurrency,
                "responseStatus": False,
                "payloadBytes": 0,
                "error": "local workload deadline",
                "elapsedMs": (time.perf_counter() - starts.get(index, time.perf_counter())) * 1000.0,
            }
            print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " + json.dumps(result, sort_keys=True), flush=True)
            results[index] = result
    user.stop()
    return [results[index] for index in sorted(results)]


def run_threaded_requests(users: list[ServiceUser],
                          args,
                          roles: list[dict],
                          key_scopes: dict[str, list[str]],
                          dependencies: list[dict],
                          scope_key_data_names: dict[str, str],
                          role_scopes: dict[str, list[str]]) -> list[dict]:
    next_index = 1
    index_lock = threading.Lock()

    def next_request_index() -> Optional[int]:
        nonlocal next_index
        with index_lock:
            if next_index > args.requests:
                return None
            index = next_index
            next_index += 1
            return index

    def worker_loop(worker_index: int, worker_user: ServiceUser) -> list[dict]:
        worker_results: list[dict] = []
        while True:
            index = next_request_index()
            if index is None:
                return worker_results
            print(
                "NDNSF_DI_NATIVE_TRACER_USER_SUBMIT "
                + json.dumps({
                    "requestIndex": index,
                    "requestCount": args.requests,
                    "concurrency": args.concurrency,
                    "workerIndex": worker_index,
                    "mode": "threaded-service-user",
                }, sort_keys=True),
                flush=True,
            )
            result = run_one_request(
                worker_user,
                args,
                roles,
                key_scopes,
                dependencies,
                scope_key_data_names,
                role_scopes,
                index)
            print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " + json.dumps(result, sort_keys=True), flush=True)
            worker_results.append(result)

    results: list[dict] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(users)) as executor:
        futures = [
            executor.submit(worker_loop, worker_index, worker_user)
            for worker_index, worker_user in enumerate(users)
        ]
        for future in concurrent.futures.as_completed(futures):
            results.extend(future.result())
    return sorted(results, key=lambda item: int(item.get("requestIndex", 0)))


def run_child_process_requests(args,
                               scope_key_data_names: dict[str, str]) -> list[dict]:
    script = Path(__file__).resolve()
    scope_json = json.dumps(scope_key_data_names, sort_keys=True)
    child_log_dir = (Path(args.plan).resolve().parents[1] / "logs") if args.plan else None
    if child_log_dir is not None:
        child_log_dir.mkdir(parents=True, exist_ok=True)
    admission_providers = [
        item.strip()
        for item in args.burst_admission_providers.split(",")
        if item.strip()
    ]

    def admission_bias_for_index(index: int) -> str:
        if not admission_providers:
            return ""
        counts = {provider: 0 for provider in admission_providers}
        for offset in range(max(0, index - 1)):
            provider = admission_providers[offset % len(admission_providers)]
            counts[provider] += 1
        return ";".join(
            f"{provider}={count}"
            for provider, count in counts.items()
            if count > 0
        )

    def role_provider_preference_for_index(index: int) -> str:
        if not admission_providers:
            return ""
        provider = admission_providers[(index - 1) % len(admission_providers)]
        return f"/Backbone=>{provider};Backbone=>{provider}"

    def run_child(index: int) -> dict:
        if args.submission_spacing_ms > 0:
            time.sleep(((index - 1) * args.submission_spacing_ms) / 1000.0)
        child_home = Path(tempfile.mkdtemp(prefix=f"ndnsf-di-user-{index}-"))
        parent_ndn_dir = Path(os.environ.get("HOME", "")).expanduser() / ".ndn"
        child_ndn_dir = child_home / ".ndn"
        if parent_ndn_dir.exists():
            shutil.copytree(parent_ndn_dir, child_ndn_dir)
        child_env = os.environ.copy()
        child_env["HOME"] = str(child_home)
        if (child_ndn_dir / "client.conf").exists():
            child_env["NDN_CLIENT_CONF"] = str(child_ndn_dir / "client.conf")
        admission_bias = admission_bias_for_index(index)
        if admission_bias:
            child_env["NDNSF_COLLAB_ADMISSION_BIAS"] = admission_bias
        role_provider_preference = role_provider_preference_for_index(index)
        if role_provider_preference:
            child_env["NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE"] = role_provider_preference

        def cleanup_child_home() -> None:
            try:
                shutil.rmtree(child_home)
            except Exception:
                pass

        command = [
            sys.executable,
            str(script),
            "--plan", args.plan,
            "--service", args.service,
            "--group", args.group,
            "--controller", args.controller,
            "--user", f"{args.user}/worker/{index}",
            "--trust-schema", args.trust_schema,
            "--ack-timeout-ms", str(args.ack_timeout_ms),
            "--timeout-ms", str(args.timeout_ms),
            "--permission-wait-ms", str(args.permission_wait_ms),
            "--requests", str(args.requests),
            "--concurrency", str(args.concurrency),
            "--worker-child",
            "--request-index", str(index),
            "--scope-key-data-names-json", scope_json,
        ]
        print(
            "NDNSF_DI_NATIVE_TRACER_USER_SUBMIT "
            + json.dumps({
                "admissionBias": admission_bias,
                "roleProviderPreference": role_provider_preference,
                "requestIndex": index,
                "requestCount": args.requests,
                "concurrency": args.concurrency,
                "mode": "child-process-service-user",
            }, sort_keys=True),
            flush=True,
        )
        started = time.perf_counter()
        child_log = child_log_dir / f"user-worker-{index}.log" if child_log_dir is not None else None

        def write_child_log(output: str) -> None:
            if child_log is None:
                return
            child_log.write_text(output, encoding="utf-8", errors="replace")

        try:
            completed = subprocess.run(
                command,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                env=child_env,
                timeout=(args.timeout_ms / 1000.0) + 25.0,
                check=False,
            )
        except subprocess.TimeoutExpired as exc:
            child_output = exc.stdout or ""
            if isinstance(child_output, bytes):
                child_output = child_output.decode("utf-8", errors="replace")
            write_child_log(child_output)
            cleanup_child_home()
            return {
                "status": "failed",
                "service": args.service,
                "requestIndex": index,
                "requestCount": args.requests,
                "concurrency": args.concurrency,
                "responseStatus": False,
                "payloadBytes": 0,
                "error": "child process local deadline",
                "elapsedMs": (time.perf_counter() - started) * 1000.0,
                "childOutput": child_output[-4000:],
            }

        child_output = completed.stdout or ""
        write_child_log(child_output)
        for line in child_output.splitlines():
            if line.startswith("NDNSF_DI_NATIVE_TRACER_USER_REQUEST "):
                result = json.loads(line.split(" ", 1)[1])
                result["childReturncode"] = completed.returncode
                if result.get("status") != "executed":
                    result["childOutput"] = child_output[-4000:]
                cleanup_child_home()
                return result
        cleanup_child_home()
        return {
            "status": "failed",
            "service": args.service,
            "requestIndex": index,
            "requestCount": args.requests,
            "concurrency": args.concurrency,
            "responseStatus": False,
            "payloadBytes": 0,
            "error": "child did not emit request result",
            "elapsedMs": (time.perf_counter() - started) * 1000.0,
            "childReturncode": completed.returncode,
            "childOutput": child_output[-2000:],
        }

    results: list[dict] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as executor:
        futures = [
            executor.submit(run_child, index)
            for index in range(1, args.requests + 1)
        ]
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " + json.dumps(result, sort_keys=True), flush=True)
            results.append(result)
    return sorted(results, key=lambda item: int(item.get("requestIndex", 0)))


def main() -> int:
    args = build_parser().parse_args()
    if args.requests <= 0:
        raise SystemExit("--requests must be positive")
    if args.concurrency <= 0:
        raise SystemExit("--concurrency must be positive")
    if args.concurrency > args.requests:
        args.concurrency = args.requests
    if args.plan:
        service_plan = load_service_plan(Path(args.plan), args.service)
    elif args.dry_run:
        service_plan = sample_service_plan(args.service)
    else:
        raise SystemExit("--plan is required unless --dry-run is used")
    roles = collaboration_roles(service_plan, args.service)
    dependencies = collaboration_dependencies(service_plan)
    key_scopes, role_scopes = key_scopes_and_role_scopes(service_plan)
    if args.dry_run:
        print(json.dumps({
            "service": args.service,
            "roles": roles,
            "dependencies": dependencies,
            "keyScopes": key_scopes,
            "roleScopes": role_scopes,
        }, indent=2, sort_keys=True))
        return 0

    user = ServiceUser(
        group=args.group,
        controller=args.controller,
        user=args.user,
        trust_schema=args.trust_schema,
        permission_wait_ms=args.permission_wait_ms,
        serve_certificates=True,
    )
    allowed = [entry.service for entry in user.get_allowed_services()]
    print("NDNSF_DI_NATIVE_TRACER_USER_ALLOWED " + json.dumps(allowed), flush=True)
    if args.service not in allowed:
        result = {
            "status": "failed",
            "service": args.service,
            "responseStatus": False,
            "payloadBytes": 0,
            "error": f"missing user permission for {args.service}; allowed={allowed}",
            "elapsedMs": 0.0,
        }
        print("NDNSF_DI_NATIVE_TRACER_USER_EXECUTION " + json.dumps(result, sort_keys=True), flush=True)
        return 1
    if args.worker_child:
        if not args.scope_key_data_names_json:
            raise SystemExit("--scope-key-data-names-json is required for worker children")
        scope_key_data_names = json.loads(args.scope_key_data_names_json)
        result = run_one_request(
            user,
            args,
            roles,
            key_scopes,
            dependencies,
            scope_key_data_names,
            role_scopes,
            args.request_index)
        print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " + json.dumps(result, sort_keys=True), flush=True)
        return 0 if result["status"] == "executed" else 1

    scope_key_data_names = publish_scope_keys(user, args.service, key_scopes)
    print(
        "NDNSF_DI_NATIVE_TRACER_SCOPE_KEYS "
        + json.dumps(scope_key_data_names, sort_keys=True),
        flush=True,
    )
    workload_start = time.perf_counter()
    results = []
    if args.concurrency == 1:
        for index in range(1, args.requests + 1):
            result = run_one_request(
                user,
                args,
                roles,
                key_scopes,
                dependencies,
                scope_key_data_names,
                role_scopes,
                index)
            results.append(result)
            print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " + json.dumps(result, sort_keys=True), flush=True)
            if result["status"] != "executed":
                break
    else:
        print(
            "NDNSF_DI_NATIVE_TRACER_USER_CONCURRENCY "
            + json.dumps({
                "mode": "child-process-service-user",
                "requestCount": args.requests,
                "concurrency": args.concurrency,
                "workers": args.concurrency,
            }, sort_keys=True),
            flush=True,
        )
        user.start()
        try:
            results = run_child_process_requests(args, scope_key_data_names)
        finally:
            user.stop()

    makespan_ms = (time.perf_counter() - workload_start) * 1000.0
    workload = summarize_workload(results, makespan_ms, args.service, args.concurrency)
    print("NDNSF_DI_NATIVE_TRACER_USER_WORKLOAD " + json.dumps(workload, sort_keys=True), flush=True)
    print("NDNSF_DI_NATIVE_TRACER_USER_EXECUTION " + json.dumps(workload, sort_keys=True), flush=True)
    return 0 if workload["status"] == "executed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
