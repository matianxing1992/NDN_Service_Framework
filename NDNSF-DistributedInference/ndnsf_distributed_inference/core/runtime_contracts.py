"""Optional-dependency-free execution evidence and feasibility contracts.

These contracts used to live in ``runtime_v1`` beside CLI, subprocess and
application orchestration code.  Core owns their validation and serialization
semantics; ``runtime_v1`` remains a compatibility re-export.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field, is_dataclass
from enum import Enum
import time
from typing import Any, Iterable


def _now_ms() -> int:
    return int(time.time() * 1000)


def _string_tuple(value: Any) -> tuple[str, ...]:
    if value is None:
        return ()
    if isinstance(value, str):
        return tuple(item.strip() for item in value.split(",") if item.strip())
    return tuple(str(item) for item in value)


def _plain(value: Any) -> Any:
    if isinstance(value, Enum):
        return value.value
    if is_dataclass(value):
        return {key: _plain(item) for key, item in asdict(value).items()}
    if isinstance(value, dict):
        return {str(key): _plain(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_plain(item) for item in value]
    return value


@dataclass(frozen=True)
class ProviderCapabilityV3:
    provider_name: str
    supported_runner_kinds: tuple[str, ...]
    total_gpu_memory_mb: int = 0
    source: str = "profile"


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
                "maxContextTokens", payload.get("max_context_tokens", 0)) or 0),
            resident_prefix_ids=_string_tuple(payload.get(
                "residentPrefixIds", payload.get("resident_prefix_ids", ()))),
            resident_session_ids=_string_tuple(payload.get(
                "residentSessionIds", payload.get("resident_session_ids", ()))),
            resident_exact_cache_key_digests=_string_tuple(payload.get(
                "residentExactCacheKeyDigests",
                payload.get("resident_exact_cache_key_digests", ()))),
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
            provider=provider, node=str(payload.get("node", "")),
            gpu_memory_mb=gpu, ram_memory_mb=ram,
            flops_tflops=float(payload.get(
                "flopsTflops", payload.get("flops_tflops", 0)) or 0),
            llm_stage_capacity_mb=float(payload.get(
                "llmStageCapacityMb", payload.get("llm_stage_capacity_mb", gpu)) or 0),
            llm_max_stage_layers=int(payload.get(
                "llmMaxStageLayers", payload.get("llm_max_stage_layers", 0)) or 0),
            max_workers=max(1, int(payload.get(
                "maxWorkers", payload.get("max_workers", 1)) or 1)),
            supported_backends=_string_tuple(payload.get(
                "supportedBackends", payload.get(
                    "backends", payload.get("supported_backends", ())))),
            model_families=_string_tuple(payload.get(
                "modelFamilies", payload.get("model_families", ("llm",)))),
            max_context_tokens=int(payload.get(
                "maxContextTokens", payload.get("max_context_tokens", 0)) or 0),
            kv_cache_budget_mb=float(payload.get(
                "kvCacheBudgetMb", payload.get("kv_cache_budget_mb", 0)) or 0),
            model_cache=_string_tuple(payload.get(
                "modelCache", payload.get("model_cache", ()))),
            version=str(payload.get("version", "")),
        )

    def effective_capacity_weight(self, max_memory: float, max_flops: float) -> float:
        memory_ratio = self.llm_stage_capacity_mb / max(max_memory, 0.001)
        compute_ratio = self.flops_tflops / max(max_flops, 0.001)
        return max(0.0, min(memory_ratio, compute_ratio))

    def to_ack_fields(self) -> dict[str, Any]:
        return {
            "schema": "ndnsf-di-runtime-v1", "providerProfile": _plain(self),
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
    timestamp_ms: int = field(default_factory=_now_ms)
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
        kv_cache = (kv_payload if isinstance(kv_payload, KvCacheTelemetry)
                    else KvCacheTelemetry.from_dict(dict(kv_payload or {})))
        return cls(
            provider=str(payload.get("provider", "")),
            timestamp_ms=int(payload.get(
                "timestampMs", payload.get("timestamp_ms", _now_ms()))),
            ready_queue=int(payload.get(
                "readyQueue", payload.get("ready_queue", 0)) or 0),
            waiting_dependencies=int(payload.get(
                "waitingDependencies", payload.get("waiting_dependencies", 0)) or 0),
            active_workers=int(payload.get(
                "activeWorkers", payload.get("active_workers", 0)) or 0),
            free_memory_mb=float(payload.get(
                "freeMemoryMb", payload.get("free_memory_mb", 0)) or 0),
            model_loaded=bool(payload.get(
                "modelLoaded", payload.get("model_loaded", False))),
            runtime_backend=str(payload.get(
                "runtimeBackend", payload.get("runtime_backend", ""))),
            service_time_ewma_ms=float(payload.get(
                "serviceTimeEwmaMs", payload.get("service_time_ewma_ms", 0)) or 0),
            queue_wait_ewma_ms=float(payload.get(
                "queueWaitEwmaMs", payload.get("queue_wait_ewma_ms", 0)) or 0),
            kv_cache=kv_cache,
            network_rtt_ms=dict(payload.get(
                "networkRttMs", payload.get("network_rtt_ms", {}))),
            network_bandwidth_mbps=dict(payload.get(
                "networkBandwidthMbps", payload.get("network_bandwidth_mbps", {}))),
        )

    @property
    def aggregate_queue(self) -> int:
        return max(0, self.ready_queue + self.waiting_dependencies + self.active_workers)

    def to_ack_fields(self) -> dict[str, Any]:
        return {
            "providerTelemetry": _plain(self), "queue": self.aggregate_queue,
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
    created_at_ms: int = field(default_factory=_now_ms)
    schema: str = "ndnsf-di-execution-evidence-v1"

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "ExecutionEvidenceV1":
        forbidden = {"key", "privateKey", "token", "userToken", "providerToken",
                     "prompt", "payload", "tensor", "kvPayload"}
        leaked = forbidden.intersection(payload)
        if leaked:
            raise ValueError(
                f"execution evidence contains forbidden fields: {sorted(leaked)}")
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
            artifact_digests={
                str(k): str(v)
                for k, v in dict(payload.get("artifactDigests", {})).items()
            },
            roles=tuple(str(item) for item in payload.get("roles", [])),
            created_at_ms=int(payload.get("createdAtMs", 0)),
        )
        value.validate()
        return value

    def validate(self) -> None:
        required = (self.provider_name, self.provider_boot_id,
                    self.runtime_version, self.model_digest, self.plan_digest,
                    self.device_kind)
        if (not all(required) or not self.roles or not self.artifact_digests
                or self.created_at_ms <= 0):
            raise ValueError("execution evidence missing required field")
        if self.real_compute != (self.runner_kind in REAL_RUNNER_KINDS):
            raise ValueError(
                "execution evidence real-compute classification mismatch")
        if self.runner_kind == RunnerKind.ONNXRUNTIME_CUDA and not self.device_id:
            raise ValueError("CUDA evidence requires a device id")


def classify_execution_evidence(items: Iterable[ExecutionEvidenceV1]) -> str:
    evidence = tuple(items)
    if not evidence:
        return "invalid-evidence"
    for item in evidence:
        item.validate()
    identity = {
        (item.real_compute, item.runner_kind, item.runtime_version,
         item.model_digest, item.plan_digest, item.device_kind)
        for item in evidence
    }
    if len(identity) != 1:
        return "invalid-evidence"
    observed_artifacts: dict[str, str] = {}
    for item in evidence:
        for role, digest in item.artifact_digests.items():
            existing = observed_artifacts.get(role)
            if existing is not None and existing != digest:
                return "invalid-evidence"
            observed_artifacts[role] = digest
    return evidence[0].runner_kind.value


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
    def from_service_payload(
        cls, payload: dict[str, Any]
    ) -> "MeasuredTelemetrySnapshotV1":
        measured = payload.get("measuredTelemetry")
        if not isinstance(measured, dict):
            raise ValueError("measured telemetry section is missing")
        if measured.get("schema") != "ndnsf-di-measured-telemetry-v1":
            raise ValueError("measured telemetry schema is unsupported")
        evidence_payload = payload.get("executionEvidence")
        if not isinstance(evidence_payload, dict):
            raise ValueError("measured telemetry execution evidence is missing")
        evidence = ExecutionEvidenceV1.from_dict(evidence_payload)

        provider_name = str(measured.get("providerName", ""))
        provider_boot_id = str(measured.get("providerBootId", ""))
        source = str(measured.get("source", ""))
        status = str(measured.get("status", ""))
        if source in {"", "configured", "profile", "unavailable"}:
            raise ValueError("configured or unavailable telemetry is not measured")
        if (provider_name != evidence.provider_name
                or provider_boot_id != evidence.provider_boot_id):
            raise ValueError(
                "telemetry and execution evidence identity mismatch")

        def nonnegative_int(name: str, *, required_positive: bool = False) -> int:
            value = measured.get(name, 0)
            if isinstance(value, bool):
                raise ValueError(f"invalid measured telemetry integer: {name}")
            try:
                parsed = int(value)
            except (TypeError, ValueError) as exc:
                raise ValueError(
                    f"invalid measured telemetry integer: {name}") from exc
            if parsed < 0 or (required_positive and parsed <= 0):
                raise ValueError(f"invalid measured telemetry integer: {name}")
            return parsed

        sequence = nonnegative_int(
            "sequence", required_positive=status == "measured")
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
                raise ValueError(
                    f"invalid measured telemetry number: {name}") from exc
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
            stage_service_time_ewma_ms=nonnegative_float(
                "stageServiceTimeEwmaMs"),
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
            name=name, status="PASS" if passed else "FAIL",
            observed=observed, limit=limit))
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
        requirements.maximum_telemetry_age_ms, "TELEMETRY_STALE", "defer")

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
              telemetry.artifact_digests, requirements.expected_artifact_digests,
              "ARTIFACT_IDENTITY_MISMATCH", "reject")
    if requirements.minimum_free_host_memory_bytes > 0:
        check("free-host-memory", telemetry.host_available_memory_bytes >=
              requirements.minimum_free_host_memory_bytes,
              telemetry.host_available_memory_bytes,
              requirements.minimum_free_host_memory_bytes,
              "HOST_MEMORY_PRESSURE", "defer")
    if requirements.maximum_ready_queue > 0:
        check("ready-queue", telemetry.ready_queue <= requirements.maximum_ready_queue,
              telemetry.ready_queue, requirements.maximum_ready_queue,
              "READY_QUEUE_PRESSURE", "defer")
    if requirements.maximum_waiting_dependencies > 0:
        check("waiting-dependencies", telemetry.waiting_dependencies <=
              requirements.maximum_waiting_dependencies,
              telemetry.waiting_dependencies,
              requirements.maximum_waiting_dependencies,
              "DEPENDENCY_QUEUE_PRESSURE", "defer")
    if requirements.maximum_active_workers > 0:
        check("active-workers", telemetry.active_workers <=
              requirements.maximum_active_workers,
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


__all__ = [name for name in globals() if not name.startswith("_")]
