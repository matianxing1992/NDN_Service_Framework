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
import json
import time
import zlib
from dataclasses import asdict, dataclass, field, is_dataclass, replace
from enum import Enum
from pathlib import Path
from typing import Any, Iterable


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


class ExactForwardCacheKind(str, Enum):
    KV_BLOCK = "kv-block"
    HIDDEN_STATE = "hidden-state"
    LOGITS = "logits"


class SemanticCacheDisposition(str, Enum):
    HIT = "hit"
    CANDIDATE = "candidate"
    MISS = "miss"
    DISABLED = "disabled"


class SemanticPatternRank(str, Enum):
    HIGH = "high"
    MID = "mid"
    LOW = "low"
    UNKNOWN = "unknown"


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


@dataclass(frozen=True)
class SemanticServiceCacheKey:
    """Provider-local semantic response cache scope.

    ``semantic_pattern_id`` is produced by the application or embedding layer.
    The runtime helper does not decide whether two prompts are semantically
    similar; it only enforces that semantic reuse stays within one service,
    model/tokenizer, response schema, and policy epoch.
    """

    service_name: str
    model_id: str
    tokenizer_id: str
    policy_epoch: str
    semantic_pattern_id: str
    response_schema: str = ""
    app_namespace: str = ""

    def digest(self) -> str:
        return stable_digest(self, length=24)


@dataclass(frozen=True)
class SemanticServiceCacheEntry:
    key: SemanticServiceCacheKey
    response_payload: bytes = b""
    provider: str = ""
    confidence_threshold: float = 0.88
    estimated_prompt_tokens: int = 0
    estimated_output_tokens: int = 0
    estimated_saved_decode_tokens: int = 0
    byte_count: int = 0
    reuse_likelihood: float = 1.0
    conversation_round: int = 0
    pattern_rank: SemanticPatternRank = SemanticPatternRank.UNKNOWN
    pattern_token_saving_ratio: float = 0.0
    created_at_ms: int = field(default_factory=now_ms)
    last_used_at_ms: int = 0
    expires_at_ms: int = 0
    metadata: dict[str, Any] = field(default_factory=dict)

    @property
    def key_digest(self) -> str:
        return self.key.digest()

    @property
    def cache_benefit_score(self) -> float:
        size_mb = max(self.byte_count, len(self.response_payload), 1) / (1024.0 * 1024.0)
        saved_tokens = max(self.estimated_saved_decode_tokens, self.estimated_output_tokens, 0)
        rank_weight = {
            SemanticPatternRank.HIGH: 1.5,
            SemanticPatternRank.MID: 1.15,
            SemanticPatternRank.LOW: 0.85,
            SemanticPatternRank.UNKNOWN: 1.0,
        }[SemanticPatternRank(self.pattern_rank)]
        saving_weight = 1.0 + max(0.0, min(1.0, self.pattern_token_saving_ratio))
        return (saved_tokens * max(0.0, self.reuse_likelihood) * rank_weight * saving_weight) / size_mb

    def is_valid(self, *, now_ms_value: int | None = None) -> bool:
        current = now_ms() if now_ms_value is None else int(now_ms_value)
        return not self.expires_at_ms or current < self.expires_at_ms

    def with_last_used(self, timestamp_ms: int) -> "SemanticServiceCacheEntry":
        return dataclass_replace(self, last_used_at_ms=timestamp_ms)


def dataclass_replace(value: Any, **changes: Any) -> Any:
    return replace(value, **changes)


@dataclass(frozen=True)
class SemanticCacheAckHint:
    disposition: SemanticCacheDisposition = SemanticCacheDisposition.MISS
    confidence: float = 0.0
    estimated_saved_decode_tokens: int = 0
    policy_epoch: str = ""
    pattern_rank: SemanticPatternRank = SemanticPatternRank.UNKNOWN
    token_saving_ratio: float = 0.0
    enabled: bool = True

    @property
    def confidence_bucket(self) -> str:
        if self.confidence >= 0.9:
            return "high"
        if self.confidence >= 0.75:
            return "medium"
        if self.confidence > 0:
            return "low"
        return "none"

    def to_ack_fields(self) -> dict[str, Any]:
        return semantic_cache_ack_fields(self)


@dataclass(frozen=True)
class SemanticPatternMeta:
    """Application-produced semantic pattern summary for cache policy.

    The pattern identifier stays local. ACKs may expose only coarse rank and
    ratio buckets derived from this metadata.
    """

    pattern_id: str
    conversation_round: int = 0
    query_count: int = 0
    total_prompt_tokens: int = 0
    total_output_tokens: int = 0
    estimated_saved_tokens: int = 0
    proportion_ratio: float = 0.0
    token_saving_ratio: float = 0.0
    rank: SemanticPatternRank = SemanticPatternRank.UNKNOWN

    @property
    def total_tokens(self) -> int:
        return max(0, int(self.total_prompt_tokens) + int(self.total_output_tokens))


def semantic_cache_token_saving_ratio(*, saved_tokens: int, total_tokens: int) -> float:
    if total_tokens <= 0:
        return 0.0
    return max(0.0, min(1.0, float(saved_tokens) / float(total_tokens)))


def classify_semantic_pattern_rank(index: int, total: int) -> SemanticPatternRank:
    if total <= 0:
        return SemanticPatternRank.UNKNOWN
    high_count = max(1, (total + 3) // 4)
    mid_count = max(high_count + 1, (total + 1) // 2)
    low_count = max(mid_count + 1, (3 * total + 3) // 4)
    if int(index) < high_count:
        return SemanticPatternRank.HIGH
    if int(index) < mid_count:
        return SemanticPatternRank.MID
    if int(index) < low_count:
        return SemanticPatternRank.LOW
    return SemanticPatternRank.UNKNOWN


def rank_semantic_patterns(patterns: Iterable[SemanticPatternMeta]) -> list[SemanticPatternMeta]:
    ordered = sorted(
        patterns,
        key=lambda pattern: (
            pattern.token_saving_ratio,
            pattern.estimated_saved_tokens,
            pattern.query_count,
        ),
        reverse=True,
    )
    total = len(ordered)
    return [
        dataclass_replace(pattern, rank=classify_semantic_pattern_rank(index, total))
        for index, pattern in enumerate(ordered)
    ]


class SemanticServiceCacheManager:
    def __init__(self, *, budget_mb: float = 0.0, min_admission_score: float = 1.0):
        self.budget_mb = float(budget_mb)
        self.min_admission_score = float(min_admission_score)
        self._entries: dict[str, SemanticServiceCacheEntry] = {}
        self._hits = 0
        self._misses = 0
        self._evictions = 0
        self._admissions = 0
        self._rejections = 0
        self._patterns: dict[str, SemanticPatternMeta] = {}

    @property
    def used_mb(self) -> float:
        total = 0
        for entry in self._entries.values():
            total += max(entry.byte_count, len(entry.response_payload))
        return total / (1024.0 * 1024.0)

    def put(self, entry: SemanticServiceCacheEntry, *, force: bool = False) -> bool:
        if not force and entry.cache_benefit_score < self._effective_min_admission_score():
            self._rejections += 1
            return False
        self._entries[entry.key_digest] = entry
        self._admissions += 1
        self._evict_if_needed()
        return True

    def register_patterns(self, patterns: Iterable[SemanticPatternMeta]) -> None:
        for pattern in rank_semantic_patterns(patterns):
            self._patterns[pattern.pattern_id] = pattern

    def pattern_meta(self, pattern_id: str) -> SemanticPatternMeta | None:
        return self._patterns.get(pattern_id)

    def entry_from_pattern(
        self,
        *,
        key: SemanticServiceCacheKey,
        response_payload: bytes = b"",
        provider: str = "",
        confidence_threshold: float = 0.88,
        estimated_prompt_tokens: int = 0,
        estimated_output_tokens: int = 0,
        byte_count: int = 0,
        reuse_likelihood: float = 1.0,
    ) -> SemanticServiceCacheEntry:
        meta = self.pattern_meta(key.semantic_pattern_id)
        saved_tokens = (
            meta.estimated_saved_tokens
            if meta is not None and meta.estimated_saved_tokens else
            estimated_output_tokens
        )
        return SemanticServiceCacheEntry(
            key=key,
            response_payload=response_payload,
            provider=provider,
            confidence_threshold=confidence_threshold,
            estimated_prompt_tokens=estimated_prompt_tokens,
            estimated_output_tokens=estimated_output_tokens,
            estimated_saved_decode_tokens=saved_tokens,
            byte_count=byte_count,
            reuse_likelihood=reuse_likelihood,
            conversation_round=0 if meta is None else meta.conversation_round,
            pattern_rank=SemanticPatternRank.UNKNOWN if meta is None else meta.rank,
            pattern_token_saving_ratio=0.0 if meta is None else meta.token_saving_ratio,
        )

    def get(self, key: SemanticServiceCacheKey, *, confidence: float,
            min_confidence: float | None = None,
            now_ms_value: int | None = None) -> SemanticServiceCacheEntry | None:
        digest = key.digest()
        entry = self._entries.get(digest)
        threshold = entry.confidence_threshold if min_confidence is None and entry else min_confidence
        threshold = 0.0 if threshold is None else float(threshold)
        if (
            entry is None or entry.key != key or
            not entry.is_valid(now_ms_value=now_ms_value) or
            float(confidence) < threshold
        ):
            if entry is not None and not entry.is_valid(now_ms_value=now_ms_value):
                self._entries.pop(digest, None)
                self._evictions += 1
            self._misses += 1
            return None
        timestamp = now_ms() if now_ms_value is None else int(now_ms_value)
        entry = entry.with_last_used(timestamp)
        self._entries[digest] = entry
        self._hits += 1
        return entry

    def hint_for(self, key: SemanticServiceCacheKey, *, confidence: float,
                 min_confidence: float | None = None,
                 now_ms_value: int | None = None,
                 policy_epoch: str | None = None) -> SemanticCacheAckHint:
        entry = self._entries.get(key.digest())
        if entry is None or entry.key != key or not entry.is_valid(now_ms_value=now_ms_value):
            return SemanticCacheAckHint(
                disposition=SemanticCacheDisposition.MISS,
                confidence=confidence,
                policy_epoch=policy_epoch if policy_epoch is not None else key.policy_epoch,
            )
        threshold = entry.confidence_threshold if min_confidence is None else float(min_confidence)
        disposition = (
            SemanticCacheDisposition.HIT
            if float(confidence) >= threshold else
            SemanticCacheDisposition.CANDIDATE
        )
        return SemanticCacheAckHint(
            disposition=disposition,
            confidence=confidence,
            estimated_saved_decode_tokens=entry.estimated_saved_decode_tokens,
            policy_epoch=policy_epoch if policy_epoch is not None else key.policy_epoch,
            pattern_rank=SemanticPatternRank(entry.pattern_rank),
            token_saving_ratio=entry.pattern_token_saving_ratio,
        )

    def telemetry(self) -> dict[str, Any]:
        return {
            "budgetMb": self.budget_mb,
            "usedMb": self.used_mb,
            "entries": len(self._entries),
            "hits": self._hits,
            "misses": self._misses,
            "evictions": self._evictions,
            "admissions": self._admissions,
            "rejections": self._rejections,
            "patternCount": len(self._patterns),
            "highRankPatterns": sum(
                1 for pattern in self._patterns.values()
                if pattern.rank == SemanticPatternRank.HIGH
            ),
            "midRankPatterns": sum(
                1 for pattern in self._patterns.values()
                if pattern.rank == SemanticPatternRank.MID
            ),
            "lowRankPatterns": sum(
                1 for pattern in self._patterns.values()
                if pattern.rank == SemanticPatternRank.LOW
            ),
            "estimatedSavedTokens": sum(
                entry.estimated_saved_decode_tokens
                for entry in self._entries.values()
            ),
        }

    def _effective_min_admission_score(self) -> float:
        if self.budget_mb <= 0:
            return self.min_admission_score
        pressure = min(1.0, self.used_mb / max(self.budget_mb, 0.000001))
        return self.min_admission_score * (1.0 + pressure)

    def _evict_if_needed(self) -> None:
        if self.budget_mb <= 0:
            return
        while self.used_mb > self.budget_mb and self._entries:
            victim = min(
                self._entries,
                key=lambda digest: (
                    self._entries[digest].cache_benefit_score,
                    self._entries[digest].last_used_at_ms or self._entries[digest].created_at_ms,
                ),
            )
            self._entries.pop(victim, None)
            self._evictions += 1


def semantic_cache_ack_fields(hint: SemanticCacheAckHint) -> dict[str, Any]:
    return {
        "semanticCache": hint.disposition.value,
        "semanticCacheEnabled": hint.enabled,
        "semanticCacheConfidenceBucket": hint.confidence_bucket,
        "semanticCacheEstimatedSavedTokens": max(0, int(hint.estimated_saved_decode_tokens)),
        "semanticCachePolicyEpoch": hint.policy_epoch,
        "semanticCachePatternRank": SemanticPatternRank(hint.pattern_rank).value,
        "semanticCacheTokenSavingRatioBucket": _ratio_bucket(hint.token_saving_ratio),
    }


def _ratio_bucket(value: float) -> str:
    if value >= 0.5:
        return "high"
    if value >= 0.2:
        return "medium"
    if value > 0:
        return "low"
    return "none"


def parse_semantic_cache_ack_hint(fields: dict[str, Any]) -> SemanticCacheAckHint:
    disposition = str(fields.get("semanticCache", SemanticCacheDisposition.MISS.value))
    try:
        parsed_disposition = SemanticCacheDisposition(disposition)
    except ValueError:
        parsed_disposition = SemanticCacheDisposition.MISS
    bucket = str(fields.get("semanticCacheConfidenceBucket", "none"))
    confidence = {
        "high": 0.9,
        "medium": 0.75,
        "low": 0.5,
        "none": 0.0,
    }.get(bucket, 0.0)
    rank_text = str(fields.get("semanticCachePatternRank", SemanticPatternRank.UNKNOWN.value))
    try:
        rank = SemanticPatternRank(rank_text)
    except ValueError:
        rank = SemanticPatternRank.UNKNOWN
    ratio_bucket = str(fields.get("semanticCacheTokenSavingRatioBucket", "none"))
    token_saving_ratio = {
        "high": 0.5,
        "medium": 0.2,
        "low": 0.01,
        "none": 0.0,
    }.get(ratio_bucket, 0.0)
    return SemanticCacheAckHint(
        disposition=parsed_disposition,
        confidence=confidence,
        estimated_saved_decode_tokens=int(fields.get("semanticCacheEstimatedSavedTokens", 0) or 0),
        policy_epoch=str(fields.get("semanticCachePolicyEpoch", "")),
        pattern_rank=rank,
        token_saving_ratio=token_saving_ratio,
        enabled=str(fields.get("semanticCacheEnabled", "1")) not in {"0", "false", "False"},
    )


def choose_semantic_cache_provider(candidates: dict[str, dict[str, Any]]) -> str:
    """Select the provider with the strongest coarse semantic-cache ACK hint."""

    def score(item: tuple[str, dict[str, Any]]) -> tuple[int, int, str]:
        provider, fields = item
        hint = parse_semantic_cache_ack_hint(fields)
        disposition_rank = {
            SemanticCacheDisposition.HIT: 3,
            SemanticCacheDisposition.CANDIDATE: 2,
            SemanticCacheDisposition.MISS: 1,
            SemanticCacheDisposition.DISABLED: 0,
        }[hint.disposition]
        rank_bonus = {
            SemanticPatternRank.HIGH: 3,
            SemanticPatternRank.MID: 2,
            SemanticPatternRank.LOW: 1,
            SemanticPatternRank.UNKNOWN: 0,
        }[hint.pattern_rank]
        return (disposition_rank, rank_bonus, hint.estimated_saved_decode_tokens, provider)

    if not candidates:
        return ""
    return max(candidates.items(), key=score)[0]


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

    def is_valid(self, *, now_ms_value: int | None = None,
                 versions: dict[str, str] | None = None) -> bool:
        current = now_ms() if now_ms_value is None else int(now_ms_value)
        if self.expires_at_ms and current >= self.expires_at_ms:
            return False
        for key, value in (versions or {}).items():
            expected = self.valid_until_versions.get(key)
            if expected is not None and expected != value:
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

    def get(self, key: PlanKeyV1, *, versions: dict[str, str] | None = None) -> PlanLeaseV1 | None:
        lease = self._leases.get(key.digest())
        if lease is None:
            return None
        return lease if lease.is_valid(versions=versions) else None

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
                    ttl_ms: int = 0) -> PlanLeaseV1:
    provider_set = tuple(sorted(provider.provider for provider in providers))
    capability_version = stable_digest([to_plain(provider) for provider in providers], length=16)
    cache_version = stable_digest(cache_placement, length=16) if cache_placement else ""
    key = PlanKeyV1(
        model_id=model.model_id,
        model_revision=model.revision,
        context_class=context_class,
        target_rps=target_rps,
        provider_set=provider_set,
        capability_version=capability_version,
        cache_version=cache_version,
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
            **({"cache": cache_version} if cache_version else {}),
        },
    )


def lease_from_dict(payload: dict[str, Any]) -> PlanLeaseV1:
    key_payload = dict(payload["plan_key"])
    placement_payload = payload.get("cache_placement")
    placement = CachePlacementDecision(**placement_payload) if placement_payload else None
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
    payload = encode_ack_metadata({
        **profile.to_ack_fields(),
        **telemetry.to_ack_fields(),
    })
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


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="NDNSF-DI Runtime v1 utilities")
    sub = parser.add_subparsers(dest="command", required=True)

    provider = sub.add_parser("provider", help="emit provider Runtime v1 ACK metadata")
    provider.add_argument("--profile", required=True)
    provider.add_argument("--out", default="")
    provider.set_defaults(func=_cmd_provider)

    plan = sub.add_parser("plan", help="build a local Runtime v1 plan lease")
    plan.add_argument("--model", required=True)
    plan.add_argument("--providers", required=True)
    plan.add_argument("--out", required=True)
    plan.add_argument("--target-rps", type=float, default=0.0)
    plan.add_argument("--context-class", default="short")
    plan.add_argument("--prefix-id", default="")
    plan.add_argument("--session-id", default="")
    plan.set_defaults(func=_cmd_plan)

    run = sub.add_parser("run", help="run a local Runtime v1 contract smoke")
    run.add_argument("--plan", required=True)
    run.add_argument("--requests", type=int, default=1)
    run.add_argument("--prompt-tokens", type=int, default=1024)
    run.add_argument("--generated-tokens", type=int, default=32)
    run.add_argument("--microbatch", type=int, default=1)
    run.add_argument("--provider-flops-tflops", type=float, default=8.0)
    run.add_argument("--out", default="")
    run.set_defaults(func=_cmd_run)

    bench = sub.add_parser("bench", help="run a local Runtime v1 smoke benchmark")
    bench.add_argument("--out-dir", type=Path, required=True)
    bench.add_argument("--runs", type=int, default=1)
    bench.set_defaults(func=_cmd_bench)

    sweep = sub.add_parser("context-sweep", help="run local RPS/context-length sweep")
    sweep.add_argument("--model", required=True)
    sweep.add_argument("--providers", required=True)
    sweep.add_argument("--out-dir", type=Path, required=True)
    sweep.add_argument("--context-tokens", default="1024,8192")
    sweep.add_argument("--rps", default="1,4,8")
    sweep.add_argument("--generated-tokens", type=int, default=32)
    sweep.add_argument("--microbatch", type=int, default=1)
    sweep.add_argument("--cache-aware", action="store_true")
    sweep.set_defaults(func=_cmd_context_sweep)

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
