#!/usr/bin/env python3
"""NDNSF runtime profile, doctor, and structured event helper.

This tool is intentionally stdlib-only so it can run inside MiniNDN nodes and
fresh VMs before optional Python dependencies are installed.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import secrets
import shlex
import shutil
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

TOKEN_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789"
DEFAULT_PROFILE = Path("examples/hello.runtime.json")
DEFAULT_DI_PROFILE = Path("examples/di-native-tracer.runtime.json")
NATIVE_TRACER_HARNESS = Path("Experiments/NDNSF_DI_NativeTracer_Minindn.py")
NATIVE_TRACER_DIR = Path("examples/python/NDNSF-DistributedInference/native_di_tracer")
NATIVE_TRACER_CAMPAIGN = NATIVE_TRACER_DIR / "run_llm_full_network_campaign.py"
NATIVE_TRACER_RATE_SWEEP = NATIVE_TRACER_DIR / "run_rate_sweep_campaign.py"
NATIVE_TRACER_RPS_SEARCH = NATIVE_TRACER_DIR / "run_llm_proportional_rps_search.py"
DI_REQUIRED_TOPOLOGY_NODES = ["memphis", "ucla", "arizona", "wustl", "neu"]
DI_REQUIRED_BINARIES = [
    "di-native-provider",
    "di-native-plan-schema-smoke",
    "di-native-plan-manifest-smoke",
    "di-native-provider-session-smoke",
]
ROOT_PROFILE_KEYS = {"name", "controller", "provider", "user", "service_name", "env", "distributed_inference", "deployment"}
CONTROLLER_KEYS = {"prefix", "policy_file", "trust_schema", "bootstrap_token_file"}
IDENTITY_KEYS = {"identity"}
DI_KEYS = {"native_tracer"}
NATIVE_TRACER_KEYS = {
    "enabled",
    "harness",
    "topology",
    "tracer_dir",
    "out",
    "assignment",
    "policy_bundle",
    "llm_planner_mode",
    "runtime_aware_user_planner",
    "requests",
    "concurrency",
    "target_rps",
    "open_loop_duration_s",
    "open_loop_driver_mode",
    "submission_spacing_ms",
    "provider_check_timeout",
    "local_execution_only",
    "full_network",
    "runtime_v1_context_tokens",
    "runtime_v1_generated_tokens",
    "runtime_v1_prefix_id",
    "provider_admission_max_queue",
    "provider_admission_max_active_workers",
    "provider_admission_min_free_memory_mb",
    "multi_user_workload",
    "runtime_aware_max_replans",
    "runtime_aware_replan_reasons",
    "core_trace",
    "tracer_deterministic_runner",
}
NATIVE_TRACER_STRING_FIELDS = {
    "harness",
    "topology",
    "tracer_dir",
    "out",
    "assignment",
    "policy_bundle",
    "llm_planner_mode",
    "open_loop_driver_mode",
    "runtime_v1_prefix_id",
    "multi_user_workload",
    "runtime_aware_replan_reasons",
}
NATIVE_TRACER_BOOL_FIELDS = {
    "enabled",
    "local_execution_only",
    "full_network",
    "runtime_aware_user_planner",
    "core_trace",
    "tracer_deterministic_runner",
}
NATIVE_TRACER_INT_FIELDS = {
    "requests",
    "concurrency",
    "submission_spacing_ms",
    "provider_check_timeout",
    "runtime_v1_context_tokens",
    "runtime_v1_generated_tokens",
    "provider_admission_max_queue",
    "provider_admission_max_active_workers",
    "runtime_aware_max_replans",
}
NATIVE_TRACER_FLOAT_FIELDS = {
    "target_rps",
    "open_loop_duration_s",
    "provider_admission_min_free_memory_mb",
}
DEPLOYMENT_KEYS = {
    "role", "identity", "nfd_endpoint", "certificate", "trust_schema",
    "release_dir", "model_manifest", "backend", "device", "writable_dirs",
    "startup_timeout_s", "shutdown_timeout_s", "telemetry_max_age_ms",
    "secret_files", "status_file", "metrics_file", "harness",
    "provider_command", "run_command",
}


def now_ms() -> int:
    return int(time.time() * 1000)


class EventWriter:
    def __init__(self, path: str | Path | None = None) -> None:
        self.path = Path(path) if path else None
        self._fh = None
        if self.path:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self._fh = self.path.open("a", encoding="utf-8")

    def emit(self, event: str, **fields: Any) -> None:
        record = {"tsMs": now_ms(), "event": event, **fields}
        line = json.dumps(record, sort_keys=True)
        if self._fh:
            self._fh.write(line + "\n")
            self._fh.flush()
        else:
            print(line, file=sys.stderr)

    def close(self) -> None:
        if self._fh:
            self._fh.close()

    def __enter__(self) -> "EventWriter":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


@dataclass
class NativeTracerProfile:
    enabled: bool = False
    harness: str = "Experiments/NDNSF_DI_NativeTracer_Minindn.py"
    topology: str = "Experiments/Topology/AI_Lab.conf"
    tracer_dir: str = "examples/python/NDNSF-DistributedInference/native_di_tracer"
    out: str = "results/native_di_real_minindn/profile-doctor"
    assignment: str = "llm-proportional"
    policy_bundle: str = "llm-proportional"
    llm_planner_mode: str = "proportional"
    runtime_aware_user_planner: bool = False
    requests: int = 1
    concurrency: int = 1
    target_rps: float = 0.0
    open_loop_duration_s: float = 0.0
    open_loop_driver_mode: str = "threaded"
    submission_spacing_ms: int = 250
    provider_check_timeout: int = 45
    local_execution_only: bool = True
    full_network: bool = False
    runtime_v1_context_tokens: int = 1024
    runtime_v1_generated_tokens: int = 32
    runtime_v1_prefix_id: str = ""
    provider_admission_max_queue: int = -1
    provider_admission_max_active_workers: int = -1
    provider_admission_min_free_memory_mb: float = 0.0
    multi_user_workload: str = ""
    runtime_aware_max_replans: int = 0
    runtime_aware_replan_reasons: str = ""
    core_trace: bool = False
    tracer_deterministic_runner: bool = False

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "NativeTracerProfile":
        if not data:
            return cls()
        return cls(
            enabled=bool(data.get("enabled", cls.enabled)),
            harness=str(data.get("harness", cls.harness)),
            topology=str(data.get("topology", cls.topology)),
            tracer_dir=str(data.get("tracer_dir", cls.tracer_dir)),
            out=str(data.get("out", cls.out)),
            assignment=str(data.get("assignment", cls.assignment)),
            policy_bundle=str(data.get("policy_bundle", cls.policy_bundle)),
            llm_planner_mode=str(data.get("llm_planner_mode", cls.llm_planner_mode)),
            runtime_aware_user_planner=bool(data.get(
                "runtime_aware_user_planner",
                cls.runtime_aware_user_planner)),
            requests=int(data.get("requests", cls.requests)),
            concurrency=int(data.get("concurrency", cls.concurrency)),
            target_rps=float(data.get("target_rps", cls.target_rps)),
            open_loop_duration_s=float(data.get("open_loop_duration_s", cls.open_loop_duration_s)),
            open_loop_driver_mode=str(data.get("open_loop_driver_mode", cls.open_loop_driver_mode)),
            submission_spacing_ms=int(data.get("submission_spacing_ms", cls.submission_spacing_ms)),
            provider_check_timeout=int(data.get("provider_check_timeout", cls.provider_check_timeout)),
            local_execution_only=bool(data.get("local_execution_only", cls.local_execution_only)),
            full_network=bool(data.get("full_network", cls.full_network)),
            runtime_v1_context_tokens=int(data.get("runtime_v1_context_tokens", cls.runtime_v1_context_tokens)),
            runtime_v1_generated_tokens=int(data.get("runtime_v1_generated_tokens", cls.runtime_v1_generated_tokens)),
            runtime_v1_prefix_id=str(data.get("runtime_v1_prefix_id", cls.runtime_v1_prefix_id)),
            provider_admission_max_queue=int(data.get("provider_admission_max_queue", cls.provider_admission_max_queue)),
            provider_admission_max_active_workers=int(
                data.get("provider_admission_max_active_workers", cls.provider_admission_max_active_workers)
            ),
            provider_admission_min_free_memory_mb=float(
                data.get("provider_admission_min_free_memory_mb", cls.provider_admission_min_free_memory_mb)
            ),
            multi_user_workload=str(data.get("multi_user_workload", cls.multi_user_workload)),
            runtime_aware_max_replans=int(data.get("runtime_aware_max_replans", cls.runtime_aware_max_replans)),
            runtime_aware_replan_reasons=str(data.get(
                "runtime_aware_replan_reasons", cls.runtime_aware_replan_reasons)),
            core_trace=bool(data.get("core_trace", cls.core_trace)),
            tracer_deterministic_runner=bool(data.get("tracer_deterministic_runner", cls.tracer_deterministic_runner)),
        )


@dataclass
class RuntimeProfile:
    name: str = "hello"
    controller_prefix: str = "/example/hello/controller"
    policy_file: str = "examples/hello.policies"
    trust_schema: str = "examples/trust-schema.conf"
    token_file: str = "examples/hello.bootstrap-tokens"
    provider_identity: str = "/example/hello/provider"
    user_identity: str = "/example/hello/user"
    service_name: str = "/HELLO"
    env: dict[str, str] = field(default_factory=dict)
    native_tracer: NativeTracerProfile = field(default_factory=NativeTracerProfile)

    @classmethod
    def from_json(cls, path: str | Path) -> "RuntimeProfile":
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        controller = data.get("controller", {})
        provider = data.get("provider", {})
        user = data.get("user", {})
        distributed_inference = data.get("distributed_inference", {})
        return cls(
            name=data.get("name", "hello"),
            controller_prefix=controller.get("prefix", data.get("controller_prefix", cls.controller_prefix)),
            policy_file=controller.get("policy_file", data.get("policy_file", cls.policy_file)),
            trust_schema=controller.get("trust_schema", data.get("trust_schema", cls.trust_schema)),
            token_file=controller.get("bootstrap_token_file", data.get("token_file", cls.token_file)),
            provider_identity=provider.get("identity", data.get("provider_identity", cls.provider_identity)),
            user_identity=user.get("identity", data.get("user_identity", cls.user_identity)),
            service_name=data.get("service_name", cls.service_name),
            env={str(k): str(v) for k, v in data.get("env", {}).items()},
            native_tracer=NativeTracerProfile.from_dict(distributed_inference.get("native_tracer", data.get("native_tracer", {}))),
        )

    def resolved(self, repo_root: Path) -> dict[str, Any]:
        def abs_path(value: str) -> str:
            path = Path(value)
            return str(path if path.is_absolute() else repo_root / path)

        return {
            "name": self.name,
            "controller": {
                "prefix": self.controller_prefix,
                "policy_file": abs_path(self.policy_file),
                "trust_schema": abs_path(self.trust_schema),
                "bootstrap_token_file": abs_path(self.token_file),
            },
            "provider": {"identity": self.provider_identity},
            "user": {"identity": self.user_identity},
            "service_name": self.service_name,
            "env": self.env,
            "distributed_inference": {
                "native_tracer": resolve_native_tracer(self.native_tracer, repo_root),
            },
        }


def repo_root_from(start: Path) -> Path:
    cur = start.resolve()
    for candidate in [cur, *cur.parents]:
        if (candidate / "README.md").exists() and (candidate / "ndn-service-framework").is_dir():
            return candidate
    raise RuntimeError(f"Cannot locate NDNSF repository root from {start}")


def load_profile_json(path: str | Path) -> dict[str, Any]:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def unknown_keys(payload: dict[str, Any], allowed: set[str], prefix: str) -> list[str]:
    return [f"{prefix}.{key}" for key in sorted(set(payload) - allowed)]


def require_object(payload: dict[str, Any], key: str, errors: list[str]) -> dict[str, Any]:
    value = payload.get(key, {})
    if value in ({}, None):
        return {}
    if not isinstance(value, dict):
        errors.append(f"{key} must be an object")
        return {}
    return value


def validate_string(payload: dict[str, Any], key: str, prefix: str, errors: list[str]) -> None:
    if key in payload and not isinstance(payload[key], str):
        errors.append(f"{prefix}.{key} must be a string")


def validate_bool(payload: dict[str, Any], key: str, prefix: str, errors: list[str]) -> None:
    if key in payload and not isinstance(payload[key], bool):
        errors.append(f"{prefix}.{key} must be a boolean")


def validate_int(payload: dict[str, Any], key: str, prefix: str, errors: list[str]) -> None:
    if key in payload and (not isinstance(payload[key], int) or isinstance(payload[key], bool)):
        errors.append(f"{prefix}.{key} must be an integer")


def validate_float(payload: dict[str, Any], key: str, prefix: str, errors: list[str]) -> None:
    if key in payload and (not isinstance(payload[key], (int, float)) or isinstance(payload[key], bool)):
        errors.append(f"{prefix}.{key} must be a number")


def validate_profile_payload(payload: dict[str, Any], require_di: bool = False) -> dict[str, Any]:
    errors: list[str] = []
    warnings: list[str] = []
    if not isinstance(payload, dict):
        return {"valid": False, "errors": ["profile root must be an object"], "warnings": warnings}

    errors.extend(unknown_keys(payload, ROOT_PROFILE_KEYS, "profile"))
    controller = require_object(payload, "controller", errors)
    provider = require_object(payload, "provider", errors)
    user = require_object(payload, "user", errors)
    distributed = require_object(payload, "distributed_inference", errors)
    deployment = require_object(payload, "deployment", errors)
    env = payload.get("env", {})

    errors.extend(unknown_keys(controller, CONTROLLER_KEYS, "controller"))
    errors.extend(unknown_keys(provider, IDENTITY_KEYS, "provider"))
    errors.extend(unknown_keys(user, IDENTITY_KEYS, "user"))
    errors.extend(unknown_keys(distributed, DI_KEYS, "distributed_inference"))
    errors.extend(unknown_keys(deployment, DEPLOYMENT_KEYS, "deployment"))
    if "env" in payload and not isinstance(env, dict):
        errors.append("env must be an object")
    elif isinstance(env, dict):
        for key, value in sorted(env.items()):
            if not isinstance(key, str) or not isinstance(value, str):
                errors.append(f"env.{key} must map string to string")

    for key in ["name", "service_name"]:
        validate_string(payload, key, "profile", errors)
    for key in CONTROLLER_KEYS:
        validate_string(controller, key, "controller", errors)
    validate_string(provider, "identity", "provider", errors)
    validate_string(user, "identity", "user", errors)
    for key in (
        "role", "identity", "nfd_endpoint", "certificate", "trust_schema",
        "release_dir", "model_manifest", "backend", "device", "status_file",
        "metrics_file", "harness",
    ):
        validate_string(deployment, key, "deployment", errors)
    for key in ("startup_timeout_s", "shutdown_timeout_s", "telemetry_max_age_ms"):
        validate_int(deployment, key, "deployment", errors)
    for key in ("writable_dirs", "secret_files", "provider_command", "run_command"):
        if key in deployment and (
            not isinstance(deployment[key], list) or
            not all(isinstance(value, str) for value in deployment[key])
        ):
            errors.append(f"deployment.{key} must be a string array")

    native = distributed.get("native_tracer", {})
    if native in ({}, None):
        if require_di:
            errors.append("distributed_inference.native_tracer is required for DI profiles")
        native = {}
    elif not isinstance(native, dict):
        errors.append("distributed_inference.native_tracer must be an object")
        native = {}
    errors.extend(unknown_keys(native, NATIVE_TRACER_KEYS, "distributed_inference.native_tracer"))
    for key in NATIVE_TRACER_STRING_FIELDS:
        validate_string(native, key, "distributed_inference.native_tracer", errors)
    for key in NATIVE_TRACER_BOOL_FIELDS:
        validate_bool(native, key, "distributed_inference.native_tracer", errors)
    for key in NATIVE_TRACER_INT_FIELDS:
        validate_int(native, key, "distributed_inference.native_tracer", errors)
    for key in NATIVE_TRACER_FLOAT_FIELDS:
        validate_float(native, key, "distributed_inference.native_tracer", errors)

    assignment = native.get("assignment")
    if assignment and assignment not in {"default", "alternate", "single-provider", "capacity-pool", "auto", "llm-proportional"}:
        errors.append("distributed_inference.native_tracer.assignment has unsupported value")
    policy_bundle = native.get("policy_bundle")
    if policy_bundle and policy_bundle not in {"native-tracer", "llm-proportional"}:
        errors.append("distributed_inference.native_tracer.policy_bundle has unsupported value")
    planner_mode = native.get("llm_planner_mode")
    if planner_mode and planner_mode not in {"greedy", "proportional"}:
        errors.append("distributed_inference.native_tracer.llm_planner_mode has unsupported value")
    driver_mode = native.get("open_loop_driver_mode")
    if driver_mode and driver_mode not in {"child", "threaded"}:
        errors.append("distributed_inference.native_tracer.open_loop_driver_mode has unsupported value")

    if require_di and native and native.get("enabled") is not True:
        warnings.append("distributed_inference.native_tracer.enabled is not true")
    return {"valid": not errors, "errors": errors, "warnings": warnings}


def deployment_preflight(payload: dict[str, Any]) -> dict[str, Any]:
    deployment = payload.get("deployment")
    if not isinstance(deployment, dict):
        return {}

    def path_status(key: str, *, directory: bool = False) -> dict[str, Any]:
        raw = deployment.get(key, "")
        path = Path(str(raw)) if raw else None
        exists = bool(path and (path.is_dir() if directory else path.is_file()))
        return {"ok": exists, "path": str(path) if path else ""}

    role = str(deployment.get("role", ""))
    identity = str(deployment.get("identity", ""))
    role_identity = {
        "ok": role in {"controller", "provider", "repo", "user", "bench"}
              and identity.startswith("/") and len(identity) > 1,
        "role": role,
        "identity": identity,
    }
    nfd_path = Path(str(deployment.get("nfd_endpoint", "")))
    nfd = {"ok": nfd_path.exists() and nfd_path.is_socket(), "endpoint": str(nfd_path)}
    backend = str(deployment.get("backend", ""))
    backend_device = {
        "ok": backend == "onnxruntime-cpu",
        "backend": backend,
        "physicalGpuEvidence": False,
    }
    writable = [Path(str(value)) for value in deployment.get("writable_dirs", [])]
    writable_status = {
        "ok": bool(writable) and all(path.is_dir() and os.access(path, os.W_OK) for path in writable),
        "paths": [str(path) for path in writable],
    }
    startup = deployment.get("startup_timeout_s", 0)
    shutdown = deployment.get("shutdown_timeout_s", 0)
    lifecycle = {
        "ok": isinstance(startup, int) and isinstance(shutdown, int)
              and 0 < startup <= 300 and 0 < shutdown <= 120,
        "startupTimeoutS": startup,
        "shutdownTimeoutS": shutdown,
    }
    telemetry_age = deployment.get("telemetry_max_age_ms", 0)
    telemetry = {
        "ok": isinstance(telemetry_age, int) and 0 < telemetry_age <= 2000,
        "source": "linux-proc",
        "maximumAgeMs": telemetry_age,
    }
    secret_paths = [Path(str(value)) for value in deployment.get("secret_files", [])]
    secrets_secure = bool(secret_paths)
    for path in secret_paths:
        if not path.is_file() or path.stat().st_mode & 0o077:
            secrets_secure = False
    release_path = Path(str(deployment.get("release_dir", ".")))
    disk_base = release_path if release_path.exists() else release_path.parent
    try:
        disk_free = shutil.disk_usage(disk_base).free
    except OSError:
        disk_free = 0
    disk_permissions = {
        "ok": secrets_secure and disk_free >= 512 * 1024 * 1024,
        "secretFileCount": len(secret_paths),
        "freeBytes": disk_free,
    }
    checks = {
        "role_identity": role_identity,
        "nfd": nfd,
        "identity_certificate": path_status("certificate"),
        "trust_schema": path_status("trust_schema"),
        "release": path_status("release_dir", directory=True),
        "backend_device": backend_device,
        "model_artifact": path_status("model_manifest"),
        "writable_dirs": writable_status,
        "lifecycle_bounds": lifecycle,
        "telemetry_probe": telemetry,
        "disk_permissions": disk_permissions,
    }
    return {
        "ready": all(value["ok"] for value in checks.values()),
        "checks": checks,
        "device": "<redacted>",
        "secret_files": ["<redacted>" for _ in secret_paths],
    }


def parse_policy_identities(policy_file: Path) -> list[tuple[str, str]]:
    text = policy_file.read_text(encoding="utf-8")
    entries: list[tuple[str, str]] = []
    for section, role in [("provider-policy", "provider"), ("user-policy", "user")]:
        pattern = re.compile(rf"{re.escape(section)}\s*\{{.*?\bfor\s+(/[^\s{{}}]+)", re.S)
        for match in pattern.finditer(text):
            entries.append((match.group(1), role))
    dedup: dict[str, str] = {}
    for identity, role in entries:
        dedup.setdefault(identity, role)
    return sorted(dedup.items())


def generate_token(length: int = 8) -> str:
    return "".join(secrets.choice(TOKEN_ALPHABET) for _ in range(length))


def ensure_token_file(policy_file: Path, token_file: Path, events: EventWriter, fix: bool) -> dict[str, Any]:
    status: dict[str, Any] = {"path": str(token_file), "exists": token_file.exists(), "generated": False}
    if token_file.exists():
        entries = read_token_file(token_file)
        bad = [entry for entry in entries if len(entry[1]) != 8]
        status.update({"entry_count": len(entries), "bad_token_count": len(bad)})
        events.emit("TOKEN_FILE_LOADED", path=str(token_file), entryCount=len(entries), badTokenCount=len(bad))
        return status

    identities = parse_policy_identities(policy_file)
    status.update({"entry_count": len(identities), "bad_token_count": 0})
    if not fix:
        events.emit("TOKEN_FILE_MISSING", path=str(token_file), policy=str(policy_file), identities=len(identities))
        return status

    token_file.parent.mkdir(parents=True, exist_ok=True)
    with token_file.open("w", encoding="utf-8") as fh:
        fh.write("# identity token role\n")
        for identity, role in identities:
            fh.write(f"{identity} {generate_token(8)} {role}\n")
    try:
        token_file.chmod(0o600)
    except OSError:
        pass
    status.update({"exists": True, "generated": True})
    events.emit("TOKEN_FILE_GENERATED", path=str(token_file), entryCount=len(identities), tokenLength=8)
    return status


def read_token_file(path: Path) -> list[tuple[str, str, str]]:
    entries: list[tuple[str, str, str]] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) >= 2:
            entries.append((parts[0], parts[1], parts[2] if len(parts) >= 3 else ""))
    return entries


def check_nfd_socket(path: Path = Path("/run/nfd/nfd.sock")) -> dict[str, Any]:
    exists = path.exists()
    is_socket = path.is_socket() if exists else False
    return {"path": str(path), "exists": exists, "is_socket": is_socket, "ready": exists and is_socket}


def check_binaries(repo_root: Path, names: Iterable[str]) -> dict[str, bool]:
    result: dict[str, bool] = {}
    for name in names:
        result[name] = (repo_root / "build" / "examples" / name).exists()
    return result


def topology_nodes(path: Path) -> list[str]:
    nodes: list[str] = []
    in_nodes = False
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            in_nodes = line == "[nodes]"
            continue
        if in_nodes and line.endswith(":"):
            nodes.append(line[:-1])
    return nodes


def resolve_native_tracer(native: NativeTracerProfile, repo_root: Path) -> dict[str, Any]:
    def abs_path(value: str) -> str:
        path = Path(value)
        return str(path if path.is_absolute() else repo_root / path)

    command = build_native_tracer_command(native)
    return {
        "enabled": native.enabled,
        "harness": abs_path(native.harness),
        "topology": abs_path(native.topology),
        "tracer_dir": abs_path(native.tracer_dir),
        "out": abs_path(native.out),
        "assignment": native.assignment,
        "policy_bundle": native.policy_bundle,
        "llm_planner_mode": native.llm_planner_mode,
        "runtime_aware_user_planner": native.runtime_aware_user_planner,
        "requests": native.requests,
        "concurrency": native.concurrency,
        "target_rps": native.target_rps,
        "open_loop_duration_s": native.open_loop_duration_s,
        "open_loop_driver_mode": native.open_loop_driver_mode,
        "submission_spacing_ms": native.submission_spacing_ms,
        "provider_check_timeout": native.provider_check_timeout,
        "local_execution_only": native.local_execution_only,
        "full_network": native.full_network,
        "runtime_v1_context_tokens": native.runtime_v1_context_tokens,
        "runtime_v1_generated_tokens": native.runtime_v1_generated_tokens,
        "runtime_v1_prefix_id": native.runtime_v1_prefix_id,
        "provider_admission_max_queue": native.provider_admission_max_queue,
        "provider_admission_max_active_workers": native.provider_admission_max_active_workers,
        "provider_admission_min_free_memory_mb": native.provider_admission_min_free_memory_mb,
        "multi_user_workload": native.multi_user_workload,
        "runtime_aware_max_replans": native.runtime_aware_max_replans,
        "runtime_aware_replan_reasons": native.runtime_aware_replan_reasons,
        "core_trace": native.core_trace,
        "tracer_deterministic_runner": native.tracer_deterministic_runner,
        "command": command,
    }


def build_native_tracer_command(native: NativeTracerProfile) -> list[str]:
    command = [
        "python3",
        native.harness,
        "--out",
        native.out,
        "--assignment",
        native.assignment,
        "--policy-bundle",
        native.policy_bundle,
        "--llm-planner-mode",
        native.llm_planner_mode,
        "--requests",
        str(native.requests),
        "--concurrency",
        str(native.concurrency),
        "--target-rps",
        str(native.target_rps),
        "--provider-check-timeout",
        str(native.provider_check_timeout),
        "--runtime-v1-context-tokens",
        str(native.runtime_v1_context_tokens),
        "--runtime-v1-generated-tokens",
        str(native.runtime_v1_generated_tokens),
    ]
    if native.open_loop_duration_s > 0:
        command.extend(["--open-loop-duration-s", str(native.open_loop_duration_s)])
    if native.open_loop_driver_mode:
        command.extend(["--open-loop-driver-mode", native.open_loop_driver_mode])
    if native.submission_spacing_ms >= 0:
        command.extend(["--submission-spacing-ms", str(native.submission_spacing_ms)])
    if native.runtime_v1_prefix_id:
        command.extend(["--runtime-v1-prefix-id", native.runtime_v1_prefix_id])
    if native.provider_admission_max_queue >= 0:
        command.extend(["--provider-admission-max-queue", str(native.provider_admission_max_queue)])
    if native.provider_admission_max_active_workers >= 0:
        command.extend(["--provider-admission-max-active-workers", str(native.provider_admission_max_active_workers)])
    if native.provider_admission_min_free_memory_mb > 0:
        command.extend(["--provider-admission-min-free-memory-mb", str(native.provider_admission_min_free_memory_mb)])
    if native.multi_user_workload:
        command.extend(["--multi-user-workload", native.multi_user_workload])
    if native.runtime_aware_max_replans > 0:
        command.extend(["--runtime-aware-max-replans", str(native.runtime_aware_max_replans)])
    if native.runtime_aware_replan_reasons:
        command.extend(["--runtime-aware-replan-reasons", native.runtime_aware_replan_reasons])
    if native.local_execution_only:
        command.append("--local-execution-only")
    if native.full_network:
        command.append("--full-network")
    if native.core_trace:
        command.append("--core-trace")
    if native.tracer_deterministic_runner:
        command.append("--tracer-deterministic-runner")
    if native.runtime_aware_user_planner:
        command.append("--runtime-aware-user-planner")
    return command


def native_tracer_preflight(repo_root: Path, resolved_native: dict[str, Any], events: EventWriter) -> dict[str, Any]:
    if not resolved_native.get("enabled", False):
        return {"enabled": False, "ready": True}

    tracer_dir = Path(resolved_native["tracer_dir"])
    required_files = {
        "harness": Path(resolved_native["harness"]),
        "topology": Path(resolved_native["topology"]),
        "plan_tracer": tracer_dir / "plan_tracer.py",
        "user_driver": tracer_dir / "user_driver.py",
        "bundle_generator": tracer_dir / "generate_llm_proportional_native_bundle.py",
        "model_spec": tracer_dir / "llm_model_spec_qwen_tiny_proportional.json",
        "provider_profiles": tracer_dir / "llm_provider_profiles_2_4_8.json",
    }
    file_status = {key: path.exists() for key, path in required_files.items()}
    binary_status = check_binaries(repo_root, DI_REQUIRED_BINARIES)
    topo_nodes = topology_nodes(required_files["topology"]) if required_files["topology"].exists() else []
    missing_nodes = [node for node in DI_REQUIRED_TOPOLOGY_NODES if node not in topo_nodes]
    ready = all(file_status.values()) and all(binary_status.values()) and not missing_nodes
    result = {
        "enabled": True,
        "ready": bool(ready),
        "files": {key: str(path) for key, path in required_files.items()},
        "file_status": file_status,
        "binaries": binary_status,
        "topology_nodes": topo_nodes,
        "missing_topology_nodes": missing_nodes,
        "command": resolved_native["command"],
    }
    events.emit(
        "DI_NATIVE_TRACER_PREFLIGHT",
        ready=bool(ready),
        missingFiles=[key for key, ok in file_status.items() if not ok],
        missingBinaries=[key for key, ok in binary_status.items() if not ok],
        missingTopologyNodes=missing_nodes,
        command=resolved_native["command"],
    )
    return result


def command_status(command: list[str]) -> dict[str, Any]:
    try:
        proc = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        return {"ok": proc.returncode == 0, "returncode": proc.returncode, "stdout": proc.stdout.strip(), "stderr": proc.stderr.strip()}
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        return {"ok": False, "error": str(exc)}


def summarize_log_markers(log_dir: Path) -> dict[str, list[str]]:
    markers = {
        "bootstrap_issued": "NDNSF_CERT_BOOTSTRAP_ISSUED",
        "bootstrap_reused": "NDNSF_CERT_BOOTSTRAP_REUSED",
        "bootstrap_refused": "NDNSF_CERT_BOOTSTRAP_REFUSED",
        "permission_ready": "Installed user permission",
        "provider_permission_ready": "Installed provider permission",
        "dkey_ready": "DK_DECRYPT_SUCCESS",
        "response_received": "Received response:",
        "negative_ack": "NEGATIVE_ACK",
    }
    result: dict[str, list[str]] = {key: [] for key in markers}
    if not log_dir.exists():
        return result
    for path in sorted(log_dir.glob("*.log")):
        try:
            lines = path.read_text(errors="ignore").splitlines()
        except OSError:
            continue
        for key, marker in markers.items():
            if any(marker in line for line in lines):
                result[key].append(path.name)
    return result


def run_doctor(args: argparse.Namespace) -> int:
    repo_root = repo_root_from(Path.cwd())
    raw_profile = load_profile_json(args.profile)
    profile = RuntimeProfile.from_json(args.profile)
    resolved = profile.resolved(repo_root)
    with EventWriter(args.event_log) as events:
        events.emit("DOCTOR_START", profile=profile.name, repoRoot=str(repo_root))
        policy_file = Path(resolved["controller"]["policy_file"])
        token_file = Path(resolved["controller"]["bootstrap_token_file"])
        checks: dict[str, Any] = {"profile": resolved}
        checks["policy_identities"] = parse_policy_identities(policy_file) if policy_file.exists() else []
        checks["token_file"] = ensure_token_file(policy_file, token_file, events, args.fix)
        checks["nfd"] = check_nfd_socket()
        checks["binaries"] = check_binaries(repo_root, ["App_ServiceController", "App_Provider", "App_User"])
        checks["commands"] = {
            "nfdc": command_status(["nfdc", "status", "show"]),
        } if args.check_commands else {}
        checks["log_summary"] = summarize_log_markers(Path(args.log_dir)) if args.log_dir else {}
        checks["distributed_inference"] = {
            "native_tracer": native_tracer_preflight(
                repo_root,
                resolved["distributed_inference"]["native_tracer"],
                events,
            )
        }
        deployment = deployment_preflight(raw_profile)
        if deployment:
            checks["deployment"] = deployment

        ready = True
        ready = ready and Path(resolved["controller"]["policy_file"]).exists()
        ready = ready and Path(resolved["controller"]["trust_schema"]).exists()
        ready = ready and checks["token_file"].get("exists", False)
        ready = ready and all(checks["binaries"].values())
        ready = ready and checks["distributed_inference"]["native_tracer"].get("ready", False)
        if deployment:
            ready = ready and deployment.get("ready", False)
        checks["ready"] = bool(ready)
        events.emit(
            "DOCTOR_RESULT",
            ready=bool(ready),
            tokenFile=checks["token_file"],
            nfd=checks["nfd"],
            binaries=checks["binaries"],
            distributedInference=checks["distributed_inference"],
        )

        if args.write_resolved:
            Path(args.write_resolved).parent.mkdir(parents=True, exist_ok=True)
            Path(args.write_resolved).write_text(json.dumps(checks, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(json.dumps(checks, indent=2, sort_keys=True))
        return 0 if ready else 1


def run_profile_validate(args: argparse.Namespace) -> int:
    payload = load_profile_json(args.profile)
    result = validate_profile_payload(payload, require_di=args.require_di)
    result["profile"] = str(args.profile)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["valid"] else 1


def run_profile_print(args: argparse.Namespace) -> int:
    repo_root = repo_root_from(Path.cwd())
    validation = validate_profile_payload(load_profile_json(args.profile), require_di=args.require_di)
    profile = RuntimeProfile.from_json(args.profile)
    payload = {
        "profile": str(args.profile),
        "validation": validation,
        "resolved": profile.resolved(repo_root),
    }
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0 if validation["valid"] else 1


def clean_remainder(args: list[str]) -> list[str]:
    if args and args[0] == "--":
        return args[1:]
    return args


def profile_args(args: argparse.Namespace) -> list[str]:
    result: list[str] = []
    if getattr(args, "profile", ""):
        result.extend(["--runtime-profile", str(args.profile)])
    if getattr(args, "resolved", ""):
        result.extend(["--runtime-resolved", str(args.resolved)])
    return result


def di_command(script: Path, args: argparse.Namespace, extra: list[str]) -> list[str]:
    return [sys.executable, str(script), *profile_args(args), *clean_remainder(extra)]


def run_di_command(label: str, command: list[str], args: argparse.Namespace) -> int:
    payload = {
        "label": label,
        "cwd": str(repo_root_from(Path.cwd())),
        "command": command,
        "shell": shlex.join(command),
    }
    if getattr(args, "dry_run", False):
        print(json.dumps(payload, indent=2, sort_keys=True))
        return 0
    print(json.dumps({"event": "DI_COMMAND_START", **payload}, sort_keys=True), file=sys.stderr)
    completed = subprocess.run(command, cwd=payload["cwd"], check=False)
    print(
        json.dumps(
            {"event": "DI_COMMAND_RESULT", "label": label, "returncode": completed.returncode},
            sort_keys=True,
        ),
        file=sys.stderr,
    )
    return int(completed.returncode)


def run_di_native(args: argparse.Namespace) -> int:
    command = di_command(NATIVE_TRACER_HARNESS, args, args.extra_args)
    return run_di_command("native-tracer-run", command, args)


def run_di_campaign(args: argparse.Namespace) -> int:
    command = di_command(NATIVE_TRACER_CAMPAIGN, args, args.extra_args)
    return run_di_command("native-tracer-campaign", command, args)


def run_di_sweep(args: argparse.Namespace) -> int:
    command = di_command(NATIVE_TRACER_RATE_SWEEP, args, args.extra_args)
    return run_di_command("native-tracer-rate-sweep", command, args)


def run_di_search(args: argparse.Namespace) -> int:
    command = di_command(NATIVE_TRACER_RPS_SEARCH, args, args.extra_args)
    return run_di_command("native-tracer-rps-search", command, args)


def execution_evidence_view(summary: dict[str, Any]) -> dict[str, Any]:
    """Return the provider-observed execution identity from a run summary.

    Legacy runnerMode is deliberately not consulted: it is only a derived
    compatibility field and cannot establish real compute.
    """
    records = summary.get("executionEvidence", [])
    if not isinstance(records, list):
        records = []
    classification = str(summary.get("runnerClassification", "invalid-evidence"))
    valid_records = [item for item in records if isinstance(item, dict)]
    return {
        "status": "available" if valid_records and classification != "invalid-evidence" else "missing",
        "runnerClassification": classification,
        "providerCount": len({str(item.get("providerName", "")) for item in valid_records}),
        "executionEvidence": valid_records,
    }


def run_di_evidence(args: argparse.Namespace) -> int:
    try:
        summary = json.loads(Path(args.summary).read_text(encoding="utf-8"))
        if not isinstance(summary, dict):
            raise ValueError("summary root must be an object")
        view = execution_evidence_view(summary)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(json.dumps({"status": "unreadable", "error": str(exc)}, sort_keys=True))
        return 2
    print(json.dumps(view, indent=2, sort_keys=True))
    return 0 if view["status"] == "available" else 2


def add_di_launcher_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--profile", default=str(DEFAULT_DI_PROFILE),
                        help="Runtime profile to pass as --runtime-profile")
    parser.add_argument("--resolved", default="",
                        help="Resolved doctor JSON to pass as --runtime-resolved")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print the underlying command without executing it")
    parser.add_argument("extra_args", nargs=argparse.REMAINDER,
                        help="Arguments after -- are passed to the underlying script")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="NDNSF runtime profile doctor")
    sub = parser.add_subparsers(dest="command", required=True)
    profile_parser = sub.add_parser("profile", help="validate and print runtime profiles")
    profile_sub = profile_parser.add_subparsers(dest="profile_command", required=True)
    profile_validate = profile_sub.add_parser("validate", help="validate a runtime profile schema")
    profile_validate.add_argument("--profile", default=str(DEFAULT_PROFILE))
    profile_validate.add_argument("--require-di", action="store_true",
                                  help="require distributed_inference.native_tracer")
    profile_validate.set_defaults(func=run_profile_validate)

    profile_print = profile_sub.add_parser("print", help="print a resolved runtime profile")
    profile_print.add_argument("--profile", default=str(DEFAULT_PROFILE))
    profile_print.add_argument("--require-di", action="store_true",
                               help="require distributed_inference.native_tracer")
    profile_print.set_defaults(func=run_profile_print)

    doctor = sub.add_parser("doctor", help="validate a runtime profile and emit structured events")
    doctor.add_argument("--profile", default=str(DEFAULT_PROFILE))
    doctor.add_argument("--fix", action="store_true", help="create missing generated files such as bootstrap tokens")
    doctor.add_argument("--event-log", default="")
    doctor.add_argument("--write-resolved", default="")
    doctor.add_argument("--log-dir", default="")
    doctor.add_argument("--check-commands", action="store_true")
    doctor.set_defaults(func=run_doctor)

    di = sub.add_parser("di", help="distributed-inference runtime profile entrypoints")
    di_sub = di.add_subparsers(dest="di_command", required=True)

    di_doctor = di_sub.add_parser("doctor", help="preflight the DI NativeTracer runtime profile")
    di_doctor.add_argument("--profile", default=str(DEFAULT_DI_PROFILE))
    di_doctor.add_argument("--fix", action="store_true", help="create missing generated files such as bootstrap tokens")
    di_doctor.add_argument("--event-log", default="")
    di_doctor.add_argument("--write-resolved", default="")
    di_doctor.add_argument("--log-dir", default="")
    di_doctor.add_argument("--check-commands", action="store_true")
    di_doctor.set_defaults(func=run_doctor)

    di_validate = di_sub.add_parser("validate", help="validate the DI NativeTracer runtime profile")
    di_validate.add_argument("--profile", default=str(DEFAULT_DI_PROFILE))
    di_validate.set_defaults(func=run_profile_validate, require_di=True)

    di_print = di_sub.add_parser("print", help="print the resolved DI NativeTracer runtime profile")
    di_print.add_argument("--profile", default=str(DEFAULT_DI_PROFILE))
    di_print.set_defaults(func=run_profile_print, require_di=True)

    di_run = di_sub.add_parser("run", help="launch the NativeTracer harness")
    add_di_launcher_args(di_run)
    di_run.set_defaults(func=run_di_native)

    di_campaign = di_sub.add_parser("campaign", help="launch the LLM full-network campaign runner")
    add_di_launcher_args(di_campaign)
    di_campaign.set_defaults(func=run_di_campaign)

    di_sweep = di_sub.add_parser("sweep", help="launch the NativeTracer rate sweep helper")
    add_di_launcher_args(di_sweep)
    di_sweep.set_defaults(func=run_di_sweep)

    di_search = di_sub.add_parser("search", help="launch the LLM proportional RPS search helper")
    add_di_launcher_args(di_search)
    di_search.set_defaults(func=run_di_search)

    di_evidence = di_sub.add_parser(
        "evidence", help="read provider-observed execution evidence from summary.json")
    di_evidence.add_argument("--summary", required=True)
    di_evidence.set_defaults(func=run_di_evidence)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
