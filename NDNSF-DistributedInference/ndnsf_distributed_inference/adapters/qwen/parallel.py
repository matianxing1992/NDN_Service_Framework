"""Qwen hybrid parallel-plan sealing.

Qwen adapters own tensor-layout compatibility and therefore must supply each
redistribution certificate explicitly.  This module only joins those adapter
certificates to the generic deterministic hybrid-plan sealer; it never infers
GATHER, SCATTER, or RESHARD from rank counts alone.
"""

from __future__ import annotations

from ...core.hybrid_contracts import HybridPlan, RedistributionEdge
from ...splitter import seal_hybrid_plan


def seal_qwen_hybrid_plan(
    *,
    tensor_degrees: tuple[int, ...] | list[int],
    redistributions: tuple[RedistributionEdge, ...] | list[RedistributionEdge] = (),
) -> HybridPlan:
    """Seal one Qwen hybrid plan from adapter-certified transitions."""

    if any(not edge.tensor.startswith("activation-")
           for edge in redistributions):
        raise ValueError("Qwen redistribution must name its activation boundary")
    return seal_hybrid_plan(
        tensor_degrees=tensor_degrees,
        redistributions=redistributions,
    )


__all__ = ["seal_qwen_hybrid_plan"]
