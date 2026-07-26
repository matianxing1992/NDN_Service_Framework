from __future__ import annotations

from ndnsf_distributed_inference.core.ports import (
    CandidateBudget, EngineSnapshot, MetricValue, OptimizationObjective,
    PolicyRequest, PolicyState,
)


def objective() -> OptimizationObjective:
    return OptimizationObjective(
        hard_constraints={"latency": MetricValue(100.0, "ms", "max")},
        weights={"latency": 1.0}, normalization={"latency": 100.0})


def snapshot(epoch: int = 1, captured_at_ms: int = 1_000) -> EngineSnapshot:
    return EngineSnapshot(
        f"snapshot-{epoch}", epoch, captured_at_ms,
        {"latency": MetricValue(10.0, "ms", "p95")}, {},
        PolicyState(epoch, f"sha256:state-{epoch}"))


def request(candidates=(), *, epoch: int = 1, scope: str = "", metadata=None,
            max_candidates: int = 16) -> PolicyRequest:
    return PolicyRequest(objective(), snapshot(epoch),
                         CandidateBudget(max_candidates), tuple(candidates),
                         scope, dict(metadata or {}))
