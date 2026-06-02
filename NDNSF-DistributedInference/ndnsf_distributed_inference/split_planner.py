"""Generic split-planning helpers for NDNSF-DI.

The planner does not assume ONNX as the only model format. ONNX graph analysis
is one way to produce candidate cut points, but any model-specific splitter or
optimizer can feed equivalent candidate metadata into this layer.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Sequence


@dataclass(frozen=True)
class ProviderProfile:
    """Coarse provider capability used while ranking split candidates."""

    name: str
    compute_score: float = 1.0
    uplink_mbps: float = 1000.0
    downlink_mbps: float = 1000.0
    rtt_ms: float = 0.0
    memory_mb: float = 0.0

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> "ProviderProfile":
        return cls(
            name=str(value["name"]),
            compute_score=float(value.get("compute_score", 1.0)),
            uplink_mbps=float(value.get("uplink_mbps", 1000.0)),
            downlink_mbps=float(value.get("downlink_mbps", 1000.0)),
            rtt_ms=float(value.get("rtt_ms", 0.0)),
            memory_mb=float(value.get("memory_mb", 0.0)),
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "computeScore": self.compute_score,
            "uplinkMbps": self.uplink_mbps,
            "downlinkMbps": self.downlink_mbps,
            "rttMs": self.rtt_ms,
            "memoryMb": self.memory_mb,
        }


def homogeneous_provider_profiles(
    names: Sequence[str],
    *,
    compute_score: float = 1.0,
    uplink_mbps: float = 200.0,
    downlink_mbps: float = 200.0,
    rtt_ms: float = 20.0,
    memory_mb: float = 0.0,
) -> list[ProviderProfile]:
    """Build default provider profiles when all providers are equivalent.

    This is the current default for automatic split planning. It keeps
    provider scheduling compatible with today's examples while leaving a single
    interface where future runtime profiling can supply measured CPU/GPU,
    memory, model latency, bandwidth, and RTT values.
    """

    return [
        ProviderProfile(
            name=str(name),
            compute_score=compute_score,
            uplink_mbps=uplink_mbps,
            downlink_mbps=downlink_mbps,
            rtt_ms=rtt_ms,
            memory_mb=memory_mb,
        )
        for name in names
    ]


@dataclass(frozen=True)
class SplitPlannerWeights:
    transfer_ms: float = 1.0
    compute_imbalance: float = 100.0
    activation_mb: float = 1.0
    unknown_tensor: float = 20.0


@dataclass(frozen=True)
class SequentialSplitCandidate:
    """Model-format-independent sequential split candidate."""

    cut_after_node: int
    boundary_tensors: tuple[str, ...]
    known_boundary_bytes: int
    unknown_size_tensors: tuple[str, ...] = ()

    @classmethod
    def from_onnx_candidate(cls, candidate: Any) -> "SequentialSplitCandidate":
        return cls(
            cut_after_node=int(candidate.cut_after_node),
            boundary_tensors=tuple(candidate.boundary_tensors),
            known_boundary_bytes=int(candidate.known_boundary_bytes),
            unknown_size_tensors=tuple(candidate.unknown_size_tensors),
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "cutAfterNode": self.cut_after_node,
            "boundaryTensors": list(self.boundary_tensors),
            "knownBoundaryBytes": self.known_boundary_bytes,
            "unknownSizeTensors": list(self.unknown_size_tensors),
        }


@dataclass(frozen=True)
class SplitPlanRecommendation:
    candidate: SequentialSplitCandidate
    left_provider: ProviderProfile
    right_provider: ProviderProfile
    left_nodes: int
    right_nodes: int
    transfer_ms: float
    left_compute_units: float
    right_compute_units: float
    compute_imbalance: float
    activation_mb: float
    score: float

    def to_dict(self) -> dict[str, Any]:
        return {
            "candidate": self.candidate.to_dict(),
            "leftProvider": self.left_provider.to_dict(),
            "rightProvider": self.right_provider.to_dict(),
            "leftNodes": self.left_nodes,
            "rightNodes": self.right_nodes,
            "transferMs": self.transfer_ms,
            "leftComputeUnits": self.left_compute_units,
            "rightComputeUnits": self.right_compute_units,
            "computeImbalance": self.compute_imbalance,
            "activationMb": self.activation_mb,
            "score": self.score,
        }


def recommend_sequential_splits(
    candidates: Sequence[SequentialSplitCandidate],
    *,
    total_nodes: int,
    providers: Sequence[ProviderProfile],
    max_recommendations: int = 10,
    weights: SplitPlannerWeights = SplitPlannerWeights(),
) -> list[SplitPlanRecommendation]:
    """Rank two-stage sequential split candidates against provider profiles."""

    if total_nodes <= 1:
        return []
    if not providers:
        providers = (ProviderProfile(name="local"),)

    recommendations: list[SplitPlanRecommendation] = []
    for candidate in candidates:
        left_nodes = max(1, min(total_nodes - 1, candidate.cut_after_node + 1))
        right_nodes = max(1, total_nodes - left_nodes)
        for left in providers:
            for right in providers:
                if len(providers) > 1 and left.name == right.name:
                    continue
                recommendations.append(_score_candidate(
                    candidate,
                    left=left,
                    right=right,
                    left_nodes=left_nodes,
                    right_nodes=right_nodes,
                    weights=weights,
                ))

    return sorted(recommendations, key=lambda item: item.score)[:max_recommendations]


def _score_candidate(
    candidate: SequentialSplitCandidate,
    *,
    left: ProviderProfile,
    right: ProviderProfile,
    left_nodes: int,
    right_nodes: int,
    weights: SplitPlannerWeights,
) -> SplitPlanRecommendation:
    left_compute = left_nodes / max(left.compute_score, 0.001)
    right_compute = right_nodes / max(right.compute_score, 0.001)
    compute_imbalance = (
        abs(left_compute - right_compute) /
        max(left_compute, right_compute, 0.001)
    )
    bottleneck_mbps = max(0.001, min(left.uplink_mbps, right.downlink_mbps))
    activation_mb = candidate.known_boundary_bytes / (1024.0 * 1024.0)
    transfer_ms = max(left.rtt_ms, right.rtt_ms) + (
        candidate.known_boundary_bytes * 8.0 / (bottleneck_mbps * 1000.0)
    )
    score = (
        weights.transfer_ms * transfer_ms +
        weights.compute_imbalance * compute_imbalance +
        weights.activation_mb * activation_mb +
        weights.unknown_tensor * len(candidate.unknown_size_tensors)
    )
    return SplitPlanRecommendation(
        candidate=candidate,
        left_provider=left,
        right_provider=right,
        left_nodes=left_nodes,
        right_nodes=right_nodes,
        transfer_ms=transfer_ms,
        left_compute_units=left_compute,
        right_compute_units=right_compute,
        compute_imbalance=compute_imbalance,
        activation_mb=activation_mb,
        score=score,
    )
