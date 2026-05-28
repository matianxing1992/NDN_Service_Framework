"""Controller facade for NDNSF-DistributedInference examples and apps."""

from __future__ import annotations

import threading

from ndnsf import ServiceController


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
    ) -> "DistributedInferenceController":
        return cls(ServiceController(
            controller_prefix=controller_prefix,
            policy_file=policy_file,
            trust_schema=trust_schema,
            bootstrap_identities=list(bootstrap_identities or []),
            serve_certificates=serve_certificates,
        ))

    def start(self) -> None:
        self._controller.start()

    def run(self) -> int:
        return self._controller.run()

    def stop(self) -> int:
        return self._controller.stop()

    def start_background(self) -> threading.Thread:
        return self._controller.start_background()
