"""ACK-driven V3 PreSplitFirst policy and isolated V2 compatibility policy."""

from __future__ import annotations

from dataclasses import dataclass, replace
from enum import IntEnum
import hashlib
import json
import math
from typing import Any, Mapping

from ..sdk.placement import (
    ArtifactPreparationMode, ExecutionDisposition, ModelPlacementStrategy,
    PlacementDecision, PlacementProposalV3, PlacementRequest,
    ProviderAssignment, ProviderPlanningViewV3, RoleAssemblySpec,
    ResidencyClassV3, ResidencyTierV3, canonical_digest, is_cpu_backend,
)
from ..splitter import SplitCandidate, SplitSource


class ResidencyTier(IntEnum):
    PINNED_GPU = 0
    RELOAD_SAFE_GPU = 1
    HOST_RAM = 2
    DISK = 3
    REPOSITORY = 4
    NEW_MATERIALIZATION = 5


@dataclass(frozen=True)
class ReusableStateView:
    state_digest: str
    state_class: str
    model_content_digest: str
    semantics_digest: str
    security_domain: str
    layer_begin: int
    layer_end: int
    boot_epoch: str
    cache_epoch: str
    captured_at_ms: int
    expires_at_ms: int
    pin_until_ms: int
    estimated_saved_ms: float

    def __post_init__(self) -> None:
        if (not self.state_digest.startswith("sha256:")
                or len(self.state_digest) != 71
                or not self.state_class or not self.security_domain
                or self.layer_begin < 0 or self.layer_end <= self.layer_begin
                or len(self.boot_epoch) < 8 or not self.cache_epoch
                or self.captured_at_ms <= 0
                or self.expires_at_ms <= self.captured_at_ms
                or self.pin_until_ms < self.captured_at_ms
                or not math.isfinite(self.estimated_saved_ms)
                or self.estimated_saved_ms < 0):
            raise ValueError("invalid reusable derived-state view")


@dataclass(frozen=True)
class SplitSpecification:
    candidate_digest: str
    graph_digest: str
    source: str
    node_roles: tuple[tuple[str, str], ...]
    provider_capacity_mb: tuple[tuple[str, int], ...]

    @property
    def digest(self) -> str:
        return canonical_digest(self)


def _cache_fields(value: Any) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        return {}
    return value


class PreassembledPartitionV2Strategy(ModelPlacementStrategy):
    """Explicit legacy V2 preassembled-partition compatibility strategy."""

    name = "preassembled-partition-v2"
    version = "2"
    state_digest = "sha256:" + hashlib.sha256(
        b"ndnsf-di-pre-split-first-v2").hexdigest()
    # This profile is never selected implicitly by the normal application path.
    placement_profile = "DI_PLACEMENT_V2"
    deterministic = True

    def __init__(
        self,
        *,
        at_ms: int,
        security_domain: str = "",
        maximum_cache_age_ms: int = 30_000,
        clock_ms=None,
    ) -> None:
        if at_ms <= 0 or maximum_cache_age_ms < 0:
            raise ValueError("invalid pre-split-first clock policy")
        self.at_ms = int(at_ms)
        self.security_domain = str(security_domain)
        self.maximum_cache_age_ms = int(maximum_cache_age_ms)
        self._clock_ms = clock_ms

    def plan(self, request: PlacementRequest) -> PlacementDecision:
        if request.model is None or request.graph is None or not request.candidates:
            raise ValueError(
                "pre-split-first requires model, graph, and candidate snapshots")
        candidates = tuple(
            candidate for candidate in request.candidates
            if isinstance(candidate, SplitCandidate)
        )
        if len(candidates) != len(request.candidates):
            raise TypeError("placement candidate is not SplitCandidate")
        for candidate in candidates:
            candidate.validate_against(request.graph)

        catalog_ids = self._matching_catalog_candidate_ids(request)
        ordered = sorted(
            candidates,
            key=lambda item: (
                0 if item.candidate_digest in catalog_ids else 1,
                0 if item.source is SplitSource.PRE_SPLIT else 1,
                item.candidate_digest,
            ),
        )
        failures = []
        for candidate in ordered:
            try:
                return self._place_candidate(
                    request, candidate,
                    pre_split=(candidate.candidate_digest in catalog_ids),
                )
            except ValueError as exc:
                failures.append(
                    f"{candidate.candidate_digest}:{exc}")
        raise ValueError(
            "no capacity-safe split/provider placement: " + ";".join(failures))

    def _matching_catalog_candidate_ids(
        self, request: PlacementRequest,
    ) -> set[str]:
        model = request.model
        candidates_by_digest = {
            candidate.candidate_digest: candidate
            for candidate in request.candidates
            if isinstance(candidate, SplitCandidate)
        }
        matches = set()
        for entry in request.catalog_snapshot or ():
            if getattr(entry, "status", "") != "ACTIVE":
                continue
            candidate_digest = getattr(entry, "candidate_digest", "")
            candidate = candidates_by_digest.get(candidate_digest)
            catalog_backend = getattr(entry, "backend", "")
            if candidate is None or not all(
                catalog_backend in candidate.requirements_by_role[role].backends
                for role in candidate.execution_plan.roles
            ):
                continue
            if (
                getattr(entry, "model_content_digest", "")
                    == model.content_digest
                and getattr(entry, "semantics_digest", "")
                    == model.semantics_digest
                and getattr(entry, "graph_digest", "")
                    == request.graph_digest
                and getattr(entry, "precision", "") == model.precision
            ):
                matches.add(candidate_digest)
        return matches

    def _place_candidate(
        self,
        request: PlacementRequest,
        candidate: SplitCandidate,
        *,
        pre_split: bool,
    ) -> PlacementDecision:
        providers = tuple(sorted(
            request.providers, key=lambda item: item.provider))
        if not providers:
            raise ValueError("no Provider offers")
        used_mb = {provider.provider: 0 for provider in providers}
        planning_at_ms = (
            int(self._clock_ms()) if self._clock_ms is not None else self.at_ms)
        assignments = []
        fallback: dict[str, tuple[str, ...]] = {}
        role_evidence = {}
        ack_set = tuple({
            "provider": provider.provider,
            "offer_digest": provider.offer_digest,
            "evidence_digest": provider.evidence_digest,
            "resource_sequence": provider.resource_sequence,
            "boot_epoch": provider.boot_epoch,
            "accepted_roles": tuple(provider.accepted_roles),
            "backends": tuple(provider.backends),
            "devices": tuple(provider.devices),
            "usable_gpu_memory_mb": provider.usable_gpu_memory_mb,
            "queue_depth": provider.queue_depth,
            "estimated_wait_ms": provider.estimated_wait_ms,
            "rtt_ms": provider.rtt_ms,
            "bandwidth_mbps": provider.bandwidth_mbps,
        } for provider in providers)
        for role in candidate.execution_plan.roles:
            required = candidate.requirements_by_role[role]
            peak_bytes = required.estimated_peak_gpu_memory_bytes
            if peak_bytes is None:
                raise ValueError(f"unknown runtime peak for {role}")
            required_mb = int(math.ceil(peak_bytes / (1024 * 1024)))
            scored = []
            candidate_scores = []
            for provider in providers:
                score_evidence = {
                    "provider": provider.provider,
                    "offer_digest": provider.offer_digest,
                    "evidence_digest": provider.evidence_digest,
                    "resource_sequence": provider.resource_sequence,
                    "boot_epoch": provider.boot_epoch,
                }
                if role not in provider.accepted_roles:
                    candidate_scores.append({
                        **score_evidence, "feasible": False,
                        "rejection": "ROLE_NOT_ACCEPTED",
                    })
                    continue
                compatible_backends = set(required.backends).intersection(
                    provider.backends)
                if not compatible_backends:
                    candidate_scores.append({
                        **score_evidence, "feasible": False,
                        "rejection": "BACKEND_INCOMPATIBLE",
                    })
                    continue
                if (used_mb[provider.provider] + required_mb
                        > provider.usable_gpu_memory_mb):
                    candidate_scores.append({
                        **score_evidence, "feasible": False,
                        "rejection": "GPU_CAPACITY",
                    })
                    continue
                backend = sorted(compatible_backends)[0]
                tier, cache_evidence = self._artifact_tier(
                    request, candidate, role, provider, pre_split,
                    at_ms=planning_at_ms)
                state_saving = self._state_saving(request, role, provider)
                score_components = {
                    "residency_tier": ResidencyTier(tier).name,
                    "estimated_wait_ms": float(
                        provider.estimated_wait_ms or 0.0),
                    "queue_depth": int(provider.queue_depth or 0),
                    "rtt_ms": float(provider.rtt_ms or 0.0),
                    "bandwidth_mbps": float(provider.bandwidth_mbps or 0.0),
                    "derived_state_saved_ms": state_saving,
                    "used_gpu_memory_mb": used_mb[provider.provider],
                }
                score = (
                    int(tier),
                    score_components["estimated_wait_ms"],
                    score_components["queue_depth"],
                    score_components["rtt_ms"],
                    -score_components["bandwidth_mbps"],
                    -state_saving,
                    used_mb[provider.provider],
                    provider.provider,
                )
                candidate_scores.append({
                    **score_evidence,
                    "feasible": True,
                    "score": score_components,
                })
                scored.append(
                    (score, provider, backend, self._select_device(
                        provider, backend), tier, cache_evidence,
                     state_saving))
            if not scored:
                raise ValueError(f"no feasible Provider for {role}")
            scored.sort(key=lambda item: item[0])
            fallback[role] = tuple(item[1].provider for item in scored)
            (_, provider, backend, device, tier, cache_evidence,
             state_saving) = scored[0]
            for score_evidence in candidate_scores:
                score_evidence["selected"] = (
                    score_evidence["provider"] == provider.provider)
            used_mb[provider.provider] += required_mb
            assignments.append(ProviderAssignment(
                role, provider.provider, required_mb, backend, device))
            role_evidence[role] = {
                "provider": provider.provider,
                "residency_tier": ResidencyTier(tier).name,
                "required_gpu_memory_mb": required_mb,
                "backend": backend,
                "device": device,
                "cache": cache_evidence,
                "derived_state_saved_ms": state_saving,
                "candidate_scores": tuple(candidate_scores),
            }

        specification = SplitSpecification(
            candidate_digest=candidate.candidate_digest,
            graph_digest=request.graph_digest,
            source=(
                "EXACT_PROVIDER_CACHE"
                if pre_split and all(
                    item["residency_tier"] in {
                        ResidencyTier.PINNED_GPU.name,
                        ResidencyTier.RELOAD_SAFE_GPU.name,
                        ResidencyTier.HOST_RAM.name,
                        ResidencyTier.DISK.name,
                    }
                    for item in role_evidence.values()
                )
                else "EXACT_PRE_SPLIT" if pre_split
                else "ACK_CAPACITY_GENERATED"),
            node_roles=tuple(sorted(
                candidate.execution_plan.node_roles.items())),
            provider_capacity_mb=tuple(
                (provider.provider, provider.usable_gpu_memory_mb)
                for provider in providers
            ),
        )
        evidence = {
            "schema": "ndnsf-di-placement-cost-evidence-v2",
            "split_specification": {
                "digest": specification.digest,
                "candidate_digest": specification.candidate_digest,
                "graph_digest": specification.graph_digest,
                "source": specification.source,
                "node_roles": specification.node_roles,
                "provider_capacity_mb": specification.provider_capacity_mb,
            },
            "roles": role_evidence,
            "ack_set": ack_set,
            "aggregate_gpu_memory_mb": used_mb,
        }
        cached_reuse = all(
            item["residency_tier"] in {
                ResidencyTier.PINNED_GPU.name,
                ResidencyTier.RELOAD_SAFE_GPU.name,
                ResidencyTier.HOST_RAM.name,
                ResidencyTier.DISK.name,
            }
            for item in role_evidence.values()
        )
        # Cache residency in an ACK is not by itself a resolvable artifact
        # reference.  REUSE_CACHED is safe only when the exact candidate is
        # also present in the active catalog (``pre_split``), which supplies
        # the content-addressed Data names consumed by Selection.  A Provider
        # may retain bytes on disk from an earlier process while the current
        # Repo has no manifest; the first request must publish/register that
        # candidate instead of timing out in resolve_existing().
        cached_reuse = pre_split and cached_reuse
        return PlacementDecision(
            split_id=candidate.candidate_digest,
            split_digest=candidate.candidate_digest,
            assignments=tuple(assignments),
            fallback_order=fallback,
            input_digest=request.digest(),
            evidence_digest=canonical_digest(evidence),
            artifact_preparation=(
                ArtifactPreparationMode.REUSE_CACHED
                if cached_reuse else
                ArtifactPreparationMode.PRE_SPLIT
                if pre_split else ArtifactPreparationMode.GENERATED),
            evidence=evidence,
        )

    @staticmethod
    def _select_device(provider: ProviderPlanningView, backend: str) -> str:
        """Resolve one exact execution device from the signed ACK capability."""

        devices = tuple(provider.devices)
        cuda_devices = tuple(
            device for device in devices if device.startswith("cuda:"))
        if is_cpu_backend(backend):
            if not devices:
                return "cpu"
            cpu_devices = tuple(device for device in devices if device == "cpu")
            if len(cpu_devices) == 1:
                return cpu_devices[0]
            raise ValueError(
                f"Provider {provider.provider} did not offer one CPU device")
        if len(cuda_devices) != 1:
            raise ValueError(
                f"Provider {provider.provider} must offer exactly one CUDA "
                f"device for backend {backend}")
        return cuda_devices[0]

    def _artifact_tier(
        self, request, candidate, role, provider, pre_split, *, at_ms,
    ) -> tuple[ResidencyTier, Mapping[str, Any]]:
        required = set(candidate.artifacts_by_role[role])
        matches: dict[ResidencyTier, list[str]] = {}
        for raw in provider.cached_shards:
            shard = _cache_fields(raw)
            try:
                tier = ResidencyTier[str(shard["tier"]).upper()]
            except (KeyError, TypeError):
                continue
            if tier not in {
                    ResidencyTier.PINNED_GPU,
                    ResidencyTier.RELOAD_SAFE_GPU,
                    ResidencyTier.HOST_RAM,
                    ResidencyTier.DISK}:
                continue
            if not self._valid_shard(
                    request, candidate, provider, shard, at_ms=at_ms):
                continue
            digest = str(shard.get("artifact_digest", ""))
            if digest in required:
                matches.setdefault(tier, []).append(digest)
        for tier in (
                ResidencyTier.PINNED_GPU,
                ResidencyTier.RELOAD_SAFE_GPU,
                ResidencyTier.HOST_RAM,
                ResidencyTier.DISK):
            if required.issubset(set(matches.get(tier, ()))):
                return tier, {
                    "artifact_digests": tuple(sorted(required)),
                    "boot_epoch": provider.boot_epoch,
                    "tier": tier.name,
                }
        return (
            ResidencyTier.REPOSITORY if pre_split
            else ResidencyTier.NEW_MATERIALIZATION,
            {"artifact_digests": tuple(sorted(required))},
        )

    def _valid_shard(
        self, request, candidate, provider, shard, *, at_ms=None,
    ) -> bool:
        model = request.model
        at_ms = self.at_ms if at_ms is None else int(at_ms)
        try:
            captured = int(shard["captured_at_ms"])
            expires = int(shard["expires_at_ms"])
        except (KeyError, TypeError, ValueError):
            return False
        if (
            shard.get("model_content_digest") != model.content_digest
            or shard.get("semantics_digest") != model.semantics_digest
            or shard.get("graph_digest") != request.graph_digest
            or shard.get("partition_digest") != candidate.candidate_digest
            or shard.get("precision") != model.precision
            or shard.get("backend") not in provider.backends
            or shard.get("boot_epoch") != provider.boot_epoch
            or not shard.get("cache_epoch")
            or captured > at_ms
            or at_ms - captured > self.maximum_cache_age_ms
            or expires <= at_ms
        ):
            return False
        tier = str(shard.get("tier", "")).upper()
        if tier == "PINNED_GPU" and int(
                shard.get("pin_until_ms", 0)) < request.deadline_ms:
            return False
        return True

    def _state_saving(self, request, role, provider) -> float:
        best = 0.0
        model = request.model
        role_layers = request.constraints.get("role_layers", {}).get(role)
        if (not isinstance(role_layers, (tuple, list))
                or len(role_layers) != 2):
            return best
        layer_begin, layer_end = map(int, role_layers)
        for raw in provider.reusable_state:
            state = raw if isinstance(raw, ReusableStateView) else None
            if state is None:
                continue
            if (
                state.model_content_digest == model.content_digest
                and state.semantics_digest == model.semantics_digest
                and (not self.security_domain
                     or state.security_domain == self.security_domain)
                and state.boot_epoch == provider.boot_epoch
                and state.layer_begin <= layer_begin
                and state.layer_end >= layer_end
                and state.captured_at_ms <= self.at_ms < state.expires_at_ms
                and state.pin_until_ms >= request.deadline_ms
                and role in provider.accepted_roles
            ):
                best = max(best, state.estimated_saved_ms)
        return best


class PreSplitFirstStrategy(PreassembledPartitionV2Strategy):
    """Default V3 policy over adapter-certified candidates and ACK offers.

    Feasibility and one-to-one role ownership are controlling. Exact canonical
    or assembled residency is a subordinate cost signal after a Provider is
    proven feasible; it never creates topology or permits one Provider to own
    several roles in the same Attempt.
    """

    name = "pre-split-first"
    version = "3"
    placement_profile = "DI_PLACEMENT_V3"
    state_digest = "sha256:" + hashlib.sha256(
        b"ndnsf-di-pre-split-first-v3").hexdigest()

    def __init__(
        self,
        *,
        at_ms: int = 1,
        security_domain: str = "",
        maximum_cache_age_ms: int = 30_000,
        clock_ms=None,
    ) -> None:
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
        """Return a pure one-role-per-Provider proposal from ACK_CLOSED."""

        if not providers:
            raise ValueError("V3 strategy requires Provider offers")
        if not roles:
            raise ValueError("V3 strategy requires at least one role")
        ranks_by_role: dict[str, set[int]] = {}
        for role in roles:
            ranks_by_role.setdefault(role.role, set()).add(role.rank)
        for role_name, ranks in ranks_by_role.items():
            count = sum(1 for role in roles if role.role == role_name)
            if len(ranks) != count or ranks != set(range(count)):
                raise ValueError(
                    f"V3 role rank cover is incomplete for {role_name}")

        assignments: dict[str, str] = {}
        selected_roles: list[RoleAssemblySpec] = []
        used_providers: set[str] = set()
        role_counts = {
            item.role: sum(1 for candidate in roles
                           if candidate.role == item.role)
            for item in roles
        }
        for role in sorted(roles, key=lambda item: (item.role, item.rank)):
            ranked = []
            for view in providers:
                if view.provider in used_providers:
                    continue
                if role.role not in view.accepted_roles:
                    continue
                compatible_backends = set(view.backends)
                if (role.backend not in compatible_backends
                        and f"{role.backend}-cpu" not in compatible_backends
                        and f"{role.backend}-cuda" not in compatible_backends):
                    continue
                selected_backend = role.backend
                available_devices = tuple(view.topology.devices)
                if (not available_devices and (
                        is_cpu_backend(role.backend)
                        or f"{role.backend}-cpu" in compatible_backends)):
                    available_devices = ("cpu",)
                if role.device_set:
                    available_devices = tuple(
                        item for item in available_devices
                        if item in role.device_set)
                if not available_devices:
                    continue

                resource_by_device = {
                    item.device: item for item in view.resources
                }
                available_devices = tuple(
                    device for device in available_devices
                    if ((device == "cpu" and (
                            is_cpu_backend(role.backend)
                            or f"{role.backend}-cpu" in compatible_backends))
                        or (device != "cpu"
                            and (role.backend in compatible_backends
                                 or f"{role.backend}-cuda"
                                 in compatible_backends)
                            and device in resource_by_device
                            and resource_by_device[device].free_memory_mb
                            >= role.required_device_memory_mb))
                )
                if not available_devices:
                    continue

                for selected_device in available_devices:
                    selected_backend = role.backend
                    if (selected_device == "cpu"
                            and f"{role.backend}-cpu" in compatible_backends):
                        selected_backend = f"{role.backend}-cpu"
                    elif (selected_device.startswith("cuda:")
                          and f"{role.backend}-cuda" in compatible_backends):
                        selected_backend = f"{role.backend}-cuda"
                    elif (selected_device.startswith("cuda:")
                          and role.backend not in compatible_backends
                          and "cuda" in compatible_backends):
                        selected_backend = "cuda"

                    # Reuse affects cost only after capability/device
                    # feasibility. Score every feasible device so an exact hit
                    # on cuda:1 is not hidden by lexical preference for cuda:0.
                    reuse = self._v3_reuse_cost(
                        role, view, selected_device=selected_device,
                        selected_backend=selected_backend)
                    exact = reuse[0] <= 1
                    if (view.execution_disposition
                            == ExecutionDisposition.ACCEPT_IF_EXACT_REUSE
                            and not exact):
                        continue
                    if (not exact and view.execution_disposition
                            != ExecutionDisposition.ACCEPT_WITH_PREPARATION):
                        continue
                    ranked.append((
                        *reuse,
                        view.queue_depth,
                        view.estimated_wait_ms,
                        view.rtt_ms,
                        -view.bandwidth_mbps,
                        view.provider,
                        selected_device,
                        selected_backend,
                    ))
            if not ranked:
                raise ValueError(
                    f"no distinct feasible Provider for V3 role {role.role}#{role.rank}")
            selected = min(ranked)
            provider = selected[8]
            key = (role.role if role_counts[role.role] == 1
                   else f"{role.role}#{role.rank}")
            assignments[key] = provider
            used_providers.add(provider)
            selected_roles.append(replace(
                role,
                backend=selected[10],
                # CPU is represented by an empty accelerator set; the final
                # DeviceBinding carries CPU versus SINGLE_DEVICE explicitly.
                device_set=(() if selected[9] == "cpu" else (selected[9],))))

        return PlacementProposalV3(
            request_id=request_id,
            attempt=attempt,
            model_digest=model_digest,
            graph_digest=graph_digest,
            roles=tuple(selected_roles),
            provider_by_role=assignments,
            strategy_name=self.name,
            strategy_version=self.version,
            strategy_state_digest=self.state_digest,
        )

    @staticmethod
    def _v3_reuse_cost(
        role: RoleAssemblySpec,
        view: ProviderPlanningViewV3,
        *, selected_device: str,
        selected_backend: str,
    ) -> tuple[int, int, float, float]:
        """Rank exact loaded, assembled, canonical, then cold residency.

        The tuple is a subordinate cost only. It cannot make an infeasible
        Provider feasible or change the role topology.
        """

        best = (3, 2**63 - 1, math.inf, math.inf)
        for proof in view.residency:
            if proof.role != role.role or proof.rank != role.rank:
                continue
            common = (
                (not role.model_manifest_digest
                 or proof.model_manifest_digest == role.model_manifest_digest)
                and (not role.artifact_profile_digest
                     or proof.artifact_profile_digest
                     == role.artifact_profile_digest)
                and proof.graph_digest
                == (role.graph_digest or view.graph_digest)
                and proof.protection_epoch == role.protection_epoch
            )
            if not common:
                continue
            if proof.residency_class is ResidencyClassV3.CANONICAL:
                if proof.tier is not ResidencyTierV3.CANONICAL:
                    continue
                best = min(best, (
                    2, proof.missing_verified_bytes,
                    proof.estimated_assembly_ms, proof.estimated_load_ms))
                continue
            if (proof.artifact_digest != role.artifact_digest
                    or proof.assembly_spec_digest != role.recipe_digest
                    or not proof.is_exact_reuse_proof()):
                continue
            if proof.residency_class is ResidencyClassV3.ASSEMBLED_FRAGMENT:
                if proof.backend not in {role.backend, selected_backend}:
                    continue
                best = min(best, (
                    1, 0, 0.0, proof.estimated_load_ms))
                continue
            expected_devices = (() if selected_device == "cpu"
                                else (selected_device,))
            if (proof.backend != selected_backend
                    or proof.device_set != expected_devices
                    or proof.boot_epoch != view.boot_epoch
                    or proof.topology_digest != view.topology.digest()):
                continue
            best = min(best, (0, 0, 0.0, 0.0))
        return best


__all__ = [
    "PreassembledPartitionV2Strategy", "PreSplitFirstStrategy",
    "ResidencyTier", "ReusableStateView", "SplitSpecification",
]
