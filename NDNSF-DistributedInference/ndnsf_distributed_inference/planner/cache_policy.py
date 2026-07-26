"""Epoch-bound cache action policy; affinity is advisory only."""

from __future__ import annotations

from ..core.ports import CacheAction, CacheProposal, PolicyRequest, PolicyResult
from .defaults import _result


class EpochBoundCachePolicy:
    version = "cache-parity-v1"

    def propose(self, request: PolicyRequest) -> PolicyResult:
        action = request.metadata.get("action", CacheAction.LOOKUP_OR_REUSE.value)
        value = CacheProposal(
            CacheAction(action), str(request.metadata["cache_key_digest"]),
            dict(request.metadata.get("provider_affinity", {})),
            request.snapshot.epoch)
        return _result("cache", request, value, self.__class__.__name__)


__all__ = ["EpochBoundCachePolicy"]
