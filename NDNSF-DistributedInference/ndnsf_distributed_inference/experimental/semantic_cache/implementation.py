"""Application-owned semantic response cache experiment."""

from __future__ import annotations

from dataclasses import dataclass, field, replace
from enum import Enum
from typing import Any, Iterable

from ...runtime_v1 import now_ms, stable_digest


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


@dataclass(frozen=True)
class SemanticServiceCacheKey:
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
        saved = max(self.estimated_saved_decode_tokens, self.estimated_output_tokens, 0)
        rank_weight = {
            SemanticPatternRank.HIGH: 1.5,
            SemanticPatternRank.MID: 1.15,
            SemanticPatternRank.LOW: 0.85,
            SemanticPatternRank.UNKNOWN: 1.0,
        }[SemanticPatternRank(self.pattern_rank)]
        saving_weight = 1.0 + max(0.0, min(1.0, self.pattern_token_saving_ratio))
        return saved * max(0.0, self.reuse_likelihood) * rank_weight * saving_weight / size_mb

    def is_valid(self, *, now_ms_value: int | None = None) -> bool:
        current = now_ms() if now_ms_value is None else int(now_ms_value)
        return not self.expires_at_ms or current < self.expires_at_ms

    def with_last_used(self, timestamp_ms: int) -> "SemanticServiceCacheEntry":
        return replace(self, last_used_at_ms=timestamp_ms)


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


def _rank(index: int, total: int) -> SemanticPatternRank:
    if total <= 0:
        return SemanticPatternRank.UNKNOWN
    high = max(1, (total + 3) // 4)
    mid = max(high + 1, (total + 1) // 2)
    low = max(mid + 1, (3 * total + 3) // 4)
    if index < high:
        return SemanticPatternRank.HIGH
    if index < mid:
        return SemanticPatternRank.MID
    if index < low:
        return SemanticPatternRank.LOW
    return SemanticPatternRank.UNKNOWN


def rank_semantic_patterns(patterns: Iterable[SemanticPatternMeta]) -> list[SemanticPatternMeta]:
    ordered = sorted(patterns, key=lambda item: (
        item.token_saving_ratio, item.estimated_saved_tokens, item.query_count), reverse=True)
    return [replace(item, rank=_rank(index, len(ordered)))
            for index, item in enumerate(ordered)]


class SemanticServiceCacheManager:
    def __init__(self, *, budget_mb: float = 0.0, min_admission_score: float = 1.0):
        self.budget_mb = float(budget_mb)
        self.min_admission_score = float(min_admission_score)
        self._entries: dict[str, SemanticServiceCacheEntry] = {}
        self._patterns: dict[str, SemanticPatternMeta] = {}
        self._hits = self._misses = self._evictions = 0
        self._admissions = self._rejections = 0

    @property
    def used_mb(self) -> float:
        return sum(max(item.byte_count, len(item.response_payload))
                   for item in self._entries.values()) / (1024.0 * 1024.0)

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

    def entry_from_pattern(self, *, key: SemanticServiceCacheKey,
                           response_payload: bytes = b"", provider: str = "",
                           confidence_threshold: float = 0.88,
                           estimated_prompt_tokens: int = 0,
                           estimated_output_tokens: int = 0,
                           byte_count: int = 0,
                           reuse_likelihood: float = 1.0) -> SemanticServiceCacheEntry:
        meta = self.pattern_meta(key.semantic_pattern_id)
        return SemanticServiceCacheEntry(
            key=key, response_payload=response_payload, provider=provider,
            confidence_threshold=confidence_threshold,
            estimated_prompt_tokens=estimated_prompt_tokens,
            estimated_output_tokens=estimated_output_tokens,
            estimated_saved_decode_tokens=(
                meta.estimated_saved_tokens
                if meta is not None and meta.estimated_saved_tokens
                else estimated_output_tokens),
            byte_count=byte_count, reuse_likelihood=reuse_likelihood,
            conversation_round=meta.conversation_round if meta else 0,
            pattern_rank=meta.rank if meta else SemanticPatternRank.UNKNOWN,
            pattern_token_saving_ratio=meta.token_saving_ratio if meta else 0.0)

    def get(self, key: SemanticServiceCacheKey, *, confidence: float,
            min_confidence: float | None = None,
            now_ms_value: int | None = None) -> SemanticServiceCacheEntry | None:
        digest = key.digest()
        entry = self._entries.get(digest)
        threshold = (entry.confidence_threshold if entry and min_confidence is None
                     else float(min_confidence or 0.0))
        if (entry is None or entry.key != key or
                not entry.is_valid(now_ms_value=now_ms_value) or confidence < threshold):
            if entry is not None and not entry.is_valid(now_ms_value=now_ms_value):
                self._entries.pop(digest, None)
                self._evictions += 1
            self._misses += 1
            return None
        entry = replace(entry, last_used_at_ms=(
            now_ms() if now_ms_value is None else int(now_ms_value)))
        self._entries[digest] = entry
        self._hits += 1
        return entry

    def hint_for(self, key: SemanticServiceCacheKey, *, confidence: float,
                 min_confidence: float | None = None,
                 now_ms_value: int | None = None,
                 policy_epoch: str | None = None) -> SemanticCacheAckHint:
        entry = self._entries.get(key.digest())
        epoch = key.policy_epoch if policy_epoch is None else policy_epoch
        if entry is None or entry.key != key or not entry.is_valid(now_ms_value=now_ms_value):
            return SemanticCacheAckHint(confidence=confidence, policy_epoch=epoch)
        threshold = entry.confidence_threshold if min_confidence is None else min_confidence
        return SemanticCacheAckHint(
            disposition=(SemanticCacheDisposition.HIT if confidence >= threshold
                         else SemanticCacheDisposition.CANDIDATE),
            confidence=confidence,
            estimated_saved_decode_tokens=entry.estimated_saved_decode_tokens,
            policy_epoch=epoch,
            pattern_rank=SemanticPatternRank(entry.pattern_rank),
            token_saving_ratio=entry.pattern_token_saving_ratio)

    def telemetry(self) -> dict[str, Any]:
        ranks = [item.rank for item in self._patterns.values()]
        return {
            "budgetMb": self.budget_mb, "usedMb": self.used_mb,
            "entries": len(self._entries), "hits": self._hits,
            "misses": self._misses, "evictions": self._evictions,
            "admissions": self._admissions, "rejections": self._rejections,
            "patternCount": len(self._patterns),
            "highRankPatterns": ranks.count(SemanticPatternRank.HIGH),
            "midRankPatterns": ranks.count(SemanticPatternRank.MID),
            "lowRankPatterns": ranks.count(SemanticPatternRank.LOW),
            "estimatedSavedTokens": sum(
                item.estimated_saved_decode_tokens for item in self._entries.values()),
        }

    def _effective_min_admission_score(self) -> float:
        if self.budget_mb <= 0:
            return self.min_admission_score
        return self.min_admission_score * (
            1.0 + min(1.0, self.used_mb / max(self.budget_mb, 0.000001)))

    def _evict_if_needed(self) -> None:
        while self.budget_mb > 0 and self.used_mb > self.budget_mb and self._entries:
            victim = min(self._entries, key=lambda digest: (
                self._entries[digest].cache_benefit_score,
                self._entries[digest].last_used_at_ms or self._entries[digest].created_at_ms))
            self._entries.pop(victim, None)
            self._evictions += 1


def _ratio_bucket(value: float) -> str:
    return "high" if value >= 0.5 else "medium" if value >= 0.2 else \
        "low" if value > 0 else "none"


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


def parse_semantic_cache_ack_hint(fields: dict[str, Any]) -> SemanticCacheAckHint:
    try:
        disposition = SemanticCacheDisposition(str(fields.get("semanticCache", "miss")))
    except ValueError:
        disposition = SemanticCacheDisposition.MISS
    try:
        rank = SemanticPatternRank(str(fields.get("semanticCachePatternRank", "unknown")))
    except ValueError:
        rank = SemanticPatternRank.UNKNOWN
    return SemanticCacheAckHint(
        disposition=disposition,
        confidence={"high": 0.9, "medium": 0.75, "low": 0.5}.get(
            str(fields.get("semanticCacheConfidenceBucket", "none")), 0.0),
        estimated_saved_decode_tokens=int(fields.get("semanticCacheEstimatedSavedTokens", 0) or 0),
        policy_epoch=str(fields.get("semanticCachePolicyEpoch", "")),
        pattern_rank=rank,
        token_saving_ratio={"high": 0.5, "medium": 0.2, "low": 0.01}.get(
            str(fields.get("semanticCacheTokenSavingRatioBucket", "none")), 0.0),
        enabled=str(fields.get("semanticCacheEnabled", "1")) not in {"0", "false", "False"})


def choose_semantic_cache_provider(candidates: dict[str, dict[str, Any]]) -> str:
    def score(item: tuple[str, dict[str, Any]]) -> tuple[int, int, int, str]:
        provider, fields = item
        hint = parse_semantic_cache_ack_hint(fields)
        dispositions = {SemanticCacheDisposition.HIT: 3, SemanticCacheDisposition.CANDIDATE: 2,
                        SemanticCacheDisposition.MISS: 1, SemanticCacheDisposition.DISABLED: 0}
        ranks = {SemanticPatternRank.HIGH: 3, SemanticPatternRank.MID: 2,
                 SemanticPatternRank.LOW: 1, SemanticPatternRank.UNKNOWN: 0}
        return dispositions[hint.disposition], ranks[hint.pattern_rank], \
            hint.estimated_saved_decode_tokens, provider
    return max(candidates.items(), key=score)[0] if candidates else ""
