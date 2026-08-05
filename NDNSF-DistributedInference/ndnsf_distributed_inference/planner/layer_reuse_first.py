"""Spec 170 default placement policy.

The legacy ``PreSplitFirstStrategy`` remains available only to callers that
explicitly request its V2 profile.  This policy keeps the existing V2 planner
implementation as a compatibility base while exposing the V3 decision surface
used by the new coordinator: exact loaded/disk artifacts first, then accepted
preparation, with no mutation of Provider state in the strategy.
"""

from __future__ import annotations

from dataclasses import dataclass
from dataclasses import replace
import hashlib
from typing import Mapping

from ..sdk.placement import (
    ExecutionDisposition, PlacementProposalV3, ProviderPlanningViewV3,
    RoleAssemblySpec,
)
from .presplit_first import PreSplitFirstStrategy


class LayerReuseFirstStrategy(PreSplitFirstStrategy):
    name = "layer-reuse-first"
    version = "3"
    # The default request-first path is now V3.  V2 remains available only when
    # an application explicitly supplies PreSplitFirstStrategy or another
    # strategy whose placement_profile is DI_PLACEMENT_V2.
    placement_profile = "DI_PLACEMENT_V3"
    state_digest = "sha256:" + hashlib.sha256(
        b"ndnsf-di-layer-reuse-first-v3").hexdigest()

    def __init__(
        self,
        *,
        at_ms: int = 1,
        security_domain: str = "",
        maximum_cache_age_ms: int = 30_000,
        clock_ms=None,
    ) -> None:
        """Construct the V3 strategy without a legacy clock requirement.

        The compatibility base owns the V2 planner's cache fields, but V3
        proposals are computed solely from the immutable ACK_CLOSED snapshot.
        A positive sentinel keeps the inherited constructor valid while the
        default application path no longer requires callers to manufacture a
        wall-clock timestamp.
        """
        super().__init__(
            at_ms=at_ms,
            security_domain=security_domain,
            maximum_cache_age_ms=maximum_cache_age_ms,
            clock_ms=clock_ms,
        )

    def propose_v3(
        self,
        *,
        request_id: str,
        attempt: int,
        model_digest: str,
        graph_digest: str,
        roles: tuple[RoleAssemblySpec, ...],
        providers: tuple[ProviderPlanningViewV3, ...],
        ack_closed_digest: str,
    ) -> PlacementProposalV3:
        """Return a data-only V3 proposal from an ACK_CLOSED snapshot.

        Provider selection is deterministic and residency-aware.  Exact
        assembled/loaded proof wins; preparation is used only when no exact
        proof exists and the Provider explicitly accepted preparation.
        """

        if not providers:
            raise ValueError("V3 strategy requires Provider offers")
        views = {item.provider: item for item in providers}
        assignments: dict[str, str] = {}
        selected_roles: list[RoleAssemblySpec] = []
        role_counts = {item.role: sum(1 for candidate in roles
                                      if candidate.role == item.role)
                       for item in roles}
        for role in sorted(roles, key=lambda item: (item.role, item.rank)):
            ranked = []
            for view in providers:
                if role.role not in view.accepted_roles:
                    continue
                # A role spec starts with the adapter's logical backend
                # (for example ``transformers``).  Providers advertise the
                # concrete execution backend as an additional capability
                # (``transformers-cpu`` or ``cuda``).  Keep the logical name
                # as the compatibility match, then bind the concrete name
                # below so CPU readiness evidence cannot be mislabeled as a
                # generic backend.
                compatible_backends = set(view.backends)
                if (role.backend not in compatible_backends
                        and f"{role.backend}-cpu" not in compatible_backends):
                    continue
                selected_backend = role.backend
                exact = any(
                    proof.role == role.role and proof.rank == role.rank
                    and proof.artifact_digest == role.artifact_digest
                    for proof in view.residency)
                if (view.execution_disposition
                        == ExecutionDisposition.ACCEPT_IF_EXACT_REUSE
                        and not exact):
                    continue
                if (not exact and view.execution_disposition
                        != ExecutionDisposition.ACCEPT_WITH_PREPARATION):
                    continue
                tier = 0 if exact else 1
                available_devices = tuple(view.topology.devices)
                if not available_devices and role.backend in {
                        "cpu", "transformers-cpu", "onnxruntime-cpu"}:
                    available_devices = ("cpu",)
                if role.device_set:
                    available_devices = tuple(
                        item for item in available_devices
                        if item in role.device_set)
                if not available_devices:
                    continue
                selected_device = min(available_devices)
                if selected_device == "cpu" and (
                        f"{role.backend}-cpu" in compatible_backends):
                    selected_backend = f"{role.backend}-cpu"
                elif (selected_device.startswith("cuda:")
                      and role.backend not in compatible_backends
                      and "cuda" in compatible_backends):
                    selected_backend = "cuda"
                ranked.append((
                    tier, view.queue_depth, view.estimated_wait_ms,
                    view.rtt_ms, -view.bandwidth_mbps, view.provider,
                    selected_device, selected_backend))
            if not ranked:
                raise ValueError(f"no feasible Provider for V3 role {role.role}")
            key = (role.role if role_counts[role.role] == 1
                   else f"{role.role}#{role.rank}")
            selected = min(ranked)
            assignments[key] = selected[5]
            selected_roles.append(replace(
                role, backend=selected[7], device_set=(selected[6],)))
        return PlacementProposalV3(
            request_id=request_id, attempt=attempt,
            model_digest=model_digest, graph_digest=graph_digest,
            roles=tuple(selected_roles), provider_by_role=assignments,
            strategy_name=self.name, strategy_version=self.version,
            strategy_state_digest=self.state_digest,
        )


__all__ = ["LayerReuseFirstStrategy"]
