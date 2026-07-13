#!/usr/bin/env python3
"""Deterministic Spec 110 workload, sampling, token, and metric helpers."""

from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import math
from typing import Any, Iterable, Mapping


PERCENTILE_REQUIREMENTS = {"p50": 20, "p95": 100, "p99": 1000}
PERCENTILES = {"p50": 0.50, "p95": 0.95, "p99": 0.99}


class WorkloadError(ValueError):
    pass


def _fail(code: str, detail: str = "") -> None:
    raise WorkloadError(code + (f":{detail}" if detail else ""))


def validate_workload(value: Mapping[str, object]) -> dict[str, Any]:
    if value.get("schemaVersion") != "spec110-workload-v1":
        _fail("WORKLOAD_SCHEMA_INVALID")
    prompt = value.get("prompt")
    if not isinstance(prompt, Mapping) or not isinstance(prompt.get("text"), str):
        _fail("WORKLOAD_PROMPT_INVALID")
    actual = hashlib.sha256(prompt["text"].encode()).hexdigest()
    if prompt.get("sha256") != actual:
        _fail("WORKLOAD_PROMPT_DIGEST_MISMATCH")
    tokenization = value.get("tokenization")
    if not isinstance(tokenization, Mapping) or tokenization.get("retainExactInputTokenIds") is not True:
        _fail("WORKLOAD_INPUT_TOKENS_NOT_RETAINED")
    decode = value.get("decode")
    if (
        not isinstance(decode, Mapping)
        or decode.get("strategy") != "greedy"
        or decode.get("doSample") is not False
        or decode.get("correctnessOutputTokenCounts") != [1, 2, 32]
        or decode.get("retainExactOutputTokenIds") is not True
    ):
        _fail("WORKLOAD_DECODE_INVALID")
    performance = value.get("performance")
    if (
        not isinstance(performance, Mapping)
        or performance.get("warmupExcluded") is not True
        or performance.get("measuredWindowSeconds") != 60
        or performance.get("candidateRepetitions") != 3
        or performance.get("matchedBaselineRepetitions") != 3
    ):
        _fail("WORKLOAD_PERFORMANCE_INVALID")
    metrics = value.get("metrics")
    if not isinstance(metrics, Mapping) or metrics.get("percentileMinimumObservations") != PERCENTILE_REQUIREMENTS:
        _fail("WORKLOAD_PERCENTILE_THRESHOLDS_INVALID")
    return dict(value)


def exact_token_record(
    input_token_ids: Iterable[int], oracle_token_ids: Iterable[int], output_token_ids: Iterable[int]
) -> dict[str, Any]:
    inputs = list(input_token_ids)
    oracle = list(oracle_token_ids)
    output = list(output_token_ids)
    if not inputs or not oracle or any(not isinstance(item, int) or isinstance(item, bool) for item in inputs + oracle + output):
        _fail("WORKLOAD_TOKEN_IDS_INVALID")
    return {
        "inputTokenIds": inputs,
        "oracleTokenIds": oracle,
        "outputTokenIds": output,
        "exactMatch": output == oracle,
        "inputDigest": "sha256:" + hashlib.sha256(",".join(map(str, inputs)).encode()).hexdigest(),
        "oracleDigest": "sha256:" + hashlib.sha256(",".join(map(str, oracle)).encode()).hexdigest(),
        "outputDigest": "sha256:" + hashlib.sha256(",".join(map(str, output)).encode()).hexdigest(),
    }


def deterministic_sample(identity: str, sample_rate: float, *, seed: str = "spec110") -> bool:
    if not isinstance(identity, str) or not identity:
        _fail("WORKLOAD_SAMPLE_ID_INVALID")
    if not isinstance(sample_rate, (int, float)) or isinstance(sample_rate, bool) or not 0 <= sample_rate <= 1:
        _fail("WORKLOAD_SAMPLE_RATE_INVALID")
    number = int.from_bytes(hashlib.sha256((seed + "\0" + identity).encode()).digest()[:8], "big")
    return number < int(sample_rate * (1 << 64))


@dataclass
class MeasurementWindow:
    warmup_requests: int
    duration_seconds: float = 60.0
    warmup_seen: int = 0
    start_time: float | None = None
    last_time: float | None = None
    samples: list[tuple[float, float]] = field(default_factory=list)

    def __post_init__(self) -> None:
        if not isinstance(self.warmup_requests, int) or isinstance(self.warmup_requests, bool) or self.warmup_requests < 0:
            _fail("WORKLOAD_WARMUP_INVALID")
        if not isinstance(self.duration_seconds, (int, float)) or self.duration_seconds <= 0:
            _fail("WORKLOAD_WINDOW_INVALID")

    def record(self, observed_at_seconds: float, value: float) -> str:
        if self.last_time is not None and observed_at_seconds < self.last_time:
            _fail("WORKLOAD_TIME_NOT_MONOTONIC")
        self.last_time = observed_at_seconds
        if self.warmup_seen < self.warmup_requests:
            self.warmup_seen += 1
            return "WARMUP_EXCLUDED"
        if self.start_time is None:
            self.start_time = observed_at_seconds
        if observed_at_seconds >= self.start_time + self.duration_seconds:
            return "OUTSIDE_MEASURED_WINDOW"
        self.samples.append((observed_at_seconds, float(value)))
        return "MEASURED"

    def report(self) -> dict[str, Any]:
        return {
            "warmupRequests": self.warmup_seen,
            "warmupExcluded": True,
            "startTimeSeconds": self.start_time,
            "endTimeSeconds": None if self.start_time is None else self.start_time + self.duration_seconds,
            "durationSeconds": self.duration_seconds,
            "sampleCount": len(self.samples),
            "samples": [value for _, value in self.samples],
        }


def percentile(samples: Iterable[float], name: str) -> dict[str, Any]:
    if name not in PERCENTILE_REQUIREMENTS:
        _fail("WORKLOAD_PERCENTILE_NAME_INVALID", name)
    values = sorted(float(item) for item in samples)
    required = PERCENTILE_REQUIREMENTS[name]
    if len(values) < required:
        return {
            "status": "UNAVAILABLE_INSUFFICIENT_SAMPLES",
            "value": None,
            "observations": len(values),
            "requiredObservations": required,
        }
    rank = max(0, math.ceil(PERCENTILES[name] * len(values)) - 1)
    return {
        "status": "AVAILABLE",
        "value": values[rank],
        "observations": len(values),
        "requiredObservations": required,
    }


def metric_summary(samples: Iterable[float]) -> dict[str, Any]:
    values = list(samples)
    return {"count": len(values), **{name: percentile(values, name) for name in PERCENTILE_REQUIREMENTS}}


__all__ = [
    "MeasurementWindow", "PERCENTILE_REQUIREMENTS", "WorkloadError",
    "deterministic_sample", "exact_token_record", "metric_summary", "percentile",
    "validate_workload",
]
