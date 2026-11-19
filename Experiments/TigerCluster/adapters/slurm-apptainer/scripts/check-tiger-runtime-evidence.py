#!/usr/bin/env python3
"""Fail-closed runner/analyzer for Spec 168 local deployment gates."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from typing import Any, Mapping

from ndnsf_distributed_inference.core.contracts import LifecycleEventV1


RUNTIME_SCHEMA = "ndnsf-di.spec168-runtime-admission.v1"
GATE_SCHEMA = "ndnsf-di.spec168-local-gate.v1"
REQUIRED_ROLES = {
    "controller", "repository", "user",
    "provider-stage-0", "provider-stage-1", "provider-stage-2",
}


class GateError(RuntimeError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def canonical_bytes(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(4 << 20):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise GateError("ANALYZER_RUNTIME_MANIFEST_INVALID", str(exc)) from exc
    if not isinstance(value, dict):
        raise GateError("ANALYZER_RUNTIME_MANIFEST_INVALID",
                        "runtime admission manifest must be an object")
    return value


def require_bool(section: Mapping[str, Any], key: str, expected: bool) -> None:
    if section.get(key) is not expected:
        raise GateError("ANALYZER_DEPLOYMENT_FIDELITY_MISMATCH",
                        f"{key} must be {expected}")


def _contained_file(output_dir: Path, value: object, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise GateError("ANALYZER_EVIDENCE_PATH_INVALID", f"missing {label}")
    path = (output_dir / value).resolve()
    try:
        path.relative_to(output_dir.resolve())
    except ValueError as exc:
        raise GateError("ANALYZER_EVIDENCE_PATH_INVALID",
                        f"{label} escapes output directory") from exc
    if not path.is_file():
        raise GateError("ANALYZER_EVIDENCE_MISSING", f"missing {label}: {path}")
    return path


def validate_lifecycle(path: Path, invocation: Mapping[str, Any]) -> dict[str, Any]:
    events = []
    line_number = 0
    try:
        for line_number, line in enumerate(
                path.read_text(encoding="utf-8").splitlines(), 1):
            if not line.strip():
                continue
            payload = json.loads(line)
            events.append(LifecycleEventV1.from_dict(payload))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError) as exc:
        raise GateError("ANALYZER_LIFECYCLE_INVALID",
                        f"invalid lifecycle JSONL near line {line_number}: {exc}") from exc
    if not events:
        raise GateError("ANALYZER_LIFECYCLE_MISSING", "no lifecycle events")
    request_ids = {item.request_id for item in events}
    attempts = {item.attempt_epoch for item in events}
    experiments = {item.experiment_id for item in events}
    if request_ids != {invocation.get("requestId")} or attempts != {
            invocation.get("attemptEpoch")} or len(experiments) != 1:
        raise GateError("ANALYZER_LIFECYCLE_BINDING_MISMATCH",
                        "lifecycle request/attempt/experiment changed")
    if any(not item.authenticated for item in events):
        raise GateError("ANALYZER_UNAUTHENTICATED_EVENT",
                        "runtime supplied unauthenticated accepted evidence")
    event_types = [item.event_type for item in events]
    required = {
        "REQUEST_CREATED", "REQUEST_PUBLISHED", "ACK_CLOSED",
        "GRAPH_INSPECTED", "PLAN_VALIDATED", "PLAN_COMMITTED",
        "FINAL_SELECTION", "ROLE_ASSIGNED", "LOCAL_READY",
        "STAGE_EXECUTING", "STAGE_COMPLETED", "RESPONSE_PUBLISHED",
    }
    missing = sorted(required - set(event_types))
    if missing:
        raise GateError("ANALYZER_LIFECYCLE_INCOMPLETE",
                        f"missing lifecycle events: {missing}")
    expected_roles = {"stage-0", "stage-1", "stage-2"}
    assignments = {
        item.role: (item.provider, item.provider_boot_epoch)
        for item in events if item.event_type == "ROLE_ASSIGNED"
    }
    if set(assignments) != expected_roles or len(set(assignments.values())) != 3:
        raise GateError("ANALYZER_ROLE_COVERAGE_INCOMPLETE",
                        "three distinct Provider-role assignments are required")
    for role in sorted(expected_roles):
        for kind in ("LOCAL_READY", "STAGE_EXECUTING", "STAGE_COMPLETED"):
            if not any(item.event_type == kind and item.role == role
                       and (item.provider, item.provider_boot_epoch)
                       == assignments[role] for item in events):
                raise GateError("ANALYZER_ROLE_COVERAGE_INCOMPLETE",
                                f"{role} is missing bound {kind}")
    plan_digests = {item.plan_digest for item in events
                    if item.plan_digest is not None}
    if len(plan_digests) != 1:
        raise GateError("ANALYZER_PLAN_BINDING_MISMATCH",
                        "lifecycle must bind one committed plan")
    terminals = [kind for kind in event_types if kind in {
        "RESPONSE_PUBLISHED", "FAILURE_PUBLISHED", "CANCELED", "EXPIRED"}]
    if terminals != ["RESPONSE_PUBLISHED"]:
        raise GateError("ANALYZER_TERMINAL_MISMATCH",
                        f"expected one Response terminal, got {terminals}")
    return {
        "eventCount": len(events),
        "eventTypes": event_types,
        "requestId": next(iter(request_ids)),
        "attemptEpoch": next(iter(attempts)),
        "sha256": digest_file(path),
    }


def validate_runtime_manifest(
    output_dir: Path, payload: Mapping[str, Any], *, mode: str,
    expected_container_digest: str = "", require_cuda: bool = False,
) -> dict[str, Any]:
    if payload.get("schema") != RUNTIME_SCHEMA:
        raise GateError("ANALYZER_RUNTIME_SCHEMA_MISMATCH",
                        f"expected {RUNTIME_SCHEMA}")
    expected_fidelity = "REAL_MININDN" if mode == "real-minindn" else "EXACT_SIF"
    if payload.get("fidelity") != expected_fidelity:
        raise GateError("ANALYZER_FIDELITY_MISMATCH",
                        f"expected fidelity {expected_fidelity}")
    if payload.get("simulatedComponents") != []:
        raise GateError("ANALYZER_SIMULATION_FORBIDDEN",
                        "simulated components cannot authorize Gate B/C")
    for key in ("sourceDigest", "modelIdentityDigest", "workloadDigest"):
        value = payload.get(key)
        if (not isinstance(value, str) or not value.startswith("sha256:")
                or len(value) != 71):
            raise GateError("ANALYZER_IDENTITY_BINDING_MISSING",
                            f"canonical {key} is required")

    processes = payload.get("processes")
    if not isinstance(processes, list):
        raise GateError("ANALYZER_PROCESS_PROVENANCE_INVALID",
                        "process list is required")
    roles = {str(item.get("role", "")) for item in processes
             if isinstance(item, dict)}
    pids = [item.get("pid") for item in processes if isinstance(item, dict)]
    if not REQUIRED_ROLES.issubset(roles) or any(
            not isinstance(pid, int) or pid <= 1 for pid in pids):
        raise GateError("ANALYZER_PROCESS_PROVENANCE_INVALID",
                        "independent Controller/Repo/User/three Providers required")
    required_pids = [item["pid"] for item in processes
                     if item.get("role") in REQUIRED_ROLES]
    if len(required_pids) != len(set(required_pids)):
        raise GateError("ANALYZER_PROCESS_PROVENANCE_INVALID",
                        "required roles must use independent processes")

    network = payload.get("network", {})
    security = payload.get("security", {})
    delivery = payload.get("artifactDelivery", {})
    readiness = payload.get("readiness", {})
    adapter = payload.get("adapter", {})
    invocation = payload.get("invocation", {})
    for section, label in ((network, "network"), (security, "security"),
                           (delivery, "artifactDelivery"),
                           (readiness, "readiness"), (adapter, "adapter"),
                           (invocation, "invocation")):
        if not isinstance(section, dict):
            raise GateError("ANALYZER_RUNTIME_MANIFEST_INVALID",
                            f"{label} must be an object")

    require_bool(network, "realMiniNdn", True)
    require_bool(network, "realNfdPerNode", True)
    require_bool(network, "hostNfdUsed", False)
    route_digest = network.get("routeSnapshotDigest")
    if (not isinstance(route_digest, str)
            or not route_digest.startswith("sha256:")
            or len(route_digest) != 71):
        raise GateError("ANALYZER_ROUTE_EVIDENCE_MISSING",
                        "route snapshot digest is required")

    for key in ("normalPermissions", "nacAbe", "userToken",
                "providerToken", "replayProtection"):
        require_bool(security, key, True)
    require_bool(security, "testOnlyIdentities", False)
    require_bool(security, "bypassEnabled", False)

    if delivery.get("transport") != "NDNSF-DistributedRepo":
        raise GateError("ANALYZER_REPO_TRANSPORT_MISSING",
                        "model bytes must use NDNSF-DistributedRepo")
    require_bool(delivery, "throughNdn", True)
    require_bool(delivery, "sharedFilesystemPayloadInjection", False)
    if int(delivery.get("uniqueBytes", 0)) <= 0:
        raise GateError("ANALYZER_REPO_BYTES_MISSING",
                        "real unique model bytes were not transferred")

    if readiness.get("mode") != "event-driven" or int(
            readiness.get("fixedSettleWaitMs", -1)) != 0:
        raise GateError("ANALYZER_FIXED_SETTLE_WAIT_FORBIDDEN",
                        "readiness must be event-driven with zero settle wait")
    if not str(adapter.get("name", "")).lower().startswith("qwen"):
        raise GateError("ANALYZER_QWEN_ADAPTER_MISSING",
                        "real Qwen adapter evidence is required")
    require_bool(adapter, "mocked", False)
    device_class = str(adapter.get("deviceClass", ""))
    if device_class not in {"CPU_LOGIC", "CUDA"}:
        raise GateError(
            "ANALYZER_RUNTIME_DEVICE_CLASS_MISSING",
            "adapter deviceClass must explicitly be CPU_LOGIC or CUDA")
    if require_cuda and device_class != "CUDA":
        raise GateError(
            "ANALYZER_CUDA_ACCEPTANCE_REQUIRED",
            "this admission mode requires CUDA rather than CPU logic fidelity")
    backend = str(adapter.get("backend", ""))
    expected_backend_suffix = "-cuda" if device_class == "CUDA" else "-cpu"
    if not backend.endswith(expected_backend_suffix):
        raise GateError("ANALYZER_RUNTIME_BACKEND_MISMATCH",
                        "adapter backend does not match its declared deviceClass")
    assignments = adapter.get("assignments")
    if not isinstance(assignments, list):
        raise GateError("ANALYZER_ADAPTER_ASSIGNMENT_INVALID",
                        "per-role adapter assignments are required")
    expected_roles = {"stage-0", "stage-1", "stage-2"}
    assignment_by_role = {
        str(item.get("role", "")): item
        for item in assignments if isinstance(item, dict)
    }
    if (set(assignment_by_role) != expected_roles
            or len(assignments) != len(expected_roles)):
        raise GateError("ANALYZER_ADAPTER_ASSIGNMENT_INVALID",
                        "exactly one adapter assignment per stage is required")
    delivered_digests = delivery.get("artifactDigests")
    if not isinstance(delivered_digests, dict):
        raise GateError("ANALYZER_ARTIFACT_BINDING_MISMATCH",
                        "Repository artifact digests are required")
    for role in sorted(expected_roles):
        assignment = assignment_by_role[role]
        device = str(assignment.get("device", ""))
        exact_device = (
            device.startswith("cuda:") and device[5:].isdigit()
            if device_class == "CUDA" else device == "cpu"
        )
        if not exact_device:
            raise GateError("ANALYZER_RUNTIME_DEVICE_MISMATCH",
                            f"{role} device does not match {device_class}")
        if (assignment.get("loadCompleted") is not True
                or assignment.get("warmupCompleted") is not True):
            raise GateError("ANALYZER_RUNTIME_PREPARATION_INCOMPLETE",
                            f"{role} did not complete load and warmup")
        if int(assignment.get("cpuFallbackCount", -1)) != 0:
            raise GateError("ANALYZER_CPU_FALLBACK",
                            f"{role} used CPU fallback")
        artifact_digest = str(assignment.get("artifactDigest", ""))
        if (artifact_digest != str(delivered_digests.get(role, ""))
                or not artifact_digest.startswith("sha256:")
                or len(artifact_digest) != 71):
            raise GateError("ANALYZER_ARTIFACT_BINDING_MISMATCH",
                            f"{role} runtime artifact differs from Repo delivery")

    if (int(invocation.get("wireRequestCount", -1)) != 1
            or int(invocation.get("tokenRequestCount", -1)) != 0):
        raise GateError("ANALYZER_INVOCATION_FRAGMENTED",
                        "one collaboration must own every generated token")
    require_bool(invocation, "completeResponse", True)
    if int(invocation.get("tokenCount", 0)) < 2:
        raise GateError("ANALYZER_RESPONSE_INCOMPLETE",
                        "complete multi-token response is required")
    if int(invocation.get("cpuFallbackCount", -1)) != 0:
        raise GateError("ANALYZER_CPU_FALLBACK", "CPU fallback is forbidden")

    if mode == "exact-sif":
        container = payload.get("container", {})
        if (not isinstance(container, dict)
                or container.get("runtime") != "apptainer"
                or container.get("sifDigest") != expected_container_digest):
            raise GateError("ANALYZER_CONTAINER_IDENTITY_MISMATCH",
                            "runtime did not bind the exact SIF digest")

    evidence = payload.get("evidence", {})
    if not isinstance(evidence, dict):
        raise GateError("ANALYZER_EVIDENCE_PATH_INVALID", "evidence is required")
    lifecycle_path = _contained_file(
        output_dir, evidence.get("lifecycleJsonl"), "lifecycleJsonl")
    lifecycle = validate_lifecycle(lifecycle_path, invocation)
    return {
        "runtimeManifestDigest": "sha256:" + hashlib.sha256(
            canonical_bytes(payload)).hexdigest(),
        "lifecycle": lifecycle,
        "requestId": invocation["requestId"],
        "tokenCount": int(invocation["tokenCount"]),
        "repoUniqueBytes": int(delivery["uniqueBytes"]),
        "deviceClass": device_class,
    }


def atomic_write(path: Path, payload: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    temporary.write_bytes(canonical_bytes(payload) + b"\n")
    os.replace(temporary, path)


def run(args: argparse.Namespace) -> int:
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    result_path = output_dir / "gate-manifest.json"
    checkpoint_path = output_dir / "gate-checkpoint.json"
    runtime_path = output_dir / "runtime-admission.json"
    log_path = output_dir / "launcher.log"
    started_ns = time.monotonic_ns()
    status = "BLOCK"
    failure_code = ""
    failure_message = ""
    validation: dict[str, Any] = {}
    return_code: int | None = None
    command = list(args.command)
    if command[:1] == ["--"]:
        command = command[1:]
    try:
        if result_path.exists():
            raise GateError("ANALYZER_DUPLICATE_RESULT_WRITER",
                            "gate result already exists")
        if not command:
            raise GateError("ANALYZER_COMMAND_MISSING", "runtime command is required")
        command_digest = "sha256:" + hashlib.sha256(
            canonical_bytes(command)).hexdigest()
        atomic_write(checkpoint_path, {
            "schema": "ndnsf-di.spec168-gate-checkpoint.v1",
            "gate": "B" if args.mode == "real-minindn" else "C",
            "mode": args.mode,
            "status": "STARTED",
            "automaticRetry": False,
            "command": command,
            "commandDigest": command_digest,
            "runtimeManifest": str(runtime_path),
            "launcherLog": str(log_path),
        })
        environment = os.environ.copy()
        environment["NDNSF_SPEC168_ADMISSION_OUTPUT"] = str(runtime_path)
        with log_path.open("wb") as log:
            try:
                process = subprocess.Popen(
                    command, stdout=log, stderr=subprocess.STDOUT,
                    env=environment, start_new_session=True)
            except OSError as exc:
                raise GateError("EXEC_LOCAL_GATE_LAUNCH_FAILED", str(exc)) from exc
            try:
                return_code = process.wait(timeout=args.hard_timeout_s)
            except subprocess.TimeoutExpired as exc:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                raise GateError("EXEC_LOCAL_GATE_HARD_DEADLINE",
                                "runtime exceeded immutable hard deadline") from exc
        if return_code != 0:
            raise GateError("EXEC_LOCAL_GATE_PROCESS_FAILED",
                            f"runtime exited with {return_code}")
        if not runtime_path.is_file():
            raise GateError("ANALYZER_RUNTIME_MANIFEST_MISSING",
                            "runtime did not emit runtime-admission.json")
        validation = validate_runtime_manifest(
            output_dir, load_object(runtime_path), mode=args.mode,
            expected_container_digest=args.expected_container_digest,
            require_cuda=args.require_cuda)
        status = "PASS"
    except GateError as exc:
        failure_code = exc.code
        failure_message = str(exc)
    result = {
        "schema": GATE_SCHEMA,
        "gate": "B" if args.mode == "real-minindn" else "C",
        "mode": args.mode,
        "requireCuda": args.require_cuda,
        "status": status,
        "failureCode": failure_code or None,
        "failureMessage": failure_message or None,
        "automaticRetry": False,
        "returnCode": return_code,
        "command": command,
        "commandDigest": "sha256:" + hashlib.sha256(
            canonical_bytes(command)).hexdigest(),
        "elapsedMs": round((time.monotonic_ns() - started_ns) / 1_000_000, 3),
        "runtimeManifest": str(runtime_path),
        "launcherLog": str(log_path),
        "validation": validation,
    }
    atomic_write(result_path, result)
    if checkpoint_path.is_file():
        atomic_write(checkpoint_path, {
            "schema": "ndnsf-di.spec168-gate-checkpoint.v1",
            "gate": result["gate"],
            "mode": args.mode,
            "status": "FINALIZED",
            "automaticRetry": False,
            "command": command,
            "commandDigest": result["commandDigest"],
            "runtimeManifest": str(runtime_path),
            "launcherLog": str(log_path),
            "gateManifest": str(result_path),
            "gateStatus": status,
        })
    print(json.dumps(result, sort_keys=True))
    return 0 if status == "PASS" else 1


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser()
    value.add_argument("--mode", required=True,
                       choices=("real-minindn", "exact-sif"))
    value.add_argument("--output-dir", required=True)
    value.add_argument("--hard-timeout-s", type=int, default=900)
    value.add_argument("--expected-container-digest", default="")
    value.add_argument("--require-cuda", action="store_true")
    value.add_argument("command", nargs=argparse.REMAINDER)
    return value


if __name__ == "__main__":
    raise SystemExit(run(parser().parse_args()))
