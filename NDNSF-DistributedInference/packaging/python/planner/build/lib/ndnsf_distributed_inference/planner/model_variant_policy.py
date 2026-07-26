"""Reference model-variant policies."""

from __future__ import annotations

from ..core.ports import ModelCandidate, PolicyRequest, PolicyResult
from .defaults import _result


class ExactOrCompatibleModelVariantPolicy:
    """Prefer exact semantics, then preserve deterministic input order."""

    def propose(self, request: PolicyRequest) -> PolicyResult:
        candidates = tuple(item for item in request.candidates
                           if isinstance(item, ModelCandidate))
        if len(candidates) > request.budget.max_candidates:
            raise ValueError("candidate budget exceeded")
        exact = tuple(item for item in candidates if item.exact_semantics)
        allowed = exact or candidates
        if not allowed:
            raise ValueError("no compatible model variant")
        return _result("model_variant", request, allowed[0], self.__class__.__name__)


__all__ = ["ExactOrCompatibleModelVariantPolicy"]
