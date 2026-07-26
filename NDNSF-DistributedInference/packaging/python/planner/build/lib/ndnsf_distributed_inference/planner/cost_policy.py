"""Stable cost model used by the migrated runtime-aware policy."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class CostPolicyWeights:
    compute: float = 1.0
    residency_ready: float = 1.0
    queue_wait: float = 1.0
    queue_depth_ms: float = 5.0
    confidence_penalty_ms: float = 50.0
    conservative_fallback_ms: float = 10_000.0


DEFAULT_COST_POLICY_VERSION = "runtime-v1-cost-parity-v1"
DEFAULT_COST_POLICY_WEIGHTS = CostPolicyWeights()


def score_cost(*, compute_ms: float, residency_ready_ms: float,
               queue_wait_ms: float, queue_length: int, confidence: float,
               weights: CostPolicyWeights = DEFAULT_COST_POLICY_WEIGHTS) -> float:
    return (
        weights.compute * max(0.0, compute_ms)
        + weights.residency_ready * max(0.0, residency_ready_ms)
        + weights.queue_wait * max(0.0, queue_wait_ms)
        + weights.queue_depth_ms * max(0, queue_length)
        + weights.confidence_penalty_ms
        * (1.0 - min(1.0, max(0.0, confidence)))
    )


__all__ = ["CostPolicyWeights", "DEFAULT_COST_POLICY_VERSION",
           "DEFAULT_COST_POLICY_WEIGHTS", "score_cost"]
