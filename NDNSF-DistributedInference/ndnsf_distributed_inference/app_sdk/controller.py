"""Controller facade for NDNSF-DistributedInference examples and apps."""

from __future__ import annotations

from pathlib import Path
import threading
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from ndnsf import ServiceController
    from ..policy import DistributedInferenceDeployment
else:
    ServiceController = Any
    DistributedInferenceDeployment = Any


class DistributedInferenceController:
    """Run the NDNSF controller role without exposing NDNSF Core classes.

    The controller still uses the generic NDNSF permission and certificate
    machinery internally. This facade exists so AI applications can stay within
    the distributed-inference API surface.
    """

    def __init__(self, controller: ServiceController):
        self._controller = controller

    @classmethod
    def create(
        cls,
        *,
        controller_prefix: str,
        policy_file: str,
        trust_schema: str,
        bootstrap_identities: list[str] | None = None,
        serve_certificates: bool = True,
        bootstrap_token_file: str = "",
    ) -> "DistributedInferenceController":
        # Keep importing the application SDK independent from the optional
        # NDNSF network runtime.  A real controller construction is the point
        # where that runtime becomes mandatory.
        from ndnsf import ServiceController as RuntimeServiceController

        return cls(RuntimeServiceController(
            controller_prefix=controller_prefix,
            policy_file=policy_file,
            trust_schema=trust_schema,
            bootstrap_identities=list(bootstrap_identities or []),
            serve_certificates=serve_certificates,
            bootstrap_token_file=bootstrap_token_file,
        ))

    def start(self) -> None:
        self._controller.start()

    def run(self) -> int:
        return self._controller.run()

    def stop(self) -> int:
        return self._controller.stop()

    def start_background(self) -> threading.Thread:
        return self._controller.start_background()


class APPController:
    """Application-owned controller facade for one deployment definition."""

    def __init__(self, deployment: DistributedInferenceDeployment,
                 controller: DistributedInferenceController):
        self.deployment = deployment
        self._controller = controller

    @classmethod
    def from_config(
        cls,
        config: str | Path,
        *,
        generated_policy_dir: str | Path = "/tmp/ndnsf-di-policy",
        bootstrap_token_file: str = "",
    ) -> "APPController":
        # Deployment parsing also reaches the optional NDNSF runtime through
        # plan wire types, so defer it until a controller is actually created.
        from ..policy import load_or_generate_deployment

        deployment = load_or_generate_deployment(config, generated_policy_dir)
        controller = DistributedInferenceController.create(
            controller_prefix=deployment.controller,
            policy_file=deployment.policy_file,
            trust_schema=deployment.trust_schema,
            bootstrap_identities=deployment.bootstrap_identities,
            bootstrap_token_file=bootstrap_token_file,
        )
        return cls(deployment, controller)

    def run(self) -> int:
        return self._controller.run()

    def stop(self) -> int:
        return self._controller.stop()


__all__ = ["APPController", "DistributedInferenceController"]
