"""Typed bounded execution-tuning defaults."""

from __future__ import annotations

from ..core.ports import PolicyRequest, PolicyResult, TuningProposal
from .defaults import _result


FAMILY_TO_PARAMETER = {
    "transfer_chunk": "transfer_chunk_bytes",
    "prefetch": "prefetch_depth",
    "microbatch": "microbatch_size",
    "compression": "compression_level",
    "overlap": "overlap",
    "speculative_decoding": "speculative_window",
}


class BoundedExecutionTuningPolicy:
    def propose(self, request: PolicyRequest) -> PolicyResult:
        families = tuple(str(item) for item in request.metadata.get(
            "declared_families", ("microbatch",)))
        values = dict(request.metadata.get("parameters", {"microbatch_size": 1.0}))
        declared_parameters = {FAMILY_TO_PARAMETER[item] for item in families
                               if item in FAMILY_TO_PARAMETER}
        if set(values) - declared_parameters:
            raise ValueError("undeclared execution tuning parameter")
        value = TuningProposal(values, families)
        return _result("execution_tuning", request, value, self.__class__.__name__)


__all__ = ["BoundedExecutionTuningPolicy", "FAMILY_TO_PARAMETER"]
