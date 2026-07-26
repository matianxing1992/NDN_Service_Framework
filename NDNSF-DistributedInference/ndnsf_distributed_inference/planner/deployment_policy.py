"""Idempotent deployment lifecycle proposals; no autonomous scaler."""

from __future__ import annotations

from ..core.ports import DeploymentProposal, PolicyRequest, PolicyResult
from .defaults import _result


SAFE_ACTIONS = frozenset({"use-existing", "prewarm", "scale", "drain", "unload"})


class LifecycleDeploymentPolicy:
    def propose(self, request: PolicyRequest) -> PolicyResult:
        action = str(request.metadata.get("action", "use-existing"))
        if action not in SAFE_ACTIONS:
            raise ValueError("unsupported deployment lifecycle action")
        if action in {"drain", "unload"} and request.metadata.get("active_binding"):
            raise ValueError("active binding prevents drain/unload")
        if action == "scale" and not request.metadata.get("cooldown_elapsed", True):
            raise ValueError("deployment cooldown is active")
        value = DeploymentProposal(
            action, str(request.metadata.get("revision", "current")),
            int(request.metadata.get("lifecycle_epoch", 1)),
            str(request.metadata.get("provider", "local")),
            str(request.metadata.get("idempotency_key", "deployment-default")))
        return _result("deployment", request, value, self.__class__.__name__)


__all__ = ["LifecycleDeploymentPolicy", "SAFE_ACTIONS"]
