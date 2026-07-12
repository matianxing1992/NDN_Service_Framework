"""Runtime v1 contracts for NDNSF distributed inference.

The classes here are intentionally transport agnostic. They define typed
provider capability, telemetry, reusable plan, and long-context state objects
that can be consumed by Python planners, MiniNDN experiments, and later native
runtime paths.
"""

from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import itertools
import json
import os
import subprocess
import sys
import time
import zlib
from dataclasses import asdict, dataclass, field, is_dataclass, replace
from enum import Enum
from pathlib import Path
from typing import Any, Callable, Iterable

from ndnsf.runtime_telemetry import (
    AdmissionLeaseStatus,
    DeploymentStatus as CoreDeploymentStatus,
    GenericAckMetadata,
    GenericAdmissionLease,
    GenericLeaseValidationResult,
    GenericProviderRuntimeHint,
    PeerNetworkMetric,
    ProviderAdmissionLeaseTable,
    ProviderCapabilityHint as CoreProviderCapabilityHint,
    ProviderNetworkMatrix,
    _string_list,
    encode_ack_metadata,
    encode_provider_capability_ack,
    now_ms,
    parse_ack_metadata,
    read_json,
    stable_digest,
    stable_json,
    to_plain,
    write_json,
)


class ContextObjectKind(str, Enum):
    PROMPT_CHUNK = "PromptChunk"
    PREFIX_STATE = "PrefixState"
    SESSION_STATE = "SessionState"
    KV_BLOCK = "KvBlock"
    GENERATION_CHUNK = "GenerationChunk"


class CacheEventKind(str, Enum):
    HIT = "hit"
    MISS = "miss"
    EVICT = "evict"
    PIN = "pin"
    EXPIRE = "expire"


class RunnerKind(str, Enum):
    SYNTHETIC_DELAY = "synthetic-delay"
    WIRING_ONLY = "wiring-only"
    ONNXRUNTIME_CPU = "onnxruntime-cpu"
    ONNXRUNTIME_CUDA = "onnxruntime-cuda"
    TRANSFORMERS = "transformers"
    LLAMA_SERVER = "llama-server"
    UNKNOWN = "unknown"


REAL_RUNNER_KINDS = {
    RunnerKind.ONNXRUNTIME_CPU,
    RunnerKind.ONNXRUNTIME_CUDA,
    RunnerKind.TRANSFORMERS,
    RunnerKind.LLAMA_SERVER,
}


@dataclass(frozen=True)
class ExecutionEvidenceV1:
    provider_name: str
    provider_boot_id: str
    runner_kind: RunnerKind
    real_compute: bool
    runtime_version: str
    model_digest: str
    plan_digest: str
    artifact_digests: dict[str, str]
    roles: tuple[str, ...]
    device_kind: str
    device_id: str = ""
    evidence_epoch: int = 0
    created_at_ms: int = field(default_factory=now_ms)
    schema: str = "ndnsf-di-execution-evidence-v1"

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "ExecutionEvidenceV1":
        forbidden = {"key", "privateKey", "token", "userToken", "providerToken",
                     "prompt", "payload", "tensor", "kvPayload"}
        leaked = forbidden.intersection(payload)
        if leaked:
            raise ValueError(f"execution evidence contains forbidden fields: {sorted(leaked)}")
        if payload.get("schema") != "ndnsf-di-execution-evidence-v1":
            raise ValueError("unsupported execution evidence schema")
        device = dict(payload.get("device", {}))
        value = cls(
            provider_name=str(payload.get("providerName", "")),
            provider_boot_id=str(payload.get("providerBootId", "")),
            evidence_epoch=int(payload.get("evidenceEpoch", 0)),
            runner_kind=RunnerKind(str(payload.get("runnerKind", "unknown"))),
            real_compute=bool(payload.get("realCompute", False)),
            device_kind=str(device.get("kind", payload.get("deviceKind", ""))),
            device_id=str(device.get("id", payload.get("deviceId", ""))),
            runtime_version=str(payload.get("runtimeVersion", "")),
            model_digest=str(payload.get("modelDigest", "")),
            plan_digest=str(payload.get("planDigest", "")),
            artifact_digests={str(k): str(v) for k, v in dict(payload.get("artifactDigests", {})).items()},
            roles=tuple(str(item) for item in payload.get("roles", [])),
            created_at_ms=int(payload.get("createdAtMs", 0)),
        )
        value.validate()
        return value

    def validate(self) -> None:
        required = (self.provider_name, self.provider_boot_id, self.runtime_version,
                    self.model_digest, self.plan_digest, self.device_kind)
        if not all(required) or not self.roles or not self.artifact_digests or self.created_at_ms <= 0:
            raise ValueError("execution evidence missing required field")
        if self.real_compute != (self.runner_kind in REAL_RUNNER_KINDS):
            raise ValueError("execution evidence real-compute classification mismatch")
        if self.runner_kind == RunnerKind.ONNXRUNTIME_CUDA and not self.device_id:
            raise ValueError("CUDA evidence requires a device id")


def classify_execution_evidence(items: Iterable[ExecutionEvidenceV1]) -> str:
    evidence = tuple(items)
    if not evidence:
        return "invalid-evidence"
    for item in evidence:
        item.validate()
    identity = {(item.real_compute, item.runner_kind, item.runtime_version,
                 item.model_digest, item.plan_digest, item.device_kind)
                for item in evidence}
    if len(identity) != 1:
        return "invalid-evidence"
    observed_artifacts: dict[str, str] = {}
    for item in evidence:
        for role, digest in item.artifact_digests.items():
            existing = observed_artifacts.get(role)
            if existing is not None and existing != digest:
                return "invalid-evidence"
            observed_artifacts[role] = digest
    if not evidence[0].real_compute:
        return evidence[0].runner_kind.value
    return evidence[0].runner_kind.value


@dataclass(frozen=True)
class ProviderCapabilityV3:
    provider_name: str
    supported_runner_kinds: tuple[str, ...]
    total_gpu_memory_mb: int = 0
    source: str = "profile"


@dataclass(frozen=True)
class MeasuredTelemetrySnapshotV1:
    provider_name: str
    provider_boot_id: str
    sequence: int
    measured_at_ms: int
    source: str
    status: str
    device_id: str = ""
    free_gpu_memory_mb: int = 0
    ready_queue: int = 0
    waiting_dependencies: int = 0
    active_workers: int = 0
    resource_sequence: int = 0
    sampled_at_ms: int = 0
    host_total_memory_bytes: int = 0
    host_available_memory_bytes: int = 0
    process_rss_bytes: int = 0
    worker_count: int = 0
    completed_stages: int = 0
    stage_service_time_ewma_ms: float = 0.0
    stage_service_rate_ewma_per_second: float = 0.0
    evidence_epoch: int = 0
    runner_kind: str = ""
    runtime_version: str = ""
    model_digest: str = ""
    plan_digest: str = ""
    artifact_digests: dict[str, str] = field(default_factory=dict)
    device_kind: str = ""
    membership_version: str = ""
    network_profile_version: str = ""
    cache_version: str = ""
    error_code: str = ""

    @classmethod
    def from_service_payload(cls, payload: dict[str, Any]) -> "MeasuredTelemetrySnapshotV1":
        measured = payload.get("measuredTelemetry")
        if not isinstance(measured, dict):
            raise ValueError("measured telemetry section is missing")
        if measured.get("schema") != "ndnsf-di-measured-telemetry-v1":
            raise ValueError("measured telemetry schema is unsupported")
        evidence_payload = payload.get("executionEvidence")
        if not isinstance(evidence_payload, dict):
            raise ValueError("measured telemetry execution evidence is missing")
        evidence = ExecutionEvidenceV1.from_dict(evidence_payload)
        evidence.validate()

        provider_name = str(measured.get("providerName", ""))
        provider_boot_id = str(measured.get("providerBootId", ""))
        source = str(measured.get("source", ""))
        status = str(measured.get("status", ""))
        if source in {"", "configured", "profile", "unavailable"}:
            raise ValueError("configured or unavailable telemetry is not measured")
        if provider_name != evidence.provider_name or provider_boot_id != evidence.provider_boot_id:
            raise ValueError("telemetry and execution evidence identity mismatch")

        def nonnegative_int(name: str, *, required_positive: bool = False) -> int:
            value = measured.get(name, 0)
            if isinstance(value, bool):
                raise ValueError(f"invalid measured telemetry integer: {name}")
            try:
                parsed = int(value)
            except (TypeError, ValueError) as exc:
                raise ValueError(f"invalid measured telemetry integer: {name}") from exc
            if parsed < 0 or (required_positive and parsed <= 0):
                raise ValueError(f"invalid measured telemetry integer: {name}")
            return parsed

        sequence = nonnegative_int("sequence", required_positive=status == "measured")
        resource_sequence = nonnegative_int(
            "resourceSequence", required_positive=status == "measured")
        sampled_at_ms = nonnegative_int(
            "sampledAtMs", required_positive=status == "measured")
        measured_at_ms = nonnegative_int(
            "resourceMeasuredAtMs", required_positive=status == "measured")
        host_total = nonnegative_int("hostTotalMemoryBytes")
        host_available = nonnegative_int("hostAvailableMemoryBytes")
        process_rss = nonnegative_int("processRssBytes")
        if status == "measured" and (host_total <= 0 or host_available > host_total):
            raise ValueError("invalid measured host memory facts")

        def nonnegative_float(name: str) -> float:
            try:
                parsed = float(measured.get(name, 0.0))
            except (TypeError, ValueError) as exc:
                raise ValueError(f"invalid measured telemetry number: {name}") from exc
            if parsed < 0.0:
                raise ValueError(f"invalid measured telemetry number: {name}")
            return parsed

        return cls(
            provider_name=provider_name,
            provider_boot_id=provider_boot_id,
            sequence=sequence,
            measured_at_ms=measured_at_ms,
            source=source,
            status=status,
            device_id=evidence.device_id,
            ready_queue=nonnegative_int("readyQueue"),
            waiting_dependencies=nonnegative_int("waitingDependencies"),
            active_workers=nonnegative_int("activeWorkers"),
            resource_sequence=resource_sequence,
            sampled_at_ms=sampled_at_ms,
            host_total_memory_bytes=host_total,
            host_available_memory_bytes=host_available,
            process_rss_bytes=process_rss,
            worker_count=nonnegative_int("workers"),
            completed_stages=nonnegative_int("completedStages"),
            stage_service_time_ewma_ms=nonnegative_float("stageServiceTimeEwmaMs"),
            stage_service_rate_ewma_per_second=nonnegative_float(
                "stageServiceRateEwmaPerSecond"),
            evidence_epoch=evidence.evidence_epoch,
            runner_kind=evidence.runner_kind.value,
            runtime_version=evidence.runtime_version,
            model_digest=evidence.model_digest,
            plan_digest=evidence.plan_digest,
            artifact_digests=dict(evidence.artifact_digests),
            device_kind=evidence.device_kind,
            membership_version=str(measured.get(
                "membershipVersion", payload.get("membershipVersion", ""))),
            network_profile_version=str(measured.get(
                "networkProfileVersion", payload.get("networkProfileVersion", ""))),
            cache_version=str(measured.get(
                "cacheVersion", payload.get("cacheVersion", ""))),
            error_code=str(measured.get("errorCode", "")),
        )

    def is_fresh(self, *, at_ms: int, maximum_age_ms: int = 2000) -> bool:
        age = int(at_ms) - self.measured_at_ms
        return (
            self.source not in {"", "configured", "profile", "unavailable"}
            and self.status == "measured"
            and 0 <= age <= maximum_age_ms
        )


@dataclass(frozen=True)
class PlanPredicateResultV1:
    name: str
    status: str
    observed: Any = None
    limit: Any = None


@dataclass(frozen=True)
class PlanFeasibilityRequirementsV1:
    expected_provider_name: str = ""
    expected_provider_boot_id: str = ""
    minimum_evidence_epoch: int = 0
    expected_runner_kind: str = ""
    expected_runtime_version: str = ""
    expected_model_digest: str = ""
    expected_plan_digest: str = ""
    expected_artifact_digests: dict[str, str] = field(default_factory=dict)
    expected_device_id: str = ""
    maximum_telemetry_age_ms: int = 2000
    minimum_free_host_memory_bytes: int = 0
    maximum_ready_queue: int = 0
    maximum_waiting_dependencies: int = 0
    maximum_active_workers: int = 0
    expected_membership_version: str = ""
    expected_network_profile_version: str = ""
    expected_cache_version: str = ""


@dataclass(frozen=True)
class PlanFeasibilityDecisionV1:
    decision: str
    reason_codes: tuple[str, ...]
    predicates: tuple[PlanPredicateResultV1, ...]


def evaluate_plan_feasibility(
    telemetry: MeasuredTelemetrySnapshotV1,
    requirements: PlanFeasibilityRequirementsV1,
    *,
    at_ms: int,
) -> PlanFeasibilityDecisionV1:
    """Evaluate mandatory fail-closed predicates before candidate scoring."""
    predicates: list[PlanPredicateResultV1] = []
    failures: list[tuple[str, str]] = []

    def check(name: str, passed: bool, observed: Any, limit: Any,
              reason: str, failure_decision: str) -> None:
        predicates.append(PlanPredicateResultV1(
            name=name,
            status="PASS" if passed else "FAIL",
            observed=observed,
            limit=limit,
        ))
        if not passed:
            failures.append((reason, failure_decision))

    measured_source = telemetry.source not in {
        "", "configured", "profile", "unavailable"
    } and telemetry.status == "measured"
    check("measured-source", measured_source,
          {"source": telemetry.source, "status": telemetry.status},
          "non-configured measured source", "TELEMETRY_NOT_MEASURED", "reject")
    check("freshness", telemetry.is_fresh(
        at_ms=at_ms, maximum_age_ms=requirements.maximum_telemetry_age_ms),
        at_ms - telemetry.measured_at_ms,
        requirements.maximum_telemetry_age_ms,
        "TELEMETRY_STALE", "defer")

    expected_checks = (
        ("provider-name", telemetry.provider_name,
         requirements.expected_provider_name, "PROVIDER_IDENTITY_MISMATCH", "reject"),
        ("provider-boot", telemetry.provider_boot_id,
         requirements.expected_provider_boot_id, "PROVIDER_BOOT_CHANGED", "replan"),
        ("runner-kind", telemetry.runner_kind,
         requirements.expected_runner_kind, "RUNNER_IDENTITY_MISMATCH", "reject"),
        ("runtime-version", telemetry.runtime_version,
         requirements.expected_runtime_version, "RUNTIME_IDENTITY_MISMATCH", "reject"),
        ("model-digest", telemetry.model_digest,
         requirements.expected_model_digest, "MODEL_IDENTITY_MISMATCH", "reject"),
        ("plan-digest", telemetry.plan_digest,
         requirements.expected_plan_digest, "PLAN_IDENTITY_MISMATCH", "reject"),
        ("device-id", telemetry.device_id,
         requirements.expected_device_id, "DEVICE_IDENTITY_MISMATCH", "reject"),
        ("membership-version", telemetry.membership_version,
         requirements.expected_membership_version, "MEMBERSHIP_VERSION_CHANGED", "replan"),
        ("network-profile-version", telemetry.network_profile_version,
         requirements.expected_network_profile_version, "NETWORK_VERSION_CHANGED", "replan"),
        ("cache-version", telemetry.cache_version,
         requirements.expected_cache_version, "CACHE_VERSION_CHANGED", "replan"),
    )
    for name, observed, expected, reason, failure_decision in expected_checks:
        if expected:
            check(name, observed == expected, observed, expected,
                  reason, failure_decision)
    if requirements.minimum_evidence_epoch > 0:
        check("evidence-epoch",
              telemetry.evidence_epoch >= requirements.minimum_evidence_epoch,
              telemetry.evidence_epoch, requirements.minimum_evidence_epoch,
              "EVIDENCE_EPOCH_REGRESSED", "reject")
    if requirements.expected_artifact_digests:
        check("artifact-digests",
              telemetry.artifact_digests == requirements.expected_artifact_digests,
              telemetry.artifact_digests,
              requirements.expected_artifact_digests,
              "ARTIFACT_IDENTITY_MISMATCH", "reject")
    if requirements.minimum_free_host_memory_bytes > 0:
        check("free-host-memory",
              telemetry.host_available_memory_bytes >=
              requirements.minimum_free_host_memory_bytes,
              telemetry.host_available_memory_bytes,
              requirements.minimum_free_host_memory_bytes,
              "HOST_MEMORY_PRESSURE", "defer")
    if requirements.maximum_ready_queue > 0:
        check("ready-queue", telemetry.ready_queue <= requirements.maximum_ready_queue,
              telemetry.ready_queue, requirements.maximum_ready_queue,
              "READY_QUEUE_PRESSURE", "defer")
    if requirements.maximum_waiting_dependencies > 0:
        check("waiting-dependencies",
              telemetry.waiting_dependencies <=
              requirements.maximum_waiting_dependencies,
              telemetry.waiting_dependencies,
              requirements.maximum_waiting_dependencies,
              "DEPENDENCY_QUEUE_PRESSURE", "defer")
    if requirements.maximum_active_workers > 0:
        check("active-workers",
              telemetry.active_workers <= requirements.maximum_active_workers,
              telemetry.active_workers, requirements.maximum_active_workers,
              "ACTIVE_WORKER_PRESSURE", "defer")

    priority = {"reuse": 0, "defer": 1, "replan": 2, "reject": 3}
    decision = "reuse"
    for _, candidate_decision in failures:
        if priority[candidate_decision] > priority[decision]:
            decision = candidate_decision
    return PlanFeasibilityDecisionV1(
        decision=decision,
        reason_codes=tuple(reason for reason, _ in failures),
        predicates=tuple(predicates),
    )


class TerminalReasonV1(str, Enum):
    NONE = "NONE"
    PROVIDER_LOST = "PROVIDER_LOST"
    STRAGGLER_DEADLINE = "STRAGGLER_DEADLINE"
    DEPENDENCY_MISSING = "DEPENDENCY_MISSING"
    DEPENDENCY_HASH_MISMATCH = "DEPENDENCY_HASH_MISMATCH"
    PLAN_STALE = "PLAN_STALE"
    TELEMETRY_STALE = "TELEMETRY_STALE"
    CACHE_MISS_FULL_CONTEXT_REQUIRED = "CACHE_MISS_FULL_CONTEXT_REQUIRED"
    ATTEMPT_CANCELLED = "ATTEMPT_CANCELLED"
    NO_COMPATIBLE_REPLACEMENT = "NO_COMPATIBLE_REPLACEMENT"
    REQUEST_DEADLINE = "REQUEST_DEADLINE"


@dataclass(frozen=True)
class ExecutionAttemptV1:
    request_id: str
    attempt_epoch: int
    plan_id: str
    terminal_reason: TerminalReasonV1 = TerminalReasonV1.NONE

    def __post_init__(self) -> None:
        if not self.request_id or not self.plan_id or self.attempt_epoch not in (0, 1):
            raise ValueError("invalid execution attempt")


class RoleWorkStatus(str, Enum):
    QUEUED = "queued"
    WAITING_DEPENDENCY = "waiting_dependency"
    READY = "ready"
    RUNNING = "running"
    DONE = "done"
    CANCELLED = "cancelled"
    FAILED = "failed"


class FailureAction(str, Enum):
    RETRY_SAME_PROVIDER = "retry-same-provider"
    RETRY_ALTERNATE_PROVIDER = "retry-alternate-provider"
    USE_FALLBACK_PLAN = "use-fallback-plan"
    REJECT_OR_DEFER = "reject-or-defer"


class TransferQueueKind(str, Enum):
    PREFETCH = "prefetch"
    PUBLISH = "publish"


class FragmentResidency(str, Enum):
    GPU_LOADED = "GPU_LOADED"
    CPU_RESIDENT = "CPU_RESIDENT"
    DISK_RESIDENT = "DISK_RESIDENT"
    REPO_AVAILABLE = "REPO_AVAILABLE"
    MISSING = "MISSING"


class ExactForwardCacheKind(str, Enum):
    KV_BLOCK = "kv-block"
    HIDDEN_STATE = "hidden-state"
    LOGITS = "logits"


def now_ms() -> int:
    return int(time.time() * 1000)


def stable_json(payload: Any) -> str:
    return json.dumps(to_plain(payload), sort_keys=True, separators=(",", ":"))


def stable_digest(payload: Any, *, length: int = 16) -> str:
    return hashlib.sha256(stable_json(payload).encode("utf-8")).hexdigest()[:length]


def to_plain(value: Any) -> Any:
    if isinstance(value, Enum):
        return value.value
    if is_dataclass(value):
        return {key: to_plain(item) for key, item in asdict(value).items()}
    if isinstance(value, dict):
        return {str(key): to_plain(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [to_plain(item) for item in value]
    return value


def write_json(path: str | Path, payload: Any) -> None:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(to_plain(payload), indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")


def read_json(path: str | Path) -> dict[str, Any]:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def _string_list(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, str):
        return [item.strip() for item in value.split(",") if item.strip()]
    return [str(item) for item in value]


def _safe_field(value: str, field: str) -> str:
    text = str(value)
    if not text:
        raise ValueError(f"{field} must not be empty")
    if any(ch in text for ch in ";\r\n"):
        raise ValueError(f"{field} must not contain ';' or newlines: {text!r}")
    return text


def encode_ack_metadata(fields: dict[str, Any]) -> bytes:
    """Encode typed metadata as legacy-compatible ``key=value;`` fields."""

    parts: list[str] = []
    for key in sorted(fields):
        value = fields[key]
        if value is None:
            continue
        safe_key = _safe_field(key, "ACK key")
        if isinstance(value, bool):
            encoded = "1" if value else "0"
        elif isinstance(value, (int, float, str)):
            encoded = str(value)
        elif isinstance(value, (list, tuple)) and all(
            isinstance(item, (str, int, float)) for item in value
        ):
            if not value:
                continue
            encoded = ",".join(str(item) for item in value)
        else:
            raw = stable_json(value).encode("utf-8")
            encoded = "json64:" + base64.urlsafe_b64encode(raw).decode("ascii")
        _safe_field(encoded, f"ACK field {safe_key}")
        parts.append(f"{safe_key}={encoded}")
    return (";".join(parts) + (";" if parts else "")).encode("utf-8")


def parse_ack_metadata(payload: bytes | str) -> dict[str, Any]:
    text = payload.decode("utf-8", errors="replace") if isinstance(payload, bytes) else str(payload)
    fields: dict[str, Any] = {}
    for item in text.split(";"):
        if not item or "=" not in item:
            continue
        key, value = item.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not key:
            continue
        if value.startswith("json64:"):
            try:
                raw = base64.urlsafe_b64decode(value[len("json64:"):].encode("ascii"))
                fields[key] = json.loads(raw.decode("utf-8"))
            except Exception:
                fields[key] = value
        else:
            fields[key] = value
    return fields


# ---------------------------------------------------------------------------
# Deployment lifecycle (DI-specific)
# ---------------------------------------------------------------------------

DeploymentStatus = CoreDeploymentStatus


@dataclass
class Deployment:
    deployment_id: str
    plan_id: str = ""
    plan_content_digest: str = ""
    creator: str = ""
    service_name: str = ""
    status: DeploymentStatus = DeploymentStatus.PROVISIONING
    fragment_map: dict[str, list[dict[str, Any]]] = field(default_factory=dict)
    scope_key_data_names: dict[str, str] = field(default_factory=dict)
    artifact_data_names: dict[str, str] = field(default_factory=dict)
    created_at_ms: int = field(default_factory=now_ms)
    updated_at_ms: int = field(default_factory=now_ms)
    ready_cost_ms: float = 0.0
    ref_count: int = 0
    idle_timeout_s: int = 300

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "Deployment":
        return cls(
            deployment_id=str(payload.get("deploymentId", payload.get("deployment_id", ""))),
            plan_id=str(payload.get("planId", payload.get("plan_id", ""))),
            plan_content_digest=str(payload.get("planContentDigest", payload.get("plan_content_digest", ""))),
            creator=str(payload.get("creator", "")),
            service_name=str(payload.get("serviceName", payload.get("service_name", ""))),
            status=DeploymentStatus(payload.get("status", DeploymentStatus.PROVISIONING.value)),
            fragment_map=dict(payload.get("fragmentMap", payload.get("fragment_map", {}))),
            scope_key_data_names=dict(payload.get("scopeKeyDataNames", payload.get("scope_key_data_names", {}))),
            artifact_data_names=dict(payload.get("artifactDataNames", payload.get("artifact_data_names", {}))),
            created_at_ms=int(payload.get("createdAtMs", payload.get("created_at_ms", now_ms()))),
            updated_at_ms=int(payload.get("updatedAtMs", payload.get("updated_at_ms", now_ms()))),
            ready_cost_ms=float(payload.get("readyCostMs", payload.get("ready_cost_ms", 0.0)) or 0.0),
            ref_count=int(payload.get("refCount", payload.get("ref_count", 0)) or 0),
            idle_timeout_s=int(payload.get("idleTimeoutS", payload.get("idle_timeout_s", 300)) or 300),
        )

    def can_evict(self) -> tuple[bool, str]:
        if self.status == DeploymentStatus.PROVISIONING:
            return False, "DEPLOYMENT_NOT_READY"
        return False, "PROVIDER_EXECUTION_LEASE_CHECK_REQUIRED"

    def role_provider(self, role_id: str) -> str:
        assignments = self.fragment_map.get(role_id, [])
        if assignments:
            return str(assignments[0].get("provider", ""))
        return ""

    def estimated_ready_ms(self) -> float:
        if self.status == DeploymentStatus.ACTIVE:
            return 0.0
        if self.status == DeploymentStatus.DISK_RESIDENT:
            return RESIDENCY_READY_COST_MS["DISK_RESIDENT"]
        return self.ready_cost_ms


@dataclass(frozen=True)
class KvCacheTelemetry:
    budget_mb: float = 0.0
    used_mb: float = 0.0
    max_context_tokens: int = 0
    resident_prefix_ids: tuple[str, ...] = ()
    resident_session_ids: tuple[str, ...] = ()
    resident_exact_cache_key_digests: tuple[str, ...] = ()
    hits: int = 0
    misses: int = 0
    evictions: int = 0

    @property
    def free_mb(self) -> float:
        return max(0.0, self.budget_mb - self.used_mb)

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "KvCacheTelemetry":
        return cls(
            budget_mb=float(payload.get("budgetMb", payload.get("budget_mb", 0)) or 0),
            used_mb=float(payload.get("usedMb", payload.get("used_mb", 0)) or 0),
            max_context_tokens=int(payload.get(
                "maxContextTokens",
                payload.get("max_context_tokens", 0)) or 0),
            resident_prefix_ids=tuple(_string_list(payload.get(
                "residentPrefixIds",
                payload.get("resident_prefix_ids", ())))),
            resident_session_ids=tuple(_string_list(payload.get(
                "residentSessionIds",
                payload.get("resident_session_ids", ())))),
            resident_exact_cache_key_digests=tuple(_string_list(payload.get(
                "residentExactCacheKeyDigests",
                payload.get("resident_exact_cache_key_digests", ())))),
            hits=int(payload.get("hits", 0) or 0),
            misses=int(payload.get("misses", 0) or 0),
            evictions=int(payload.get("evictions", 0) or 0),
        )


@dataclass(frozen=True)
class ProviderProfileV1:
    provider: str
    node: str = ""
    gpu_memory_mb: float = 0.0
    ram_memory_mb: float = 0.0
    flops_tflops: float = 0.0
    llm_stage_capacity_mb: float = 0.0
    llm_max_stage_layers: int = 0
    max_workers: int = 1
    supported_backends: tuple[str, ...] = ()
    model_families: tuple[str, ...] = ("llm",)
    max_context_tokens: int = 0
    kv_cache_budget_mb: float = 0.0
    model_cache: tuple[str, ...] = ()
    version: str = ""

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "ProviderProfileV1":
        provider = str(payload.get("provider") or payload.get("providerName") or "")
        if not provider:
            raise ValueError("provider profile requires provider")
        gpu = float(payload.get("gpuMemoryMb", payload.get("gpu_memory_mb", 0)) or 0)
        ram = float(payload.get("ramMemoryMb", payload.get("ram_memory_mb", 0)) or 0)
        return cls(
            provider=provider,
            node=str(payload.get("node", "")),
            gpu_memory_mb=gpu,
            ram_memory_mb=ram,
            flops_tflops=float(payload.get("flopsTflops", payload.get("flops_tflops", 0)) or 0),
            llm_stage_capacity_mb=float(payload.get(
                "llmStageCapacityMb",
                payload.get("llm_stage_capacity_mb", gpu)) or 0),
            llm_max_stage_layers=int(payload.get(
                "llmMaxStageLayers",
                payload.get("llm_max_stage_layers", 0)) or 0),
            max_workers=max(1, int(payload.get("maxWorkers", payload.get("max_workers", 1)) or 1)),
            supported_backends=tuple(_string_list(payload.get(
                "supportedBackends",
                payload.get("backends", payload.get("supported_backends", ()))))),
            model_families=tuple(_string_list(payload.get(
                "modelFamilies",
                payload.get("model_families", ("llm",))))),
            max_context_tokens=int(payload.get(
                "maxContextTokens",
                payload.get("max_context_tokens", 0)) or 0),
            kv_cache_budget_mb=float(payload.get(
                "kvCacheBudgetMb",
                payload.get("kv_cache_budget_mb", 0)) or 0),
            model_cache=tuple(_string_list(payload.get(
                "modelCache",
                payload.get("model_cache", ())))),
            version=str(payload.get("version", "")),
        )

    def effective_capacity_weight(self, max_memory: float, max_flops: float) -> float:
        memory_ratio = self.llm_stage_capacity_mb / max(max_memory, 0.001)
        compute_ratio = self.flops_tflops / max(max_flops, 0.001)
        return max(0.0, min(memory_ratio, compute_ratio))

    def to_ack_fields(self) -> dict[str, Any]:
        return {
            "schema": "ndnsf-di-runtime-v1",
            "providerProfile": to_plain(self),
            "gpuMemoryMb": self.gpu_memory_mb,
            "ramMemoryMb": self.ram_memory_mb,
            "flopsTflops": self.flops_tflops,
            "llmStageCapacityMb": self.llm_stage_capacity_mb,
            "llmMaxStageLayers": self.llm_max_stage_layers,
            "maxWorkers": self.max_workers,
            "modelFamilies": list(self.model_families),
            "maxContextTokens": self.max_context_tokens,
            "kvCacheBudgetMb": self.kv_cache_budget_mb,
            "backends": list(self.supported_backends),
        }


@dataclass(frozen=True)
class RuntimeTelemetryV1:
    provider: str
    timestamp_ms: int = field(default_factory=now_ms)
    ready_queue: int = 0
    waiting_dependencies: int = 0
    active_workers: int = 0
    free_memory_mb: float = 0.0
    model_loaded: bool = False
    runtime_backend: str = ""
    service_time_ewma_ms: float = 0.0
    queue_wait_ewma_ms: float = 0.0
    kv_cache: KvCacheTelemetry = field(default_factory=KvCacheTelemetry)
    network_rtt_ms: dict[str, float] = field(default_factory=dict)
    network_bandwidth_mbps: dict[str, float] = field(default_factory=dict)

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "RuntimeTelemetryV1":
        kv_payload = payload.get("kvCache", payload.get("kv_cache", {}))
        kv_cache = (
            kv_payload
            if isinstance(kv_payload, KvCacheTelemetry)
            else KvCacheTelemetry.from_dict(dict(kv_payload or {}))
        )
        return cls(
            provider=str(payload.get("provider", "")),
            timestamp_ms=int(payload.get("timestampMs", payload.get("timestamp_ms", now_ms()))),
            ready_queue=int(payload.get("readyQueue", payload.get("ready_queue", 0)) or 0),
            waiting_dependencies=int(payload.get(
                "waitingDependencies",
                payload.get("waiting_dependencies", 0)) or 0),
            active_workers=int(payload.get("activeWorkers", payload.get("active_workers", 0)) or 0),
            free_memory_mb=float(payload.get("freeMemoryMb", payload.get("free_memory_mb", 0)) or 0),
            model_loaded=bool(payload.get("modelLoaded", payload.get("model_loaded", False))),
            runtime_backend=str(payload.get("runtimeBackend", payload.get("runtime_backend", ""))),
            service_time_ewma_ms=float(payload.get(
                "serviceTimeEwmaMs",
                payload.get("service_time_ewma_ms", 0)) or 0),
            queue_wait_ewma_ms=float(payload.get(
                "queueWaitEwmaMs",
                payload.get("queue_wait_ewma_ms", 0)) or 0),
            kv_cache=kv_cache,
            network_rtt_ms=dict(payload.get("networkRttMs", payload.get("network_rtt_ms", {}))),
            network_bandwidth_mbps=dict(payload.get(
                "networkBandwidthMbps",
                payload.get("network_bandwidth_mbps", {}))),
        )

    @property
    def aggregate_queue(self) -> int:
        return max(0, self.ready_queue + self.waiting_dependencies + self.active_workers)

    def to_ack_fields(self) -> dict[str, Any]:
        return {
            "providerTelemetry": to_plain(self),
            "queue": self.aggregate_queue,
            "readyQueue": self.ready_queue,
            "waitingInputs": self.waiting_dependencies,
            "activeWorkers": self.active_workers,
            "freeMemoryMb": self.free_memory_mb,
            "modelLoaded": self.model_loaded,
            "runtimeBackend": self.runtime_backend,
            "kvCacheUsedMb": self.kv_cache.used_mb,
            "kvCacheBudgetMb": self.kv_cache.budget_mb,
            "kvCacheHits": self.kv_cache.hits,
            "kvCacheMisses": self.kv_cache.misses,
            "kvCacheEvictions": self.kv_cache.evictions,
        }


@dataclass(frozen=True)
class ModelManifestV1:
    model_id: str
    revision: str = ""
    model_family: str = "llm"
    model_format: str = "unknown"
    layers: int = 0
    memory_per_layer_mb: float = 0.0
    flops_per_layer_tflop: float = 0.0
    activation_boundary_mb: float = 0.0
    fixed_runtime_memory_mb: float = 0.0
    context_window_tokens: int = 0
    tokenizer_id: str = ""
    kv_cache_bytes_per_token_per_layer: int = 0
    supports_prefill: bool = True
    supports_decode: bool = True
    supports_streaming: bool = False
    metadata: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "ModelManifestV1":
        return cls(
            model_id=str(payload.get("modelId", payload.get("model_id", ""))),
            revision=str(payload.get("revision", "")),
            model_family=str(payload.get("modelFamily", payload.get("model_family", "llm"))),
            model_format=str(payload.get("modelFormat", payload.get("model_format", "unknown"))),
            layers=int(payload.get("layers", 0) or 0),
            memory_per_layer_mb=float(payload.get(
                "memoryPerLayerMb",
                payload.get("memory_per_layer_mb", 0)) or 0),
            flops_per_layer_tflop=float(payload.get(
                "flopsPerLayerTflop",
                payload.get("flops_per_layer_tflop", 0)) or 0),
            activation_boundary_mb=float(payload.get(
                "activationBoundaryMb",
                payload.get("activation_boundary_mb", 0)) or 0),
            fixed_runtime_memory_mb=float(payload.get(
                "fixedRuntimeMemoryMb",
                payload.get("fixed_runtime_memory_mb", 0)) or 0),
            context_window_tokens=int(payload.get(
                "contextWindowTokens",
                payload.get("context_window_tokens", 0)) or 0),
            tokenizer_id=str(payload.get("tokenizerId", payload.get("tokenizer_id", ""))),
            kv_cache_bytes_per_token_per_layer=int(payload.get(
                "kvCacheBytesPerTokenPerLayer",
                payload.get("kv_cache_bytes_per_token_per_layer", 0)) or 0),
            supports_prefill=bool(payload.get("supportsPrefill", payload.get("supports_prefill", True))),
            supports_decode=bool(payload.get("supportsDecode", payload.get("supports_decode", True))),
            supports_streaming=bool(payload.get("supportsStreaming", payload.get("supports_streaming", False))),
            metadata=dict(payload.get("metadata", {})),
        )

    def kv_cache_mb(self, tokens: int, layers: int | None = None) -> float:
        layer_count = self.layers if layers is None else max(0, int(layers))
        return (
            max(0, int(tokens)) *
            max(0, int(layer_count)) *
            max(0, int(self.kv_cache_bytes_per_token_per_layer)) /
            (1024.0 * 1024.0)
        )


@dataclass(frozen=True)
class ModelFragmentKey:
    model_id: str
    model_version: str = ""
    model_digest: str = ""
    runtime_backend: str = ""
    precision: str = ""
    split_strategy: str = ""
    stage_index: int = 0
    stage_count: int = 1
    layer_start: int = 0
    layer_end: int = -1
    shard_index: int = 0
    shard_count: int = 1
    fragment_digest: str = ""

    def __post_init__(self) -> None:
        if not self.model_id:
            raise ValueError("model_id is required")
        if self.stage_index < 0 or self.stage_index >= max(1, self.stage_count):
            raise ValueError("stage_index must be within stage_count")
        if self.shard_index < 0 or self.shard_index >= max(1, self.shard_count):
            raise ValueError("shard_index must be within shard_count")
        if self.layer_end >= 0 and self.layer_end < self.layer_start:
            raise ValueError("layer range is invalid")

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "ModelFragmentKey":
        return cls(
            model_id=str(payload.get("modelId", payload.get("model_id", ""))),
            model_version=str(payload.get("modelVersion", payload.get("model_version", ""))),
            model_digest=str(payload.get("modelDigest", payload.get("model_digest", ""))),
            runtime_backend=str(payload.get("runtimeBackend", payload.get("runtime_backend", ""))),
            precision=str(payload.get("precision", "")),
            split_strategy=str(payload.get("splitStrategy", payload.get("split_strategy", ""))),
            stage_index=int(payload.get("stageIndex", payload.get("stage_index", 0)) or 0),
            stage_count=max(1, int(payload.get("stageCount", payload.get("stage_count", 1)) or 1)),
            layer_start=int(payload.get("layerStart", payload.get("layer_start", 0)) or 0),
            layer_end=int(payload.get("layerEnd", payload.get("layer_end", -1))),
            shard_index=int(payload.get("shardIndex", payload.get("shard_index", 0)) or 0),
            shard_count=max(1, int(payload.get("shardCount", payload.get("shard_count", 1)) or 1)),
            fragment_digest=str(payload.get("fragmentDigest", payload.get("fragment_digest", ""))),
        )

    def digest(self) -> str:
        return stable_digest(self, length=24)


@dataclass(frozen=True)
class DiFragmentRuntimeState:
    fragment_key: ModelFragmentKey
    residency: FragmentResidency = FragmentResidency.MISSING
    estimated_ready_ms: float = 0.0
    pinned: bool = False
    last_used_ms: int = 0
    memory_footprint_mb: float = 0.0
    confidence: float = 1.0

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "DiFragmentRuntimeState":
        key_payload = payload.get("fragmentKey", payload.get("fragment_key", {}))
        return cls(
            fragment_key=(
                key_payload if isinstance(key_payload, ModelFragmentKey)
                else ModelFragmentKey.from_dict(dict(key_payload))
            ),
            residency=FragmentResidency(payload.get("residency", FragmentResidency.MISSING.value)),
            estimated_ready_ms=float(payload.get(
                "estimatedReadyMs",
                payload.get("estimated_ready_ms", 0.0)) or 0.0),
            pinned=bool(payload.get("pinned", False)),
            last_used_ms=int(payload.get("lastUsedMs", payload.get("last_used_ms", 0)) or 0),
            memory_footprint_mb=float(payload.get(
                "memoryFootprintMb",
                payload.get("memory_footprint_mb", 0.0)) or 0.0),
            confidence=float(payload.get("confidence", 1.0)),
        )


@dataclass(frozen=True)
class DiProviderRuntimeState:
    provider_name: str
    timestamp_ms: int = field(default_factory=now_ms)
    active_role_count: int = 0
    queue_length: int = 0
    estimated_queue_wait_ms: float = 0.0
    free_gpu_memory_mb: float = 0.0
    free_cpu_memory_mb: float = 0.0
    supported_backends: tuple[str, ...] = ()
    fragment_states: tuple[DiFragmentRuntimeState, ...] = ()
    kv_cache_hints: tuple[dict[str, Any], ...] = ()
    confidence: float = 1.0

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "DiProviderRuntimeState":
        fragments = payload.get("fragmentStates", payload.get("fragment_states", ())) or ()
        return cls(
            provider_name=str(payload.get("providerName", payload.get("provider_name", ""))),
            timestamp_ms=int(payload.get("timestampMs", payload.get("timestamp_ms", now_ms()))),
            active_role_count=int(payload.get(
                "activeRoleCount",
                payload.get("active_role_count", 0)) or 0),
            queue_length=int(payload.get("queueLength", payload.get("queue_length", 0)) or 0),
            estimated_queue_wait_ms=float(payload.get(
                "estimatedQueueWaitMs",
                payload.get("estimated_queue_wait_ms", 0.0)) or 0.0),
            free_gpu_memory_mb=float(payload.get(
                "freeGpuMemoryMb",
                payload.get("free_gpu_memory_mb", 0.0)) or 0.0),
            free_cpu_memory_mb=float(payload.get(
                "freeCpuMemoryMb",
                payload.get("free_cpu_memory_mb", 0.0)) or 0.0),
            supported_backends=tuple(_string_list(payload.get(
                "supportedBackends",
                payload.get("supported_backends", ())))),
            fragment_states=tuple(
                item if isinstance(item, DiFragmentRuntimeState) else
                DiFragmentRuntimeState.from_dict(dict(item))
                for item in fragments
            ),
            kv_cache_hints=tuple(dict(item) for item in payload.get(
                "kvCacheHints",
                payload.get("kv_cache_hints", ())) or ()),
            confidence=float(payload.get("confidence", 1.0)),
        )

    def fragment_state_for(self, fragment_key: ModelFragmentKey) -> DiFragmentRuntimeState | None:
        digest = fragment_key.digest()
        for state in self.fragment_states:
            if state.fragment_key.digest() == digest:
                return state
        return None


@dataclass
class ProviderFragmentInventoryEntry:
    fragment_key: ModelFragmentKey
    disk_path: str = ""
    memory_footprint_mb: float = 0.0
    repo_available: bool = False
    pinned: bool = False
    confidence: float = 1.0
    cpu_resident: bool = False
    gpu_loaded: bool = False
    last_used_ms: int = 0
    lease_binding_proof: bytes = b""


class ProviderFragmentInventoryManager:
    """Provider-local source of truth for DI fragment residency.

    GPU and CPU residency are runtime events reported by the provider process.
    Disk residency is derived from the actual local artifact path. This keeps
    model-fragment state in NDNSF-DI while still producing the generic ACK
    metadata consumed by the user-side planner.
    """

    def __init__(self,
                 provider_name: str,
                 *,
                 supported_backends: Iterable[str] = (),
                 free_gpu_memory_mb: float = 0.0,
                 free_cpu_memory_mb: float = 0.0,
                 active_role_count: int = 0,
                 queue_length: int = 0,
                 estimated_queue_wait_ms: float = 0.0,
                 confidence: float = 1.0,
                 lease_pin_checker: Callable[[bytes, int], bool] | None = None):
        if not provider_name:
            raise ValueError("provider_name is required")
        self.provider_name = provider_name
        self.supported_backends = tuple(str(item) for item in supported_backends if str(item))
        self.free_gpu_memory_mb = float(free_gpu_memory_mb)
        self.free_cpu_memory_mb = float(free_cpu_memory_mb)
        self.active_role_count = int(active_role_count)
        self.queue_length = int(queue_length)
        self.estimated_queue_wait_ms = float(estimated_queue_wait_ms)
        self.confidence = float(confidence)
        self._lease_pin_checker = lease_pin_checker
        self._entries: dict[str, ProviderFragmentInventoryEntry] = {}

    def register_fragment(self,
                          fragment_key: ModelFragmentKey,
                          *,
                          disk_path: str | Path = "",
                          memory_footprint_mb: float = 0.0,
                          repo_available: bool = False,
                          pinned: bool = False,
                          lease_binding_proof: bytes = b"",
                          confidence: float = 1.0) -> ProviderFragmentInventoryEntry:
        entry = ProviderFragmentInventoryEntry(
            fragment_key=fragment_key,
            disk_path=str(disk_path),
            memory_footprint_mb=float(memory_footprint_mb),
            repo_available=bool(repo_available),
            pinned=bool(pinned),
            lease_binding_proof=bytes(lease_binding_proof),
            confidence=float(confidence),
        )
        self._entries[fragment_key.digest()] = entry
        return entry

    def mark_cpu_resident(self, fragment_key: ModelFragmentKey, resident: bool = True) -> None:
        entry = self._entry_for(fragment_key)
        entry.cpu_resident = bool(resident)
        if resident:
            entry.last_used_ms = now_ms()
        elif not entry.gpu_loaded:
            entry.last_used_ms = 0

    def mark_gpu_loaded(self, fragment_key: ModelFragmentKey, loaded: bool = True) -> None:
        entry = self._entry_for(fragment_key)
        entry.gpu_loaded = bool(loaded)
        if loaded:
            entry.cpu_resident = True
            entry.last_used_ms = now_ms()
        elif not entry.cpu_resident:
            entry.last_used_ms = 0

    def evict(self, fragment_key: ModelFragmentKey, *, from_gpu: bool = True, from_cpu: bool = False) -> None:
        entry = self._entry_for(fragment_key)
        if entry.pinned or (
            entry.lease_binding_proof
            and self._lease_pin_checker is not None
            and self._lease_pin_checker(entry.lease_binding_proof, now_ms())
        ):
            raise RuntimeError("LEASE_BINDING_PINNED")
        if from_gpu:
            entry.gpu_loaded = False
        if from_cpu:
            entry.cpu_resident = False
        if not entry.gpu_loaded and not entry.cpu_resident:
            entry.last_used_ms = 0

    def state_for(self, fragment_key: ModelFragmentKey) -> DiFragmentRuntimeState:
        entry = self._entries.get(fragment_key.digest())
        if entry is None:
            return DiFragmentRuntimeState(
                fragment_key=fragment_key,
                residency=FragmentResidency.MISSING,
                estimated_ready_ms=RESIDENCY_READY_COST_MS[FragmentResidency.MISSING],
                confidence=0.0,
            )
        return self._state_from_entry(entry)

    def snapshot(self) -> DiProviderRuntimeState:
        return DiProviderRuntimeState(
            provider_name=self.provider_name,
            active_role_count=max(0, self.active_role_count),
            queue_length=max(0, self.queue_length),
            estimated_queue_wait_ms=max(0.0, self.estimated_queue_wait_ms),
            free_gpu_memory_mb=max(0.0, self.free_gpu_memory_mb),
            free_cpu_memory_mb=max(0.0, self.free_cpu_memory_mb),
            supported_backends=self.supported_backends,
            fragment_states=tuple(
                self._state_from_entry(entry)
                for _, entry in sorted(self._entries.items())
            ),
            confidence=self.confidence,
        )

    def ack_metadata(self,
                     *,
                     lease_offers: Iterable[GenericAdmissionLease] = ()) -> GenericAckMetadata:
        snapshot = self.snapshot()
        return GenericAckMetadata(
            provider_runtime_hint=GenericProviderRuntimeHint(
                provider_name=self.provider_name,
                active_work_count=max(0, self.active_role_count),
                queue_length=max(0, self.queue_length),
                estimated_queue_wait_ms=max(0.0, self.estimated_queue_wait_ms),
                confidence=self.confidence,
            ),
            lease_offers=tuple(lease_offers),
            service_payload_schema="ndnsf-di-runtime-ack-v1",
            service_payload=to_plain(snapshot),
        )

    def capability_hint(self,
                        service_name: str,
                        *,
                        ready: bool = True,
                        reason_code: str = "",
                        message: str = "") -> CoreProviderCapabilityHint:
        metadata = self.ack_metadata()
        return CoreProviderCapabilityHint(
            provider_name=self.provider_name,
            service_name=service_name,
            ready=ready,
            reason_code=reason_code,
            message=message,
            runtime_hint=metadata.provider_runtime_hint,
            service_payload_schema=metadata.service_payload_schema,
            service_payload=metadata.service_payload,
        )

    def residency_counters(self) -> dict[str, int]:
        counters = {item.value: 0 for item in FragmentResidency}
        for state in self.snapshot().fragment_states:
            counters[state.residency.value] += 1
        return counters

    def _entry_for(self, fragment_key: ModelFragmentKey) -> ProviderFragmentInventoryEntry:
        digest = fragment_key.digest()
        if digest not in self._entries:
            raise KeyError(f"fragment is not registered: {fragment_key.fragment_digest or digest}")
        return self._entries[digest]

    def _state_from_entry(self, entry: ProviderFragmentInventoryEntry) -> DiFragmentRuntimeState:
        if entry.gpu_loaded:
            residency = FragmentResidency.GPU_LOADED
        elif entry.cpu_resident:
            residency = FragmentResidency.CPU_RESIDENT
        elif entry.disk_path and Path(entry.disk_path).exists():
            residency = FragmentResidency.DISK_RESIDENT
        elif entry.repo_available:
            residency = FragmentResidency.REPO_AVAILABLE
        else:
            residency = FragmentResidency.MISSING
        return DiFragmentRuntimeState(
            fragment_key=entry.fragment_key,
            residency=residency,
            estimated_ready_ms=RESIDENCY_READY_COST_MS[residency],
            pinned=entry.pinned,
            last_used_ms=entry.last_used_ms,
            memory_footprint_mb=entry.memory_footprint_mb,
            confidence=entry.confidence,
        )


@dataclass(frozen=True)
class DiLeaseResourceBinding:
    role_id: str
    fragment_key: ModelFragmentKey
    residency: FragmentResidency = FragmentResidency.MISSING
    reserved_gpu_memory_mb: float = 0.0
    reserved_cpu_memory_mb: float = 0.0
    estimated_ready_ms: float = 0.0

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "DiLeaseResourceBinding":
        key_payload = payload.get("fragmentKey", payload.get("fragment_key", {}))
        return cls(
            role_id=str(payload.get("roleId", payload.get("role_id", ""))),
            fragment_key=(
                key_payload if isinstance(key_payload, ModelFragmentKey)
                else ModelFragmentKey.from_dict(dict(key_payload))
            ),
            residency=FragmentResidency(payload.get("residency", FragmentResidency.MISSING.value)),
            reserved_gpu_memory_mb=float(payload.get(
                "reservedGpuMemoryMb",
                payload.get("reserved_gpu_memory_mb", 0.0)) or 0.0),
            reserved_cpu_memory_mb=float(payload.get(
                "reservedCpuMemoryMb",
                payload.get("reserved_cpu_memory_mb", 0.0)) or 0.0),
            estimated_ready_ms=float(payload.get(
                "estimatedReadyMs",
                payload.get("estimated_ready_ms", 0.0)) or 0.0),
        )

    def matches(self, *, role_id: str, fragment_key: ModelFragmentKey) -> bool:
        return self.role_id == role_id and self.fragment_key.digest() == fragment_key.digest()


@dataclass(frozen=True)
class PlanRole:
    role_id: str
    fragment_key: ModelFragmentKey
    estimated_compute_ms: float = 0.0
    memory_mb: float = 0.0

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "PlanRole":
        return cls(
            role_id=str(payload.get("roleId", payload.get("role_id", payload.get("role", "")))),
            fragment_key=ModelFragmentKey.from_dict(dict(payload.get(
                "fragmentKey",
                payload.get("fragment_key", {})))),
            estimated_compute_ms=float(payload.get(
                "estimatedComputeMs",
                payload.get("estimated_compute_ms", 0.0)) or 0.0),
            memory_mb=float(payload.get("memoryMb", payload.get("memory_mb", 0.0)) or 0.0),
        )


@dataclass(frozen=True)
class PlanDependency:
    from_role: str
    to_role: str
    bytes_count: int = 0

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "PlanDependency":
        return cls(
            from_role=str(payload.get("fromRole", payload.get("from_role", payload.get("from", "")))),
            to_role=str(payload.get("toRole", payload.get("to_role", payload.get("to", "")))),
            bytes_count=int(payload.get("bytes", payload.get("bytes_count", 0)) or 0),
        )


@dataclass(frozen=True)
class PlanTemplate:
    template_id: str
    model_id: str
    roles: tuple[PlanRole, ...]
    dependencies: tuple[PlanDependency, ...] = ()
    split_strategy: str = ""

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "PlanTemplate":
        return cls(
            template_id=str(payload.get("templateId", payload.get("template_id", ""))),
            model_id=str(payload.get("modelId", payload.get("model_id", ""))),
            split_strategy=str(payload.get("splitStrategy", payload.get("split_strategy", ""))),
            roles=tuple(PlanRole.from_dict(dict(item)) for item in payload.get("roles", [])),
            dependencies=tuple(PlanDependency.from_dict(dict(item)) for item in payload.get("dependencies", [])),
        )


@dataclass(frozen=True)
class RuntimeAssignment:
    request_id: str
    template_id: str
    role_assignments: dict[str, dict[str, Any]]
    score_breakdown: dict[str, Any] = field(default_factory=dict)
    replan_attempt: int = 0
    selected_at_ms: int = field(default_factory=now_ms)


@dataclass(frozen=True)
class ReplanRecord:
    request_id: str
    attempt: int
    failed_provider: str
    failed_lease_id: str
    reason_code: str
    excluded_providers: tuple[str, ...] = ()
    timestamp_ms: int = field(default_factory=now_ms)

    @classmethod
    def from_failure(cls,
                     *,
                     request_id: str,
                     attempt: int,
                     failed_provider: str,
                     reason_code: str,
                     failed_lease_id: str = "",
                     previous_excluded: Iterable[str] = ()) -> "ReplanRecord":
        excluded = set(str(item) for item in previous_excluded if str(item))
        if failed_provider:
            excluded.add(failed_provider)
        return cls(
            request_id=request_id,
            attempt=max(1, int(attempt)),
            failed_provider=failed_provider,
            failed_lease_id=failed_lease_id,
            reason_code=reason_code or "UNKNOWN",
            excluded_providers=tuple(sorted(excluded)),
        )

    def excluded_provider_set(self) -> set[str]:
        excluded = set(self.excluded_providers)
        if self.failed_provider:
            excluded.add(self.failed_provider)
        return excluded


RESIDENCY_READY_COST_MS = {
    FragmentResidency.GPU_LOADED: 0.0,
    FragmentResidency.CPU_RESIDENT: 8.0,
    FragmentResidency.DISK_RESIDENT: 35.0,
    FragmentResidency.REPO_AVAILABLE: 120.0,
    FragmentResidency.MISSING: 1_000_000.0,
}


def residency_ready_cost_ms(residency: FragmentResidency, estimated_ready_ms: float = 0.0) -> float:
    return RESIDENCY_READY_COST_MS[FragmentResidency(residency)] + max(0.0, float(estimated_ready_ms))


def _metadata_from_candidate(candidate: dict[str, Any]) -> GenericAckMetadata | None:
    if "genericAckMetadata" in candidate:
        payload = candidate["genericAckMetadata"]
        return payload if isinstance(payload, GenericAckMetadata) else GenericAckMetadata.from_dict(dict(payload))
    if "providerCapabilityHint" in candidate:
        payload = candidate["providerCapabilityHint"]
        hint = (
            payload if isinstance(payload, CoreProviderCapabilityHint)
            else CoreProviderCapabilityHint.from_dict(dict(payload))
        )
        return GenericAckMetadata.from_provider_capability_hint(hint)
    if "ackFields" in candidate:
        fields = dict(candidate["ackFields"])
        if "providerCapabilityHint" in fields:
            hint = CoreProviderCapabilityHint.from_ack_fields(fields)
            return GenericAckMetadata.from_provider_capability_hint(hint)
        return GenericAckMetadata.from_ack_fields(fields)
    return None


def _di_state_from_metadata(metadata: GenericAckMetadata) -> DiProviderRuntimeState:
    payload = dict(metadata.service_payload or {})
    payload.setdefault("providerName", metadata.provider_runtime_hint.provider_name)
    payload.setdefault("queueLength", metadata.provider_runtime_hint.queue_length)
    payload.setdefault("estimatedQueueWaitMs", metadata.provider_runtime_hint.estimated_queue_wait_ms)
    return DiProviderRuntimeState.from_dict(payload)


def score_runtime_candidate(role: PlanRole,
                            candidate: dict[str, Any],
                            *,
                            runtime_required: bool = True,
                            now_ms_value: int | None = None,
                            feasibility_requirements:
                            PlanFeasibilityRequirementsV1 | None = None) -> dict[str, Any]:
    provider = str(candidate.get("providerName", candidate.get("provider", "")))
    if feasibility_requirements is not None:
        telemetry = candidate.get("measuredTelemetrySnapshot")
        if isinstance(telemetry, dict):
            telemetry = MeasuredTelemetrySnapshotV1.from_service_payload(telemetry)
        if not isinstance(telemetry, MeasuredTelemetrySnapshotV1):
            return {
                "provider": provider,
                "roleId": role.role_id,
                "valid": False,
                "reason": "TELEMETRY_REQUIRED",
                "feasibilityDecision": "defer",
                "planPredicates": [],
                "scoreMs": float("inf"),
            }
        feasibility = evaluate_plan_feasibility(
            telemetry,
            feasibility_requirements,
            at_ms=now_ms() if now_ms_value is None else int(now_ms_value),
        )
        if feasibility.decision != "reuse":
            return {
                "provider": provider,
                "roleId": role.role_id,
                "valid": False,
                "reason": (
                    feasibility.reason_codes[0]
                    if feasibility.reason_codes else "PLAN_FEASIBILITY_REJECTED"
                ),
                "feasibilityDecision": feasibility.decision,
                "planPredicates": to_plain(feasibility.predicates),
                "scoreMs": float("inf"),
            }
    metadata = _metadata_from_candidate(candidate)
    if metadata is None:
        if runtime_required:
            return {
                "provider": provider,
                "roleId": role.role_id,
                "valid": False,
                "reason": "RUNTIME_METADATA_REQUIRED",
                "scoreMs": float("inf"),
            }
        return {
            "provider": provider,
            "roleId": role.role_id,
            "valid": True,
            "reason": "CONSERVATIVE_FALLBACK",
            "scoreMs": 10_000.0,
        }
    hint = metadata.provider_runtime_hint
    provider = hint.provider_name
    valid_leases = [
        lease for lease in metadata.lease_offers
        if lease.is_valid(now_ms_value=now_ms_value)
    ]
    if metadata.lease_offers and not valid_leases:
        return {
            "provider": provider,
            "roleId": role.role_id,
            "valid": False,
            "reason": "NO_VALID_LEASE",
            "scoreMs": float("inf"),
        }
    di_state = _di_state_from_metadata(metadata)
    fragment_state = di_state.fragment_state_for(role.fragment_key)
    residency = fragment_state.residency if fragment_state else FragmentResidency.MISSING
    ready_ms = fragment_state.estimated_ready_ms if fragment_state else 0.0
    if residency == FragmentResidency.MISSING:
        return {
            "provider": provider,
            "roleId": role.role_id,
            "valid": False,
            "reason": "FRAGMENT_MISSING",
            "scoreMs": float("inf"),
        }
    lease = valid_leases[0] if valid_leases else None
    score = (
        max(0.0, role.estimated_compute_ms) +
        residency_ready_cost_ms(residency, ready_ms) +
        max(0.0, hint.estimated_queue_wait_ms) +
        max(0, hint.queue_length) * 5.0 +
        (1.0 - min(1.0, max(0.0, hint.confidence))) * 50.0
    )
    return {
        "provider": provider,
        "roleId": role.role_id,
        "valid": True,
        "reason": "OK",
        "scoreMs": score,
        "residency": residency.value,
        "leaseId": "" if lease is None else lease.lease_id,
        "fragmentDigest": role.fragment_key.fragment_digest,
        "queueLength": hint.queue_length,
        "estimatedQueueWaitMs": hint.estimated_queue_wait_ms,
    }


def choose_runtime_assignment(template: PlanTemplate,
                              provider_candidates: dict[str, list[dict[str, Any]]],
                              *,
                              request_id: str,
                              runtime_required: bool = True,
                              network_matrix: ProviderNetworkMatrix | None = None,
                              excluded_providers: Iterable[str] = (),
                              feasibility_requirements_by_provider:
                              dict[str, PlanFeasibilityRequirementsV1] | None = None,
                              telemetry_at_ms: int | None = None) -> RuntimeAssignment:
    excluded = set(str(item) for item in excluded_providers if str(item))
    role_assignments: dict[str, dict[str, Any]] = {}
    rejected: list[dict[str, Any]] = []
    role_scores: dict[str, list[dict[str, Any]]] = {}
    for role in template.roles:
        scored = [
            ({
                "provider": str(candidate.get("providerName", candidate.get("provider", ""))),
                "roleId": role.role_id,
                "valid": False,
                "reason": "PROVIDER_EXCLUDED",
                "scoreMs": float("inf"),
            } if str(candidate.get("providerName", candidate.get("provider", ""))) in excluded
             else score_runtime_candidate(
                 role,
                 candidate,
                 runtime_required=runtime_required,
                 now_ms_value=telemetry_at_ms,
                 feasibility_requirements=(feasibility_requirements_by_provider or {}).get(
                     str(candidate.get("providerName", candidate.get("provider", "")))
                 ),
             ))
            for candidate in provider_candidates.get(role.role_id, [])
        ]
        role_scores[role.role_id] = scored
        valid = [item for item in scored if item["valid"]]
        rejected.extend(item for item in scored if not item["valid"])
        if not valid:
            raise ValueError(f"no valid provider for role {role.role_id}")
        selected = min(valid, key=lambda item: item["scoreMs"])
        role_assignments[role.role_id] = selected
    edge_costs: list[dict[str, Any]] = []
    edge_total = 0.0
    if network_matrix is not None:
        for dependency in template.dependencies:
            src = role_assignments[dependency.from_role]["provider"]
            dst = role_assignments[dependency.to_role]["provider"]
            cost, detail = network_matrix.transfer_cost_ms(src, dst, dependency.bytes_count)
            edge_total += cost
            edge_costs.append(detail)
    node_total = sum(float(item["scoreMs"]) for item in role_assignments.values())
    return RuntimeAssignment(
        request_id=request_id,
        template_id=template.template_id,
        role_assignments=role_assignments,
        score_breakdown={
            "nodeCostMs": node_total,
            "edgeCostMs": edge_total,
            "totalEstimatedMs": node_total + edge_total,
            "roleScores": role_scores,
            "edgeCosts": edge_costs,
            "rejectedCandidates": rejected,
            "excludedProviders": sorted(excluded),
        },
    )


def choose_edge_aware_runtime_assignment(template: PlanTemplate,
                                         provider_candidates: dict[str, list[dict[str, Any]]],
                                         *,
                                         request_id: str,
                                         runtime_required: bool = True,
                                         network_matrix: ProviderNetworkMatrix | None = None,
                                         excluded_providers: Iterable[str] = (),
                                         feasibility_requirements_by_provider:
                                         dict[str, PlanFeasibilityRequirementsV1] | None = None,
                                         telemetry_at_ms: int | None = None,
                                         ) -> RuntimeAssignment:
    excluded = set(str(item) for item in excluded_providers if str(item))
    scored_by_role: dict[str, list[dict[str, Any]]] = {}
    rejected: list[dict[str, Any]] = []
    for role in template.roles:
        scored = [
            ({
                "provider": str(candidate.get("providerName", candidate.get("provider", ""))),
                "roleId": role.role_id,
                "valid": False,
                "reason": "PROVIDER_EXCLUDED",
                "scoreMs": float("inf"),
            } if str(candidate.get("providerName", candidate.get("provider", ""))) in excluded
             else score_runtime_candidate(
                 role,
                 candidate,
                 runtime_required=runtime_required,
                 now_ms_value=telemetry_at_ms,
                 feasibility_requirements=(feasibility_requirements_by_provider or {}).get(
                     str(candidate.get("providerName", candidate.get("provider", "")))
                 ),
             ))
            for candidate in provider_candidates.get(role.role_id, [])
        ]
        scored_by_role[role.role_id] = scored
        rejected.extend(item for item in scored if not item["valid"])
        if not any(item["valid"] for item in scored):
            raise ValueError(f"no valid provider for role {role.role_id}")

    best: tuple[float, dict[str, dict[str, Any]], list[dict[str, Any]], float, float] | None = None
    role_ids = [role.role_id for role in template.roles]
    choices = [
        [item for item in scored_by_role[role_id] if item["valid"]]
        for role_id in role_ids
    ]
    for combo in itertools.product(*choices):
        assignment = {role_id: dict(item) for role_id, item in zip(role_ids, combo)}
        node_total = sum(float(item["scoreMs"]) for item in assignment.values())
        edge_total = 0.0
        edge_costs: list[dict[str, Any]] = []
        if network_matrix is not None:
            for dependency in template.dependencies:
                src = assignment[dependency.from_role]["provider"]
                dst = assignment[dependency.to_role]["provider"]
                cost, detail = network_matrix.transfer_cost_ms(src, dst, dependency.bytes_count)
                edge_total += cost
                edge_costs.append(detail)
        total = node_total + edge_total
        if best is None or total < best[0]:
            best = (total, assignment, edge_costs, node_total, edge_total)
    assert best is not None
    _, assignment, edge_costs, node_total, edge_total = best
    return RuntimeAssignment(
        request_id=request_id,
        template_id=template.template_id,
        role_assignments=assignment,
        score_breakdown={
            "nodeCostMs": node_total,
            "edgeCostMs": edge_total,
            "totalEstimatedMs": node_total + edge_total,
            "roleScores": scored_by_role,
            "edgeCosts": edge_costs,
            "rejectedCandidates": rejected,
            "plannerMode": "edge-aware-exhaustive",
            "excludedProviders": sorted(excluded),
        },
    )


def choose_bounded_replan_assignment(template: PlanTemplate,
                                     provider_candidates: dict[str, list[dict[str, Any]]],
                                     *,
                                     request_id: str,
                                     replan_records: Iterable[ReplanRecord] = (),
                                     max_attempts: int = 2,
                                     runtime_required: bool = True,
                                     network_matrix: ProviderNetworkMatrix | None = None,
                                     feasibility_requirements_by_provider:
                                     dict[str, PlanFeasibilityRequirementsV1] | None = None,
                                     telemetry_at_ms: int | None = None,
                                     ) -> RuntimeAssignment:
    records = list(replan_records)
    attempt = len(records) + 1
    excluded: set[str] = set()
    for record in records:
        excluded.update(record.excluded_provider_set())
    if attempt > max(1, int(max_attempts)):
        raise ValueError(
            "MAX_REPLAN_ATTEMPTS_EXCEEDED excludedProviders=" +
            ",".join(sorted(excluded)))
    assignment = choose_edge_aware_runtime_assignment(
        template,
        provider_candidates,
        request_id=request_id,
        runtime_required=runtime_required,
        network_matrix=network_matrix,
        excluded_providers=excluded,
        feasibility_requirements_by_provider=feasibility_requirements_by_provider,
        telemetry_at_ms=telemetry_at_ms,
    )
    breakdown = dict(assignment.score_breakdown)
    breakdown["replanCount"] = len(records)
    breakdown["replanAttempt"] = attempt
    breakdown["replanStatus"] = "executed" if records else "not-needed"
    breakdown["excludedProviders"] = sorted(excluded)
    return replace(assignment,
                   replan_attempt=len(records),
                   score_breakdown=breakdown)


def exact_token_prefix_digest(token_ids: Iterable[int], *,
                              tokenizer_id: str = "",
                              length: int = 24) -> str:
    """Digest token IDs, not text meaning, for strict forward-cache lookup."""

    normalized = [int(token_id) for token_id in token_ids]
    if any(token_id < 0 for token_id in normalized):
        raise ValueError("token IDs must be non-negative integers")
    return stable_digest({
        "schema": "ndnsf-di-exact-token-prefix-v1",
        "tokenizerId": tokenizer_id,
        "tokenIds": normalized,
    }, length=length)


# ---------------------------------------------------------------------------
# Placement helpers for deployment
# ---------------------------------------------------------------------------


def filter_feasible_providers(role_id: str,
                               candidates: list[dict[str, Any]],
                               constraint: dict[str, Any] | None = None,
                               *,
                               existing_deployments: dict[str, Any] | None = None,
                               already_placed: dict[str, str] | None = None,
                               excluded: set[str] | None = None,
                               circuit_open: set[str] | None = None) -> list[dict[str, Any]]:
    """Filter provider candidates by hard constraints.

    Checks: backend compatibility, GPU memory (with IDLE preemption),
    anti-affinity, max workers, circuit breaker.
    """
    excluded = excluded or set()
    circuit_open = circuit_open or set()
    already_placed = already_placed or {}
    feasible = []
    for c in candidates:
        provider = str(c.get("providerName", c.get("provider", "")))
        if not provider or provider in excluded or provider in circuit_open:
            continue
        if constraint:
            meta = _metadata_from_candidate(c)
            if meta is not None:
                di = _di_state_from_metadata(meta)
                hint = meta.provider_runtime_hint
                # 1. backend compatibility
                req_backend = constraint.get("required_backend", "")
                if req_backend and req_backend not in di.supported_backends:
                    continue
                # 2. GPU memory
                required_gpu = constraint.get("min_gpu_memory_mb", 0)
                if required_gpu > 0 and di.free_gpu_memory_mb < required_gpu:
                    can_preempt = False
                    if existing_deployments:
                        for did, dep in existing_deployments.items():
                            if (dep.get("status") in {"ACTIVE", "IDLE"} and
                                    int(dep.get("refCount", dep.get("ref_count", 0)) or 0) == 0):
                                fm = dep.get("fragmentMap", dep.get("fragment_map", {}))
                                for role_entries in fm.values():
                                    for frag in (role_entries if isinstance(role_entries, list) else []):
                                        if isinstance(frag, dict) and frag.get("provider") == provider:
                                            can_preempt = True
                                            break
                    if not can_preempt:
                        continue
                # 3. anti-affinity: cannot co-locate with listed roles
                anti_roles = constraint.get("anti_affinity", ())
                conflict = False
                for arole in (anti_roles if isinstance(anti_roles, (list, tuple)) else []):
                    if already_placed.get(arole) == provider:
                        conflict = True
                        break
                if conflict:
                    continue
                # 4. max workers
                max_w = constraint.get("max_workers_per_provider", 0)
                if max_w > 0 and hint.active_work_count >= max_w:
                    continue
        feasible.append(c)
    return feasible


# Placement scoring strategies — preset weights for different environments
PLACEMENT_STRATEGY_PRESETS: dict[str, dict[str, int]] = {
    "gpu-cluster": {
        "fragment": 50, "queue": 15, "edge": 5, "health": 15, "fairness": 10,
        "affinity_bonus": 5, "anti_affinity": 0,
    },
    "edge-network": {
        "fragment": 25, "queue": 10, "edge": 40, "health": 10, "fairness": 10,
        "affinity_bonus": 5, "anti_affinity": 0,
    },
    "multi-tenant": {
        "fragment": 30, "queue": 15, "edge": 10, "health": 15, "fairness": 25,
        "affinity_bonus": 5, "anti_affinity": 0,
    },
    "high-availability": {
        "fragment": 30, "queue": 15, "edge": 10, "health": 25, "fairness": 10,
        "affinity_bonus": 0, "anti_affinity": 10,
    },
}


def pick_optimal_placement(scored_by_role: dict[str, list[dict[str, Any]]],
                            strategy: str = "best_score",
                            *,
                            min_replicas: int = 1) -> dict[str, dict[str, Any]]:
    """Pick the best provider combination from scored candidates.

    Strategies:
      best_score:   highest total score per role (default)
      pack:         minimize distinct providers (consolidation)
      spread:       maximize distinct providers (fault tolerance)
      min_replicas: ensure at least N replicas per role
    """
    if strategy == "pack":
        return _pick_pack(scored_by_role)
    if strategy == "spread":
        return _pick_spread(scored_by_role)
    if strategy == "min_replicas":
        return _pick_min_replicas(scored_by_role, min_replicas)
    # default: best_score
    return _pick_best_score(scored_by_role)


def _pick_best_score(scored_by_role):
    result = {}
    for role, scored in scored_by_role.items():
        if scored:
            best = max(scored, key=lambda x: x.get("totalScore", x.get("scoreMs", 0)))
            result[role] = best
    return result


def _pick_pack(scored_by_role):
    """Minimize distinct providers — useful for energy efficiency."""
    result = {}
    used: set[str] = set()
    for role, scored in sorted(scored_by_role.items()):
        if not scored:
            continue
        # Prefer already-used providers (add bonus for reuse)
        scored_sorted = sorted(scored, key=lambda x: (
            0 if x.get("provider", "") in used else 1,
            -(x.get("totalScore", x.get("scoreMs", 0)))
        ))
        best = scored_sorted[0]
        result[role] = best
        used.add(best.get("provider", ""))
    return result


def _pick_spread(scored_by_role):
    """Maximize distinct providers — useful for fault tolerance."""
    result = {}
    used: set[str] = set()
    for role, scored in sorted(scored_by_role.items()):
        if not scored:
            continue
        scored_sorted = sorted(scored, key=lambda x: (
            0 if x.get("provider", "") not in used else 1,
            -(x.get("totalScore", x.get("scoreMs", 0)))
        ))
        best = scored_sorted[0]
        result[role] = best
        used.add(best.get("provider", ""))
    return result


def _pick_min_replicas(scored_by_role, min_replicas):
    result = {}
    for role, scored in scored_by_role.items():
        if not scored:
            continue
        scored_sorted = sorted(scored, key=lambda x: -(x.get("totalScore", x.get("scoreMs", 0))))
        picks = scored_sorted[:max(1, min_replicas)]
        result[role] = picks[0]  # primary
        if len(picks) > 1:
            result[f"{role}__replica_1"] = picks[1]
    return result


# Re-export for backward compatibility — old name kept for existing callers
_score_normalized = __import__("ndnsf.runtime_telemetry", fromlist=["score_normalized"]).score_normalized


def score_provider_candidates(role_id: str,
                               feasible: list[dict[str, Any]],
                               *,
                               runtime_required: bool = True,
                               now_ms_value: int | None = None) -> list[dict[str, Any]]:
    """Score each feasible candidate. Lower score = better."""
    current = now_ms() if now_ms_value is None else int(now_ms_value)
    scored = []
    for c in feasible:
        node = score_runtime_candidate(
            PlanRole(role_id, fragment_key=ModelFragmentKey()),
            c, runtime_required=runtime_required, now_ms_value=current)
        scored.append({
            "provider": str(c.get("providerName", c.get("provider", ""))),
            "scoreMs": node.get("scoreMs", float("inf")),
            "valid": node.get("valid", False),
            "residency": node.get("residency", "MISSING"),
            "details": node,
        })
    scored.sort(key=lambda x: x["scoreMs"])
    return scored


@dataclass(frozen=True)
class StageDefinitionV1:
    role: str
    stage_id: str = ""
    layer_start: int = 0
    layer_end: int = -1
    model_id: str = ""
    model_revision: str = ""
    plan_hash: str = ""
    split_layout_hash: str = ""
    runtime_backend: str = ""
    export_artifact_hash: str = ""

    @property
    def layer_count(self) -> int:
        if self.layer_end < self.layer_start:
            return 0
        return self.layer_end - self.layer_start + 1

    def digest(self) -> str:
        return stable_digest(self, length=24)


@dataclass(frozen=True)
class ExactForwardCacheKey:
    token_prefix_digest: str
    tokenizer_id: str
    model_id: str
    model_revision: str = ""
    model_artifact_hash: str = ""
    plan_hash: str = ""
    split_layout_hash: str = ""
    stage_definition_digest: str = ""
    role: str = ""
    layer_start: int = 0
    layer_end: int = -1
    runtime_backend: str = ""
    export_artifact_hash: str = ""
    position_state_digest: str = ""
    dtype: str = ""
    quantization: str = ""
    security_epoch: str = ""
    cache_epoch: str = ""
    cache_kind: ExactForwardCacheKind = ExactForwardCacheKind.KV_BLOCK

    def digest(self) -> str:
        return stable_digest(self, length=24)

    def data_name(self, *, provider: str = "") -> str:
        prefix = provider.rstrip("/") if provider else "/NDNSF/DI/PROVIDER-LOCAL"
        return f"{prefix}/EXACT-FORWARD-CACHE/{self.digest()}"


@dataclass(frozen=True)
class ExactForwardCacheEntry:
    key: ExactForwardCacheKey
    provider: str
    object_name: str = ""
    byte_count: int = 0
    token_count: int = 0
    created_at_ms: int = field(default_factory=now_ms)
    expires_at_ms: int = 0
    local_only: bool = True
    metadata: dict[str, Any] = field(default_factory=dict)

    @property
    def key_digest(self) -> str:
        return self.key.digest()

    def is_valid(self, *, now_ms_value: int | None = None) -> bool:
        current = now_ms() if now_ms_value is None else int(now_ms_value)
        return not self.expires_at_ms or current < self.expires_at_ms


class ExactForwardCacheManager:
    def __init__(self, *, budget_mb: float = 0.0):
        self.budget_mb = float(budget_mb)
        self._entries: dict[str, ExactForwardCacheEntry] = {}
        self._hits = 0
        self._misses = 0
        self._evictions = 0

    @property
    def used_mb(self) -> float:
        return sum(entry.byte_count for entry in self._entries.values()) / (1024.0 * 1024.0)

    def put(self, entry: ExactForwardCacheEntry) -> None:
        self._entries[entry.key_digest] = entry
        self._evict_if_needed()

    def get(self, key: ExactForwardCacheKey, *,
            now_ms_value: int | None = None) -> ExactForwardCacheEntry | None:
        digest = key.digest()
        entry = self._entries.get(digest)
        if entry is None or entry.key != key or not entry.is_valid(now_ms_value=now_ms_value):
            if entry is not None and not entry.is_valid(now_ms_value=now_ms_value):
                self._entries.pop(digest, None)
                self._evictions += 1
            self._misses += 1
            return None
        self._hits += 1
        return entry

    def telemetry(self) -> KvCacheTelemetry:
        return KvCacheTelemetry(
            budget_mb=self.budget_mb,
            used_mb=self.used_mb,
            resident_exact_cache_key_digests=tuple(sorted(self._entries)),
            hits=self._hits,
            misses=self._misses,
            evictions=self._evictions,
        )

    def _evict_if_needed(self) -> None:
        if self.budget_mb <= 0:
            return
        while self.used_mb > self.budget_mb and self._entries:
            oldest_key = min(
                self._entries,
                key=lambda item: self._entries[item].created_at_ms,
            )
            self._entries.pop(oldest_key, None)
            self._evictions += 1



def stage_definition_from_plan_stage(stage: dict[str, Any], *,
                                     model: ModelManifestV1,
                                     plan_hash: str = "",
                                     split_layout_hash: str = "",
                                     runtime_backend: str = "",
                                     export_artifact_hash: str = "") -> StageDefinitionV1:
    raw_layer_start = stage.get("layerStart", stage.get("layer_start", 0))
    raw_layer_end = stage.get("layerEnd", stage.get("layer_end", -1))
    return StageDefinitionV1(
        role=str(stage.get("role", "")),
        stage_id=str(stage.get("stageId", stage.get("stage_id", ""))),
        layer_start=int(0 if raw_layer_start is None else raw_layer_start),
        layer_end=int(-1 if raw_layer_end is None else raw_layer_end),
        model_id=model.model_id,
        model_revision=model.revision,
        plan_hash=plan_hash,
        split_layout_hash=split_layout_hash,
        runtime_backend=runtime_backend,
        export_artifact_hash=export_artifact_hash,
    )


def exact_forward_cache_key_for_stage(model: ModelManifestV1,
                                      stage: dict[str, Any] | StageDefinitionV1,
                                      *,
                                      token_ids: Iterable[int],
                                      plan_hash: str,
                                      split_layout_hash: str = "",
                                      model_artifact_hash: str = "",
                                      runtime_backend: str = "",
                                      export_artifact_hash: str = "",
                                      position_state: dict[str, Any] | None = None,
                                      dtype: str = "",
                                      quantization: str = "",
                                      security_epoch: str = "",
                                      cache_epoch: str = "",
                                      cache_kind: ExactForwardCacheKind = ExactForwardCacheKind.KV_BLOCK
                                      ) -> ExactForwardCacheKey:
    stage_definition = (
        stage if isinstance(stage, StageDefinitionV1)
        else stage_definition_from_plan_stage(
            stage,
            model=model,
            plan_hash=plan_hash,
            split_layout_hash=split_layout_hash,
            runtime_backend=runtime_backend,
            export_artifact_hash=export_artifact_hash,
        )
    )
    return ExactForwardCacheKey(
        token_prefix_digest=exact_token_prefix_digest(token_ids, tokenizer_id=model.tokenizer_id),
        tokenizer_id=model.tokenizer_id,
        model_id=model.model_id,
        model_revision=model.revision,
        model_artifact_hash=model_artifact_hash,
        plan_hash=plan_hash,
        split_layout_hash=split_layout_hash,
        stage_definition_digest=stage_definition.digest(),
        role=stage_definition.role,
        layer_start=stage_definition.layer_start,
        layer_end=stage_definition.layer_end,
        runtime_backend=runtime_backend,
        export_artifact_hash=export_artifact_hash,
        position_state_digest=stable_digest(position_state or {}, length=24),
        dtype=dtype,
        quantization=quantization,
        security_epoch=security_epoch,
        cache_epoch=cache_epoch,
        cache_kind=cache_kind,
    )


@dataclass(frozen=True)
class ContextStateObject:
    kind: ContextObjectKind
    object_id: str
    model_id: str
    tokenizer_id: str = ""
    session_id: str = ""
    prefix_id: str = ""
    provider: str = ""
    token_start: int = 0
    token_count: int = 0
    byte_count: int = 0
    digest: str = ""
    data_name: str = ""
    local_only: bool = False
    expires_at_ms: int = 0
    metadata: dict[str, Any] = field(default_factory=dict)

    def cache_key(self) -> str:
        return stable_digest({
            "kind": self.kind.value,
            "objectId": self.object_id,
            "modelId": self.model_id,
            "tokenizerId": self.tokenizer_id,
            "sessionId": self.session_id,
            "prefixId": self.prefix_id,
            "provider": self.provider,
        }, length=24)


def prompt_chunk(**kwargs: Any) -> ContextStateObject:
    return ContextStateObject(kind=ContextObjectKind.PROMPT_CHUNK, **kwargs)


def prefix_state(**kwargs: Any) -> ContextStateObject:
    prefix_id = str(kwargs.get("prefix_id", kwargs.get("object_id", "")))
    kwargs.setdefault("object_id", prefix_id)
    kwargs.setdefault("local_only", True)
    return ContextStateObject(kind=ContextObjectKind.PREFIX_STATE, **kwargs)


def session_state(**kwargs: Any) -> ContextStateObject:
    session_id = str(kwargs.get("session_id", kwargs.get("object_id", "")))
    kwargs.setdefault("object_id", session_id)
    kwargs.setdefault("local_only", True)
    return ContextStateObject(kind=ContextObjectKind.SESSION_STATE, **kwargs)


def kv_block(**kwargs: Any) -> ContextStateObject:
    kwargs.setdefault("local_only", True)
    return ContextStateObject(kind=ContextObjectKind.KV_BLOCK, **kwargs)


def generation_chunk(**kwargs: Any) -> ContextStateObject:
    return ContextStateObject(kind=ContextObjectKind.GENERATION_CHUNK, **kwargs)


@dataclass(frozen=True)
class CachePlacementDecision:
    provider: str
    reason: str
    prefix_id: str = ""
    session_id: str = ""
    expected_hit: bool = False
    kv_memory_mb: float = 0.0
    migration_required: bool = False


@dataclass(frozen=True)
class PlanKeyV1:
    model_id: str
    model_revision: str = ""
    context_class: str = "short"
    target_rps: float = 0.0
    provider_set: tuple[str, ...] = ()
    capability_version: str = ""
    network_version: str = ""
    cache_version: str = ""
    planner_mode: str = "proportional"

    def digest(self) -> str:
        return stable_digest(self, length=24)


@dataclass(frozen=True)
class PlanLeaseBindingsV1:
    """Exact external facts that make a cached plan safe to reuse."""

    membership_version: str
    provider_boot_ids: dict[str, str]
    evidence_epochs: dict[str, int]
    runtime_identity_digests: dict[str, str]
    telemetry_versions: dict[str, str]
    network_profile_version: str
    cache_version: str

    @classmethod
    def capture(
        cls,
        telemetry_by_provider: dict[str, MeasuredTelemetrySnapshotV1],
        *,
        provider_set: Iterable[str],
        membership_version: str,
        network_profile_version: str,
        cache_version: str,
    ) -> "PlanLeaseBindingsV1":
        expected = set(str(item) for item in provider_set)
        observed = set(telemetry_by_provider)
        if observed != expected:
            raise ValueError(
                "plan lease telemetry provider set mismatch: "
                f"expected={sorted(expected)!r} observed={sorted(observed)!r}")
        if not membership_version or not network_profile_version or not cache_version:
            raise ValueError(
                "bound plan lease requires membership, network-profile, and cache versions")

        boot_ids: dict[str, str] = {}
        evidence_epochs: dict[str, int] = {}
        runtime_digests: dict[str, str] = {}
        telemetry_versions: dict[str, str] = {}
        for provider in sorted(expected):
            item = telemetry_by_provider[provider]
            if item.provider_name != provider:
                raise ValueError(f"telemetry provider identity mismatch for {provider}")
            if item.status != "measured" or item.source in {
                "", "configured", "profile", "unavailable"
            }:
                raise ValueError(f"bound plan lease requires measured telemetry for {provider}")
            if item.membership_version != membership_version:
                raise ValueError(f"membership version mismatch for {provider}")
            if item.network_profile_version != network_profile_version:
                raise ValueError(f"network-profile version mismatch for {provider}")
            if item.cache_version != cache_version:
                raise ValueError(f"cache version mismatch for {provider}")
            if not item.provider_boot_id or item.evidence_epoch <= 0 or item.sequence <= 0:
                raise ValueError(f"incomplete telemetry binding for {provider}")
            boot_ids[provider] = item.provider_boot_id
            evidence_epochs[provider] = item.evidence_epoch
            runtime_digests[provider] = stable_digest({
                "runnerKind": item.runner_kind,
                "runtimeVersion": item.runtime_version,
                "modelDigest": item.model_digest,
                "planDigest": item.plan_digest,
                "artifactDigests": item.artifact_digests,
                "deviceId": item.device_id,
            }, length=32)
            telemetry_versions[provider] = (
                f"{item.provider_boot_id}:{item.sequence}:{item.resource_sequence}")
        return cls(
            membership_version=membership_version,
            provider_boot_ids=boot_ids,
            evidence_epochs=evidence_epochs,
            runtime_identity_digests=runtime_digests,
            telemetry_versions=telemetry_versions,
            network_profile_version=network_profile_version,
            cache_version=cache_version,
        )


@dataclass(frozen=True)
class PlanLeaseV1:
    plan_key: PlanKeyV1
    plan_id: str
    layout: dict[str, Any]
    prediction: dict[str, Any] = field(default_factory=dict)
    cache_placement: CachePlacementDecision | None = None
    fallback_plan_ids: tuple[str, ...] = ()
    created_at_ms: int = field(default_factory=now_ms)
    expires_at_ms: int = 0
    valid_until_versions: dict[str, str] = field(default_factory=dict)
    bindings: PlanLeaseBindingsV1 | None = None

    def is_valid(self, *, now_ms_value: int | None = None,
                 versions: dict[str, str] | None = None,
                 bindings: PlanLeaseBindingsV1 | None = None) -> bool:
        current = now_ms() if now_ms_value is None else int(now_ms_value)
        if self.expires_at_ms and current >= self.expires_at_ms:
            return False
        for key, value in (versions or {}).items():
            expected = self.valid_until_versions.get(key)
            if expected is not None and expected != value:
                return False
        if self.bindings is not None and bindings != self.bindings:
            return False
        return True


class PlanCache:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        self._leases: dict[str, PlanLeaseV1] = {}
        if self.path.exists():
            self.load()

    def put(self, lease: PlanLeaseV1) -> None:
        self._leases[lease.plan_key.digest()] = lease

    def get(self, key: PlanKeyV1, *, versions: dict[str, str] | None = None,
            bindings: PlanLeaseBindingsV1 | None = None) -> PlanLeaseV1 | None:
        lease = self._leases.get(key.digest())
        if lease is None:
            return None
        return lease if lease.is_valid(versions=versions, bindings=bindings) else None

    def load(self) -> None:
        payload = read_json(self.path)
        self._leases = {}
        for item in payload.get("leases", []):
            lease = lease_from_dict(item)
            self._leases[lease.plan_key.digest()] = lease

    def save(self) -> None:
        write_json(self.path, {
            "schema": "ndnsf-di-plan-cache-v1",
            "leases": list(self._leases.values()),
        })


@dataclass(frozen=True)
class RoleWorkItem:
    request_id: str
    role: str
    provider: str
    dependencies: tuple[str, ...] = ()
    deadline_ms: int = 0
    sequence: int = 0


@dataclass(frozen=True)
class RoleWorkSnapshot:
    request_id: str
    role: str
    provider: str
    status: RoleWorkStatus
    dependencies: tuple[str, ...] = ()


class RolePipelineScheduler:
    """Small role-level scheduler contract used by Runtime v1 tests.

    It models the key runtime rule: each provider advances independently.  When
    a provider finishes its role for request N, it can immediately start ready
    work for request N+1 without waiting for all providers to finish request N.
    """

    def __init__(self, *, max_active_per_provider: int = 1):
        self.max_active_per_provider = max(1, int(max_active_per_provider))
        self._items: dict[tuple[str, str], RoleWorkItem] = {}
        self._status: dict[tuple[str, str], RoleWorkStatus] = {}
        self._completed: set[tuple[str, str]] = set()

    def submit(self, item: RoleWorkItem) -> None:
        key = (item.request_id, item.role)
        self._items[key] = item
        self._status[key] = (
            RoleWorkStatus.READY
            if self._deps_ready(item) else
            RoleWorkStatus.WAITING_DEPENDENCY
        )

    def complete(self, request_id: str, role: str) -> None:
        key = (request_id, role)
        self._status[key] = RoleWorkStatus.DONE
        self._completed.add(key)
        for item_key, item in self._items.items():
            if self._status.get(item_key) == RoleWorkStatus.WAITING_DEPENDENCY and self._deps_ready(item):
                self._status[item_key] = RoleWorkStatus.READY

    def cancel_expired(self, *, now_ms_value: int | None = None) -> int:
        current = now_ms() if now_ms_value is None else int(now_ms_value)
        cancelled = 0
        for key, item in self._items.items():
            if item.deadline_ms and current >= item.deadline_ms:
                if self._status.get(key) not in {
                    RoleWorkStatus.DONE,
                    RoleWorkStatus.CANCELLED,
                    RoleWorkStatus.FAILED,
                }:
                    self._status[key] = RoleWorkStatus.CANCELLED
                    cancelled += 1
        return cancelled

    def next_ready(self, provider: str) -> RoleWorkItem | None:
        active = sum(
            1 for key, status in self._status.items()
            if status == RoleWorkStatus.RUNNING and self._items[key].provider == provider
        )
        if active >= self.max_active_per_provider:
            return None
        ready = [
            item for key, item in self._items.items()
            if item.provider == provider and self._status.get(key) == RoleWorkStatus.READY
        ]
        if not ready:
            return None
        ready.sort(key=lambda item: (item.deadline_ms or 2**63 - 1, item.sequence, item.request_id))
        item = ready[0]
        self._status[(item.request_id, item.role)] = RoleWorkStatus.RUNNING
        return item

    def snapshot(self) -> list[RoleWorkSnapshot]:
        return [
            RoleWorkSnapshot(
                request_id=item.request_id,
                role=item.role,
                provider=item.provider,
                status=self._status[key],
                dependencies=item.dependencies,
            )
            for key, item in sorted(self._items.items())
        ]

    def _deps_ready(self, item: RoleWorkItem) -> bool:
        return all((item.request_id, dep) in self._completed for dep in item.dependencies)


@dataclass(frozen=True)
class DependencyTransferItem:
    item_id: str
    kind: TransferQueueKind
    bytes_count: int
    priority: int = 0
    deadline_ms: int = 0


@dataclass(frozen=True)
class DependencyTransferSnapshot:
    prefetch_depth: int
    publish_depth: int
    active_prefetch: int
    active_publish: int
    expired: int


class BoundedDependencyTransferQueues:
    """Bounded prefetch/publish queues for dependency data-plane contracts."""

    def __init__(self, *, prefetch_window: int = 4, publish_window: int = 4):
        self.prefetch_window = max(1, int(prefetch_window))
        self.publish_window = max(1, int(publish_window))
        self._prefetch: list[DependencyTransferItem] = []
        self._publish: list[DependencyTransferItem] = []
        self._active_prefetch = 0
        self._active_publish = 0
        self._expired = 0

    def submit(self, item: DependencyTransferItem) -> None:
        queue = self._prefetch if item.kind == TransferQueueKind.PREFETCH else self._publish
        queue.append(item)
        queue.sort(key=lambda entry: (-entry.priority, entry.deadline_ms or 2**63 - 1, entry.item_id))

    def next(self, kind: TransferQueueKind, *, now_ms_value: int | None = None) -> DependencyTransferItem | None:
        current = now_ms() if now_ms_value is None else int(now_ms_value)
        queue = self._prefetch if kind == TransferQueueKind.PREFETCH else self._publish
        active = self._active_prefetch if kind == TransferQueueKind.PREFETCH else self._active_publish
        window = self.prefetch_window if kind == TransferQueueKind.PREFETCH else self.publish_window
        if active >= window:
            return None
        while queue:
            item = queue.pop(0)
            if item.deadline_ms and current >= item.deadline_ms:
                self._expired += 1
                continue
            if kind == TransferQueueKind.PREFETCH:
                self._active_prefetch += 1
            else:
                self._active_publish += 1
            return item
        return None

    def complete(self, kind: TransferQueueKind) -> None:
        if kind == TransferQueueKind.PREFETCH and self._active_prefetch > 0:
            self._active_prefetch -= 1
        elif kind == TransferQueueKind.PUBLISH and self._active_publish > 0:
            self._active_publish -= 1

    def snapshot(self) -> DependencyTransferSnapshot:
        return DependencyTransferSnapshot(
            prefetch_depth=len(self._prefetch),
            publish_depth=len(self._publish),
            active_prefetch=self._active_prefetch,
            active_publish=self._active_publish,
            expired=self._expired,
        )


@dataclass(frozen=True)
class FallbackPlan:
    plan_id: str
    reason: str
    layout: dict[str, Any]


def generate_fallback_plans(plan: dict[str, Any]) -> list[FallbackPlan]:
    stages = list(plan.get("stages", []))
    fallbacks: list[FallbackPlan] = []
    if stages:
        provider_counts: dict[str, int] = {}
        for stage in stages:
            provider = str(stage.get("provider", ""))
            provider_counts[provider] = provider_counts.get(provider, 0) + 1
        if provider_counts:
            primary = max(provider_counts, key=lambda provider: provider_counts[provider])
            single = dict(plan)
            single["stages"] = [
                {**dict(stage), "provider": primary}
                for stage in stages
            ]
            single["plannerMode"] = "fallback-single-provider"
            fallbacks.append(FallbackPlan(
                plan_id=stable_digest(single, length=16),
                reason="single-provider fallback avoids cross-provider dependency transfer",
                layout=single,
            ))
    defer = dict(plan)
    defer["plannerMode"] = "fallback-defer"
    fallbacks.append(FallbackPlan(
        plan_id=stable_digest(defer, length=16),
        reason="defer/reject when all feasible providers are saturated or context state is unavailable",
        layout=defer,
    ))
    return fallbacks


@dataclass(frozen=True)
class RetryPolicy:
    max_attempts: int = 2
    straggler_factor: float = 2.0
    provider_failure_backoff_ms: int = 500

    def action_for(self, *, attempt: int, same_provider_available: bool,
                   alternate_provider_available: bool,
                   fallback_available: bool) -> FailureAction:
        if attempt < self.max_attempts and same_provider_available:
            return FailureAction.RETRY_SAME_PROVIDER
        if alternate_provider_available:
            return FailureAction.RETRY_ALTERNATE_PROVIDER
        if fallback_available:
            return FailureAction.USE_FALLBACK_PLAN
        return FailureAction.REJECT_OR_DEFER

    def is_straggler(self, observed_ms: float, expected_ms: float) -> bool:
        return expected_ms > 0 and observed_ms >= expected_ms * self.straggler_factor


def adaptive_segment_size(byte_count: int, *,
                          rtt_ms: float = 10.0,
                          bandwidth_mbps: float = 1000.0,
                          min_size: int = 4096,
                          max_size: int = 65536) -> int:
    if byte_count <= 0:
        return min_size
    bandwidth_bytes_per_ms = max(1.0, bandwidth_mbps * 1_000_000.0 / 8.0 / 1000.0)
    bdp = bandwidth_bytes_per_ms * max(1.0, rtt_ms)
    target = int(min(max_size, max(min_size, bdp / 4.0)))
    return min(max_size, max(min_size, min(byte_count, target)))


def compress_payload(payload: bytes, *, enabled: bool = True,
                     min_bytes: int = 1024) -> tuple[bytes, dict[str, Any]]:
    if not enabled or len(payload) < min_bytes:
        return payload, {"compression": "none", "originalBytes": len(payload), "bytes": len(payload)}
    compressed = zlib.compress(payload)
    if len(compressed) >= len(payload):
        return payload, {"compression": "none", "originalBytes": len(payload), "bytes": len(payload)}
    return compressed, {
        "compression": "zlib",
        "originalBytes": len(payload),
        "bytes": len(compressed),
        "ratio": round(len(compressed) / max(1, len(payload)), 6),
    }


@dataclass(frozen=True)
class PrefillDecodeResult:
    request_id: str
    provider: str
    prompt_tokens: int
    generated_tokens: int
    prefill_ms: float
    decode_ms: float
    time_to_first_token_ms: float
    inter_token_ms: float
    chunks: tuple[ContextStateObject, ...]


def simulate_prefill_decode(*, request_id: str, provider: ProviderProfileV1,
                            model: ModelManifestV1, prompt_tokens: int,
                            generated_tokens: int,
                            microbatch: int = 1) -> PrefillDecodeResult:
    microbatch = max(1, int(microbatch))
    compute = max(provider.flops_tflops, 0.001)
    layer_factor = max(model.layers, 1)
    prefill_ms = prompt_tokens * layer_factor / compute / 10.0
    decode_ms = generated_tokens * layer_factor / compute / microbatch
    inter_token = decode_ms / max(1, generated_tokens)
    chunks = tuple(
        generation_chunk(
            object_id=f"{request_id}-chunk-{index}",
            model_id=model.model_id,
            session_id=request_id,
            token_start=index * microbatch,
            token_count=min(microbatch, generated_tokens - index * microbatch),
            byte_count=min(microbatch, generated_tokens - index * microbatch) * 4,
        )
        for index in range((generated_tokens + microbatch - 1) // microbatch)
    )
    return PrefillDecodeResult(
        request_id=request_id,
        provider=provider.provider,
        prompt_tokens=prompt_tokens,
        generated_tokens=generated_tokens,
        prefill_ms=round(prefill_ms, 3),
        decode_ms=round(decode_ms, 3),
        time_to_first_token_ms=round(prefill_ms + inter_token, 3),
        inter_token_ms=round(inter_token, 3),
        chunks=chunks,
    )


class LongContextManager:
    def __init__(self, *, budget_mb: float):
        self.budget_mb = float(budget_mb)
        self._objects: dict[str, ContextStateObject] = {}
        self._pinned: set[str] = set()
        self._events: list[dict[str, Any]] = []

    @property
    def used_mb(self) -> float:
        return sum(obj.byte_count for obj in self._objects.values()) / (1024.0 * 1024.0)

    def put(self, obj: ContextStateObject, *, pin: bool = False) -> None:
        key = obj.cache_key()
        self._objects[key] = obj
        if pin:
            self._pinned.add(key)
            self._record(CacheEventKind.PIN, obj)
        self._evict_if_needed()

    def get(self, kind: ContextObjectKind, *, object_id: str = "",
            session_id: str = "", prefix_id: str = "") -> ContextStateObject | None:
        for key, obj in list(self._objects.items()):
            if obj.expires_at_ms and now_ms() >= obj.expires_at_ms:
                self._record(CacheEventKind.EXPIRE, obj)
                self._objects.pop(key, None)
                self._pinned.discard(key)
                continue
            if obj.kind != kind:
                continue
            if object_id and obj.object_id != object_id:
                continue
            if session_id and obj.session_id != session_id:
                continue
            if prefix_id and obj.prefix_id != prefix_id:
                continue
            self._record(CacheEventKind.HIT, obj)
            return obj
        self._record(CacheEventKind.MISS, ContextStateObject(
            kind=kind,
            object_id=object_id or session_id or prefix_id or "unknown",
            model_id="",
            session_id=session_id,
            prefix_id=prefix_id,
        ))
        return None

    def telemetry(self) -> KvCacheTelemetry:
        return KvCacheTelemetry(
            budget_mb=self.budget_mb,
            used_mb=self.used_mb,
            resident_prefix_ids=tuple(sorted(
                obj.prefix_id for obj in self._objects.values() if obj.prefix_id)),
            resident_session_ids=tuple(sorted(
                obj.session_id for obj in self._objects.values() if obj.session_id)),
            hits=sum(1 for event in self._events if event["event"] == CacheEventKind.HIT.value),
            misses=sum(1 for event in self._events if event["event"] == CacheEventKind.MISS.value),
            evictions=sum(1 for event in self._events if event["event"] == CacheEventKind.EVICT.value),
        )

    def events(self) -> list[dict[str, Any]]:
        return list(self._events)

    def _evict_if_needed(self) -> None:
        while self.used_mb > self.budget_mb and self._objects:
            evict_key = next((key for key in self._objects if key not in self._pinned), "")
            if not evict_key:
                break
            obj = self._objects.pop(evict_key)
            self._record(CacheEventKind.EVICT, obj)

    def _record(self, event: CacheEventKind, obj: ContextStateObject) -> None:
        self._events.append({
            "timestampMs": now_ms(),
            "event": event.value,
            "kind": obj.kind.value,
            "objectId": obj.object_id,
            "sessionId": obj.session_id,
            "prefixId": obj.prefix_id,
        })


def choose_cache_placement(providers: Iterable[ProviderProfileV1],
                           telemetry: dict[str, RuntimeTelemetryV1],
                           *,
                           prefix_id: str = "",
                           session_id: str = "",
                           required_kv_mb: float = 0.0) -> CachePlacementDecision:
    best: tuple[float, ProviderProfileV1, bool] | None = None
    candidates = list(providers)
    if not candidates:
        raise ValueError("cache placement requires providers")
    for provider in candidates:
        snapshot = telemetry.get(provider.provider)
        hit = False
        free = provider.kv_cache_budget_mb
        if snapshot is not None:
            hit = (
                bool(prefix_id and prefix_id in snapshot.kv_cache.resident_prefix_ids) or
                bool(session_id and session_id in snapshot.kv_cache.resident_session_ids)
            )
            free = snapshot.kv_cache.free_mb
        if free < required_kv_mb and not hit:
            continue
        score = (1_000_000.0 if hit else 0.0) + free + provider.flops_tflops
        if best is None or score > best[0]:
            best = (score, provider, hit)
    if best is None:
        provider = max(candidates, key=lambda item: item.kv_cache_budget_mb)
        return CachePlacementDecision(
            provider=provider.provider,
            reason="no-provider-has-free-cache; choose largest cache for fallback",
            prefix_id=prefix_id,
            session_id=session_id,
            expected_hit=False,
            kv_memory_mb=required_kv_mb,
            migration_required=True,
        )
    _, provider, hit = best
    return CachePlacementDecision(
        provider=provider.provider,
        reason="cache-hit" if hit else "largest-free-cache-capacity",
        prefix_id=prefix_id,
        session_id=session_id,
        expected_hit=hit,
        kv_memory_mb=required_kv_mb,
        migration_required=not hit and bool(prefix_id or session_id),
    )


def proportional_layer_allocation(providers: list[ProviderProfileV1],
                                  layers: int) -> dict[str, int]:
    if layers <= 0:
        raise ValueError("layers must be positive")
    max_memory = max(provider.llm_stage_capacity_mb for provider in providers)
    max_flops = max(provider.flops_tflops for provider in providers)
    weighted = [
        (provider, provider.effective_capacity_weight(max_memory, max_flops))
        for provider in providers
    ]
    usable = [(provider, weight) for provider, weight in weighted if weight > 0.0]
    if not usable:
        raise ValueError("no provider has positive effective capacity")
    total_weight = sum(weight for _, weight in usable)
    raw = {provider.provider: layers * weight / total_weight for provider, weight in usable}
    allocation = {provider.provider: int(raw[provider.provider]) for provider, _ in usable}
    while sum(allocation.values()) < layers:
        provider = max(
            usable,
            key=lambda item: (raw[item[0].provider] - allocation[item[0].provider], item[1]),
        )[0]
        allocation[provider.provider] += 1
    return allocation


def validate_linear_llm_plan(plan: dict[str, Any]) -> None:
    stages = list(plan.get("stages", []))
    if not stages:
        raise ValueError("plan must contain at least one stage")
    expected_start = 0
    seen_roles: set[str] = set()
    for index, stage in enumerate(stages):
        role = str(stage.get("role", ""))
        if not role:
            raise ValueError(f"stage {index} has no role")
        if role in seen_roles:
            raise ValueError(f"duplicate role: {role}")
        seen_roles.add(role)
        layer_start = int(stage.get("layerStart", -1))
        layer_end = int(stage.get("layerEnd", -1))
        if layer_start != expected_start:
            raise ValueError(
                f"stage {index} starts at layer {layer_start}, expected {expected_start}")
        if layer_end < layer_start:
            raise ValueError(f"stage {index} has invalid layer range")
        expected_start = layer_end + 1
    for dep in plan.get("dependencies", []):
        if "from" not in dep or "to" not in dep:
            raise ValueError("dependency must contain from and to")
    for shard in plan.get("shards", []):
        if not shard.get("reason"):
            raise ValueError("sharded stages must explain why sharding is required")


def make_plan_lease(plan: dict[str, Any], *,
                    model: ModelManifestV1,
                    providers: list[ProviderProfileV1],
                    context_class: str = "short",
                    target_rps: float = 0.0,
                    cache_placement: CachePlacementDecision | None = None,
                    telemetry_by_provider:
                    dict[str, MeasuredTelemetrySnapshotV1] | None = None,
                    membership_version: str = "",
                    network_profile_version: str = "",
                    cache_version: str = "",
                    ttl_ms: int = 0) -> PlanLeaseV1:
    provider_set = tuple(sorted(provider.provider for provider in providers))
    capability_version = stable_digest([to_plain(provider) for provider in providers], length=16)
    effective_cache_version = (
        cache_version or
        (stable_digest(cache_placement, length=16) if cache_placement else "")
    )
    bindings = None
    if telemetry_by_provider is not None:
        bindings = PlanLeaseBindingsV1.capture(
            telemetry_by_provider,
            provider_set=provider_set,
            membership_version=membership_version,
            network_profile_version=network_profile_version,
            cache_version=effective_cache_version,
        )
    key = PlanKeyV1(
        model_id=model.model_id,
        model_revision=model.revision,
        context_class=context_class,
        target_rps=target_rps,
        provider_set=provider_set,
        capability_version=capability_version,
        network_version=network_profile_version,
        cache_version=effective_cache_version,
        planner_mode=str(plan.get("plannerMode", "proportional")),
    )
    created = now_ms()
    return PlanLeaseV1(
        plan_key=key,
        plan_id=str(plan.get("planId") or key.digest()),
        layout=plan,
        prediction=dict(plan.get("prediction", {})),
        cache_placement=cache_placement,
        fallback_plan_ids=tuple(
            str(item.get("planId", item.get("plan_id", "")))
            for item in plan.get("fallbackPlans", [])
            if item.get("planId", item.get("plan_id", ""))),
        created_at_ms=created,
        expires_at_ms=(created + ttl_ms if ttl_ms > 0 else 0),
        valid_until_versions={
            "capability": capability_version,
            **({"membership": membership_version} if membership_version else {}),
            **({"network": network_profile_version} if network_profile_version else {}),
            **({"cache": effective_cache_version} if effective_cache_version else {}),
        },
        bindings=bindings,
    )


def lease_from_dict(payload: dict[str, Any]) -> PlanLeaseV1:
    key_payload = dict(payload["plan_key"])
    placement_payload = payload.get("cache_placement")
    placement = CachePlacementDecision(**placement_payload) if placement_payload else None
    bindings_payload = payload.get("bindings")
    bindings = None
    if bindings_payload:
        bindings = PlanLeaseBindingsV1(
            membership_version=str(bindings_payload["membership_version"]),
            provider_boot_ids={
                str(key): str(value)
                for key, value in bindings_payload["provider_boot_ids"].items()
            },
            evidence_epochs={
                str(key): int(value)
                for key, value in bindings_payload["evidence_epochs"].items()
            },
            runtime_identity_digests={
                str(key): str(value)
                for key, value in bindings_payload["runtime_identity_digests"].items()
            },
            telemetry_versions={
                str(key): str(value)
                for key, value in bindings_payload["telemetry_versions"].items()
            },
            network_profile_version=str(bindings_payload["network_profile_version"]),
            cache_version=str(bindings_payload["cache_version"]),
        )
    return PlanLeaseV1(
        plan_key=PlanKeyV1(
            model_id=key_payload["model_id"],
            model_revision=key_payload.get("model_revision", ""),
            context_class=key_payload.get("context_class", "short"),
            target_rps=float(key_payload.get("target_rps", 0.0)),
            provider_set=tuple(key_payload.get("provider_set", ())),
            capability_version=key_payload.get("capability_version", ""),
            network_version=key_payload.get("network_version", ""),
            cache_version=key_payload.get("cache_version", ""),
            planner_mode=key_payload.get("planner_mode", "proportional"),
        ),
        plan_id=payload["plan_id"],
        layout=dict(payload.get("layout", {})),
        prediction=dict(payload.get("prediction", {})),
        cache_placement=placement,
        fallback_plan_ids=tuple(payload.get("fallback_plan_ids", ())),
        created_at_ms=int(payload.get("created_at_ms", 0)),
        expires_at_ms=int(payload.get("expires_at_ms", 0)),
        valid_until_versions=dict(payload.get("valid_until_versions", {})),
        bindings=bindings,
    )


def export_telemetry_csv(path: str | Path, telemetry: Iterable[RuntimeTelemetryV1]) -> None:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=[
            "provider", "timestamp_ms", "queue", "ready_queue",
            "waiting_dependencies", "active_workers", "free_memory_mb",
            "model_loaded", "runtime_backend", "kv_budget_mb", "kv_used_mb",
            "kv_hits", "kv_misses", "kv_evictions", "exact_cache_key_digests",
        ])
        writer.writeheader()
        for item in telemetry:
            writer.writerow({
                "provider": item.provider,
                "timestamp_ms": item.timestamp_ms,
                "queue": item.aggregate_queue,
                "ready_queue": item.ready_queue,
                "waiting_dependencies": item.waiting_dependencies,
                "active_workers": item.active_workers,
                "free_memory_mb": item.free_memory_mb,
                "model_loaded": int(item.model_loaded),
                "runtime_backend": item.runtime_backend,
                "kv_budget_mb": item.kv_cache.budget_mb,
                "kv_used_mb": item.kv_cache.used_mb,
                "kv_hits": item.kv_cache.hits,
                "kv_misses": item.kv_cache.misses,
                "kv_evictions": item.kv_cache.evictions,
                "exact_cache_key_digests": ",".join(item.kv_cache.resident_exact_cache_key_digests),
            })


def write_runtime_report(path: str | Path, *,
                         lease: PlanLeaseV1 | None = None,
                         telemetry: dict[str, RuntimeTelemetryV1] | None = None,
                         cache_events: list[dict[str, Any]] | None = None,
                         decision_table: list[dict[str, Any]] | None = None) -> None:
    write_json(path, {
        "schema": "ndnsf-di-runtime-v1-report",
        "generatedAtMs": now_ms(),
        "lease": lease,
        "telemetry": telemetry or {},
        "cacheEvents": cache_events or [],
        "decisionTable": decision_table or [],
    })


def runtime_v1_smoke() -> dict[str, Any]:
    providers = [
        ProviderProfileV1("llm-2gb", gpu_memory_mb=2048, ram_memory_mb=2048,
                          flops_tflops=2, llm_stage_capacity_mb=2048,
                          max_context_tokens=4096, kv_cache_budget_mb=512),
        ProviderProfileV1("llm-4gb", gpu_memory_mb=4096, ram_memory_mb=4096,
                          flops_tflops=4, llm_stage_capacity_mb=4096,
                          max_context_tokens=8192, kv_cache_budget_mb=1024),
        ProviderProfileV1("llm-8gb", gpu_memory_mb=8192, ram_memory_mb=8192,
                          flops_tflops=8, llm_stage_capacity_mb=8192,
                          max_context_tokens=16384, kv_cache_budget_mb=2048),
    ]
    model = ModelManifestV1(
        model_id="qwen-tiny",
        revision="runtime-v1-smoke",
        layers=28,
        context_window_tokens=16384,
        tokenizer_id="qwen-tokenizer",
        kv_cache_bytes_per_token_per_layer=128,
    )
    manager = LongContextManager(budget_mb=4)
    manager.put(prefix_state(
        object_id="uav-mission-prefix",
        prefix_id="uav-mission-prefix",
        model_id=model.model_id,
        tokenizer_id=model.tokenizer_id,
        provider="llm-8gb",
        token_count=1024,
    ), pin=True)
    smoke_stage = {
        "stageId": "stage-2",
        "role": "/LLM/Stage/2",
        "layerStart": 12,
        "layerEnd": 27,
    }
    exact_cache = ExactForwardCacheManager(budget_mb=2048)
    exact_key = exact_forward_cache_key_for_stage(
        model,
        smoke_stage,
        token_ids=range(1024),
        plan_hash="runtime-v1-smoke-plan",
        split_layout_hash="proportional-2-4-8",
        runtime_backend="minindn-native-tracer",
        dtype="float16",
        quantization="none",
        security_epoch="smoke",
    )
    exact_cache.put(ExactForwardCacheEntry(
        key=exact_key,
        provider="llm-8gb",
        object_name=exact_key.data_name(provider="/llm-8gb"),
        byte_count=int(model.kv_cache_mb(1024, 16) * 1024 * 1024),
        token_count=1024,
    ))
    exact_cache.get(exact_key)
    telemetry = {
        "llm-8gb": RuntimeTelemetryV1(
            provider="llm-8gb",
            kv_cache=KvCacheTelemetry(
                budget_mb=2048,
                used_mb=exact_cache.telemetry().used_mb,
                resident_prefix_ids=("uav-mission-prefix",),
                resident_exact_cache_key_digests=(exact_key.digest(),),
                hits=1,
            ),
        )
    }
    placement = choose_cache_placement(
        providers,
        telemetry,
        prefix_id="uav-mission-prefix",
        required_kv_mb=model.kv_cache_mb(1024, 8),
    )
    return {
        "allocation": proportional_layer_allocation(providers, 28),
        "cachePlacement": to_plain(placement),
        "cacheTelemetry": to_plain(manager.telemetry()),
        "exactForwardCache": {
            "keyDigest": exact_key.digest(),
            "hitRequires": "exact token prefix, model, plan, stage definition, runtime, and security epoch",
            "telemetry": to_plain(exact_cache.telemetry()),
        },
    }


def build_local_llm_plan(model: ModelManifestV1,
                         providers: list[ProviderProfileV1],
                         *,
                         target_rps: float = 0.0,
                         context_class: str = "short",
                         prefix_id: str = "",
                         session_id: str = "") -> PlanLeaseV1:
    allocation = proportional_layer_allocation(providers, model.layers)
    stages: list[dict[str, Any]] = []
    cursor = 0
    for index, provider in enumerate(providers):
        layer_count = allocation.get(provider.provider, 0)
        if layer_count <= 0:
            continue
        stages.append({
            "stageId": f"stage-{len(stages)}",
            "role": f"/LLM/Stage/{len(stages)}",
            "provider": provider.provider,
            "layerStart": cursor,
            "layerEnd": cursor + layer_count - 1,
            "layerCount": layer_count,
        })
        cursor += layer_count
    plan = {
        "plannerMode": "proportional",
        "plannerKind": "llm-pipeline",
        "modelFamily": model.model_family,
        "modelId": model.model_id,
        "modelRevision": model.revision,
        "context": {
            "contextClass": context_class,
            "contextWindowTokens": model.context_window_tokens,
            "tokenizerId": model.tokenizer_id,
            "kvCacheBytesPerTokenPerLayer": model.kv_cache_bytes_per_token_per_layer,
            "supportsPrefill": model.supports_prefill,
            "supportsDecode": model.supports_decode,
            "supportsStreaming": model.supports_streaming,
            "stateObjects": [kind.value for kind in ContextObjectKind],
        },
        "stages": stages,
        "dependencies": [
            {"from": stages[index]["stageId"], "to": stages[index + 1]["stageId"]}
            for index in range(max(0, len(stages) - 1))
        ],
        "shards": [],
        "summary": {"layerAllocation": allocation, "stageCount": len(stages)},
    }
    fallbacks = generate_fallback_plans(plan)
    plan["fallbackPlans"] = [
        {"planId": item.plan_id, "reason": item.reason, "layout": item.layout}
        for item in fallbacks
    ]
    validate_linear_llm_plan(plan)
    telemetry = {
        provider.provider: RuntimeTelemetryV1(
            provider=provider.provider,
            kv_cache=KvCacheTelemetry(
                budget_mb=provider.kv_cache_budget_mb,
                max_context_tokens=provider.max_context_tokens,
            ),
        )
        for provider in providers
    }
    placement = choose_cache_placement(
        providers,
        telemetry,
        prefix_id=prefix_id,
        session_id=session_id,
        required_kv_mb=model.kv_cache_mb(
            min(model.context_window_tokens or 0, 1024),
            max(1, model.layers // max(1, len(stages)))),
    )
    return make_plan_lease(
        plan,
        model=model,
        providers=providers,
        context_class=context_class,
        target_rps=target_rps,
        cache_placement=placement,
    )


def load_provider_profiles(path: str | Path) -> list[ProviderProfileV1]:
    payload = read_json(path)
    providers = payload.get("providers", payload if isinstance(payload, list) else [])
    return [ProviderProfileV1.from_dict(item) for item in providers]


def _cmd_schema_sample(args: argparse.Namespace) -> int:
    payload = runtime_v1_smoke()
    payload["status"] = "contract-smoke"
    if args.out:
        write_json(args.out, payload)
    else:
        print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


def _cmd_provider(args: argparse.Namespace) -> int:
    profile = ProviderProfileV1.from_dict(read_json(args.profile))
    telemetry = RuntimeTelemetryV1(
        provider=profile.provider,
        kv_cache=KvCacheTelemetry(
            budget_mb=profile.kv_cache_budget_mb,
            max_context_tokens=profile.max_context_tokens,
        ),
    )
    service_payload = {
        **profile.to_ack_fields(),
        **telemetry.to_ack_fields(),
    }
    payload = encode_provider_capability_ack(CoreProviderCapabilityHint(
        provider_name=profile.provider,
        service_name="/Inference/RuntimeV1",
        ready=True,
        runtime_hint=GenericProviderRuntimeHint(
            provider_name=profile.provider,
            active_work_count=telemetry.active_workers,
            queue_length=telemetry.aggregate_queue,
            estimated_queue_wait_ms=telemetry.queue_wait_ewma_ms,
            free_memory_mb=telemetry.free_memory_mb,
            capacity_hints=profile.to_ack_fields(),
        ),
        service_payload_schema="ndnsf-di-runtime-v1",
        service_payload=service_payload,
    ))
    if args.out:
        write_json(args.out, {
            "provider": profile.provider,
            "ackPayload": payload.decode("utf-8"),
            "fields": parse_ack_metadata(payload),
        })
    else:
        print(payload.decode("utf-8"))
    return 0


def _cmd_plan(args: argparse.Namespace) -> int:
    model = ModelManifestV1.from_dict(read_json(args.model))
    providers = load_provider_profiles(args.providers)
    lease = build_local_llm_plan(
        model,
        providers,
        target_rps=args.target_rps,
        context_class=args.context_class,
        prefix_id=args.prefix_id,
        session_id=args.session_id,
    )
    write_json(args.out, lease)
    return 0


def _cmd_run(args: argparse.Namespace) -> int:
    payload = read_json(args.plan)
    lease = lease_from_dict(payload) if "plan_key" in payload else None
    layout = lease.layout if lease else payload
    context = layout.get("context", {})
    provider_name = (
        lease.cache_placement.provider
        if lease and lease.cache_placement is not None else
        str((layout.get("stages") or [{}])[-1].get("provider", "runtime-v1-provider"))
    )
    model = ModelManifestV1(
        model_id=str(layout.get("modelId", "runtime-v1-model")),
        revision=str(layout.get("modelRevision", "")),
        layers=sum(int(stage.get("layerCount", 0)) for stage in layout.get("stages", [])),
        context_window_tokens=int(context.get("contextWindowTokens", 0) or 0),
        tokenizer_id=str(context.get("tokenizerId", "")),
        kv_cache_bytes_per_token_per_layer=int(context.get("kvCacheBytesPerTokenPerLayer", 0) or 0),
        supports_streaming=bool(context.get("supportsStreaming", False)),
    )
    provider = ProviderProfileV1(
        provider=provider_name,
        flops_tflops=max(1.0, float(args.provider_flops_tflops)),
        llm_stage_capacity_mb=8192,
        kv_cache_budget_mb=2048,
    )
    generation = simulate_prefill_decode(
        request_id="runtime-v1-run",
        provider=provider,
        model=model,
        prompt_tokens=args.prompt_tokens,
        generated_tokens=args.generated_tokens,
        microbatch=args.microbatch,
    )
    result = {
        "status": "executed-contract-smoke",
        "planId": lease.plan_id if lease else payload.get("planId", ""),
        "requestCount": args.requests,
        "streamingSupported": bool(context.get("supportsStreaming", False)),
        "prefillDecode": to_plain(generation),
        "fallbackPlanIds": list(lease.fallback_plan_ids) if lease else [],
    }
    if args.out:
        write_json(args.out, result)
    else:
        print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def _cmd_bench(args: argparse.Namespace) -> int:
    args.out_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    for index in range(args.runs):
        payload = runtime_v1_smoke()
        row = {
            "run": index,
            "provider": payload["cachePlacement"]["provider"],
            "reason": payload["cachePlacement"]["reason"],
            **payload["allocation"],
        }
        rows.append(row)
    write_json(args.out_dir / "bench-summary.json", {"runs": rows})
    return 0


def _cmd_context_sweep(args: argparse.Namespace) -> int:
    args.out_dir.mkdir(parents=True, exist_ok=True)
    model = ModelManifestV1.from_dict(read_json(args.model))
    providers = load_provider_profiles(args.providers)
    rows = []
    for context_tokens in [int(item) for item in args.context_tokens.split(",") if item]:
        for rps in [float(item) for item in args.rps.split(",") if item]:
            lease = build_local_llm_plan(
                model,
                providers,
                target_rps=rps,
                context_class="long" if context_tokens > 4096 else "short",
                prefix_id="sweep-prefix" if args.cache_aware else "",
            )
            provider = max(providers, key=lambda item: item.flops_tflops)
            generation = simulate_prefill_decode(
                request_id=f"sweep-{context_tokens}-{rps}",
                provider=provider,
                model=model,
                prompt_tokens=context_tokens,
                generated_tokens=args.generated_tokens,
                microbatch=args.microbatch,
            )
            rows.append({
                "contextTokens": context_tokens,
                "targetRps": rps,
                "planId": lease.plan_id,
                "cacheProvider": lease.cache_placement.provider if lease.cache_placement else "",
                "cacheReason": lease.cache_placement.reason if lease.cache_placement else "",
                "timeToFirstTokenMs": generation.time_to_first_token_ms,
                "interTokenMs": generation.inter_token_ms,
                "fallbackPlanCount": len(lease.fallback_plan_ids),
            })
    write_json(args.out_dir / "context-sweep-summary.json", {"rows": rows})
    return 0


def _cmd_inspect(args: argparse.Namespace) -> int:
    payload = read_json(args.path)
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _resolve_command(value: Any, default: list[str]) -> list[str]:
    if value is None:
        return default
    if not isinstance(value, list) or not value or not all(isinstance(v, str) for v in value):
        raise ValueError("deployment adapter command must be a non-empty string array")
    return list(value)


def _emit_or_execute(command: list[str], args: argparse.Namespace,
                     *, identities: dict[str, str] | None = None) -> int:
    payload = {
        "schema": "ndnsf-di-production-adapter-v1",
        "mode": "production-adapter",
        "command": command,
        "identities": identities or {},
    }
    if getattr(args, "dry_run", False):
        print(json.dumps(payload, indent=2, sort_keys=True))
        return 0
    return subprocess.run(command, cwd=_repo_root(), check=False).returncode


def _deployment_payload(path: str | Path) -> tuple[dict[str, Any], dict[str, Any]]:
    payload = read_json(path)
    deployment = payload.get("deployment", payload)
    if not isinstance(deployment, dict):
        raise ValueError("deployment profile must contain an object")
    return payload, deployment


def _cmd_production_provider(args: argparse.Namespace) -> int:
    payload, deployment = _deployment_payload(args.profile)
    default = [str(_repo_root() / "build/examples/di-native-provider"), "--check-only"]
    command = _resolve_command(deployment.get("provider_command"), default)
    return _emit_or_execute(command, args, identities={
        "profile": str(Path(args.profile).resolve()),
        "release": str(deployment.get("release_dir", "")),
        "provider": str(deployment.get("identity", payload.get("provider", {}).get("identity", ""))),
    })


def _deprecated_simulation(command: str) -> int:
    print(
        f"ndnsf-di {command}: the simulated Runtime v1 entrypoint moved to "
        f"'ndnsf-di contract-smoke {command}'; production {command} requires a profile/campaign",
        file=sys.stderr,
    )
    return 2


def _cmd_production_run(args: argparse.Namespace) -> int:
    if not args.profile or not args.request or not args.out:
        return _deprecated_simulation("run")
    _payload, deployment = _deployment_payload(args.profile)
    harness = str(deployment.get(
        "harness", "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"))
    command = _resolve_command(deployment.get("run_command"), [
        sys.executable, harness, "--output-dir", str(Path(args.out).parent),
    ])
    return _emit_or_execute(command, args, identities={
        "profile": str(Path(args.profile).resolve()),
        "plan": str(Path(args.plan).resolve()),
        "request": str(Path(args.request).resolve()),
        "result": str(Path(args.out).resolve()),
    })


def _cmd_production_bench(args: argparse.Namespace) -> int:
    if not args.campaign or not args.out:
        return _deprecated_simulation("bench")
    campaign = read_json(args.campaign)
    runner = str(campaign.get("runner", "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"))
    perf = campaign.get("performance", {})
    command = _resolve_command(campaign.get("command"), [
        sys.executable, runner,
        "--output-dir", str(args.out),
        "--measured-duration-s", str(perf.get("measurementSeconds", 60)),
        "--request-interval-ms", str(1000.0 / float(perf.get("offeredRps", 1.0))),
        "--max-new-tokens", str(campaign.get("maxNewTokens", 32)),
        "--campaign-id", str(campaign.get("campaignId", "operator-bench")),
        "--runner-mode", "qwen-onnx-native",
    ])
    return _emit_or_execute(command, args, identities={
        "campaign": str(Path(args.campaign).resolve()),
        "output": str(Path(args.out).resolve()),
    })


def _cmd_production_doctor(args: argparse.Namespace) -> int:
    command = [sys.executable, str(_repo_root() / "tools/ndnsf_runtime.py"),
               "doctor", "--profile", args.profile]
    if args.json:
        pass
    return _emit_or_execute(command, args, identities={
        "profile": str(Path(args.profile).resolve()),
    })


def _cmd_status(args: argparse.Namespace) -> int:
    _payload, deployment = _deployment_payload(args.profile)
    status_path = Path(str(deployment.get("status_file", "/run/ndnsf-di/status.json")))
    status = read_json(status_path) if status_path.is_file() else {
        "schema": "ndnsf-di-status-v1", "ready": False,
        "reason": "STATUS_FILE_MISSING", "statusFile": str(status_path),
    }
    status["profile"] = str(Path(args.profile).resolve())
    print(json.dumps(status, indent=2, sort_keys=True))
    return 0 if status.get("ready") else 1


def _cmd_metrics(args: argparse.Namespace) -> int:
    from .operations import MetricsSnapshot, atomic_export_metrics
    _payload, deployment = _deployment_payload(args.profile)
    source = Path(str(deployment.get("metrics_file", "/run/ndnsf-di/metrics.json")))
    snapshot = MetricsSnapshot.from_dict(read_json(source) if source.is_file() else {})
    atomic_export_metrics(snapshot, args.out, args.format)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="NDNSF-DI Runtime v1 utilities")
    sub = parser.add_subparsers(dest="command", required=True)

    provider = sub.add_parser("provider", help="run the native provider adapter")
    provider.add_argument("--profile", required=True)
    provider.add_argument("--dry-run", action="store_true")
    provider.set_defaults(func=_cmd_production_provider)

    plan = sub.add_parser("plan", help="build a local Runtime v1 plan lease")
    plan.add_argument("--model", required=True)
    plan.add_argument("--providers", required=True)
    plan.add_argument("--out", required=True)
    plan.add_argument("--target-rps", type=float, default=0.0)
    plan.add_argument("--context-class", default="short")
    plan.add_argument("--prefix-id", default="")
    plan.add_argument("--session-id", default="")
    plan.set_defaults(func=_cmd_plan)

    run = sub.add_parser("run", help="run a real deployment request adapter")
    run.add_argument("--plan", required=True)
    run.add_argument("--profile", default="")
    run.add_argument("--request", default="")
    run.add_argument("--out", default="")
    run.add_argument("--dry-run", action="store_true")
    run.set_defaults(func=_cmd_production_run)

    bench = sub.add_parser("bench", help="run a real MiniNDN campaign adapter")
    bench.add_argument("--campaign", default="")
    bench.add_argument("--out", type=Path)
    bench.add_argument("--dry-run", action="store_true")
    bench.set_defaults(func=_cmd_production_bench)

    status = sub.add_parser("status", help="read the deployment status snapshot")
    status.add_argument("--profile", required=True)
    status.add_argument("--json", action="store_true")
    status.set_defaults(func=_cmd_status)

    metrics = sub.add_parser("metrics", help="export a deployment metrics snapshot")
    metrics.add_argument("--profile", required=True)
    metrics.add_argument("--format", choices=("json", "prometheus-textfile"), default="json")
    metrics.add_argument("--out", required=True)
    metrics.set_defaults(func=_cmd_metrics)

    doctor = sub.add_parser("doctor", help="run production deployment preflight")
    doctor.add_argument("--profile", required=True)
    doctor.add_argument("--json", action="store_true")
    doctor.add_argument("--dry-run", action="store_true")
    doctor.set_defaults(func=_cmd_production_doctor)

    smoke = sub.add_parser("contract-smoke", help="explicit simulated Runtime v1 utilities")
    smoke_sub = smoke.add_subparsers(dest="smoke_command", required=True)

    smoke_run = smoke_sub.add_parser("run")
    smoke_run.add_argument("--plan", required=True)
    smoke_run.add_argument("--requests", type=int, default=1)
    smoke_run.add_argument("--prompt-tokens", type=int, default=1024)
    smoke_run.add_argument("--generated-tokens", type=int, default=32)
    smoke_run.add_argument("--microbatch", type=int, default=1)
    smoke_run.add_argument("--provider-flops-tflops", type=float, default=8.0)
    smoke_run.add_argument("--out", default="")
    smoke_run.set_defaults(func=_cmd_run)

    smoke_bench = smoke_sub.add_parser("bench")
    smoke_bench.add_argument("--out-dir", type=Path, required=True)
    smoke_bench.add_argument("--runs", type=int, default=1)
    smoke_bench.set_defaults(func=_cmd_bench)

    sweep = smoke_sub.add_parser("context-sweep")
    sweep.add_argument("--model", required=True)
    sweep.add_argument("--providers", required=True)
    sweep.add_argument("--out-dir", type=Path, required=True)
    sweep.add_argument("--context-tokens", default="1024,8192")
    sweep.add_argument("--rps", default="1,4,8")
    sweep.add_argument("--generated-tokens", type=int, default=32)
    sweep.add_argument("--microbatch", type=int, default=1)
    sweep.add_argument("--cache-aware", action="store_true")
    sweep.set_defaults(func=_cmd_context_sweep)

    smoke_sample = smoke_sub.add_parser("schema-sample")
    smoke_sample.add_argument("--out", default="")
    smoke_sample.set_defaults(func=_cmd_schema_sample)

    sample = sub.add_parser("schema-sample", help="write a Runtime v1 smoke payload")
    sample.add_argument("--out", default="")
    sample.set_defaults(func=_cmd_schema_sample)

    inspect = sub.add_parser("inspect", help="pretty-print a Runtime v1 JSON file")
    inspect.add_argument("path")
    inspect.set_defaults(func=_cmd_inspect)

    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
