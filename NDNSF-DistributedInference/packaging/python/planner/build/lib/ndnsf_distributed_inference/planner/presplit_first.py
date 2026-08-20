"""Default exact-reuse-first joint split and Provider placement strategy."""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
import hashlib
import json
import math
from typing import Any, Mapping

from ..sdk.placement import (
    ArtifactPreparationMode, ModelPlacementStrategy, PlacementDecision, PlacementRequest,
    ProviderAssignment, canonical_digest,
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


class PreSplitFirstStrategy(ModelPlacementStrategy):
    """Prefer exact verified shards, then a capacity-safe graph candidate."""

    name = "pre-split-first"
    version = "2"
    state_digest = "sha256:" + hashlib.sha256(
        b"ndnsf-di-pre-split-first-v2").hexdigest()
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
        matches = set()
        for entry in request.catalog_snapshot or ():
            if getattr(entry, "status", "") != "ACTIVE":
                continue
            if (
                getattr(entry, "model_content_digest", "")
                    == model.content_digest
                and getattr(entry, "semantics_digest", "")
                    == model.semantics_digest
                and getattr(entry, "graph_digest", "")
                    == request.graph_digest
                and getattr(entry, "backend", "")
                    in {backend for provider in request.providers
                        for backend in provider.backends}
                and getattr(entry, "precision", "") == model.precision
            ):
                matches.add(getattr(entry, "candidate_digest", ""))
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
        for role in candidate.execution_plan.roles:
            required = candidate.requirements_by_role[role]
            peak_bytes = required.estimated_peak_gpu_memory_bytes
            if peak_bytes is None:
                raise ValueError(f"unknown runtime peak for {role}")
            required_mb = int(math.ceil(peak_bytes / (1024 * 1024)))
            scored = []
            for provider in providers:
                if (role not in provider.accepted_roles
                        or not set(required.backends).intersection(
                            provider.backends)
                        or used_mb[provider.provider] + required_mb
                            > provider.usable_gpu_memory_mb):
                    continue
                backend = sorted(
                    set(required.backends).intersection(provider.backends))[0]
                tier, cache_evidence = self._artifact_tier(
                    request, candidate, role, provider, pre_split,
                    at_ms=planning_at_ms)
                state_saving = self._state_saving(request, role, provider)
                score = (
                    int(tier),
                    float(provider.estimated_wait_ms or 0.0),
                    float(provider.rtt_ms or 0.0),
                    -float(provider.bandwidth_mbps or 0.0),
                    -state_saving,
                    used_mb[provider.provider],
                    provider.provider,
                )
                scored.append(
                    (score, provider, backend, tier, cache_evidence,
                     state_saving))
            if not scored:
                raise ValueError(f"no feasible Provider for {role}")
            scored.sort(key=lambda item: item[0])
            fallback[role] = tuple(item[1].provider for item in scored)
            _, provider, backend, tier, cache_evidence, state_saving = scored[0]
            used_mb[provider.provider] += required_mb
            assignments.append(ProviderAssignment(
                role, provider.provider, required_mb, backend))
            role_evidence[role] = {
                "provider": provider.provider,
                "residency_tier": ResidencyTier(tier).name,
                "required_gpu_memory_mb": required_mb,
                "cache": cache_evidence,
                "derived_state_saved_ms": state_saving,
            }

        specification = SplitSpecification(
            candidate_digest=candidate.candidate_digest,
            graph_digest=request.graph_digest,
            source=(
                "EXACT_PRE_SPLIT" if pre_split
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
            "aggregate_gpu_memory_mb": used_mb,
        }
        return PlacementDecision(
            split_id=candidate.candidate_digest,
            split_digest=candidate.candidate_digest,
            assignments=tuple(assignments),
            fallback_order=fallback,
            input_digest=request.digest(),
            evidence_digest=canonical_digest(evidence),
            artifact_preparation=(
                ArtifactPreparationMode.PRE_SPLIT
                if pre_split else ArtifactPreparationMode.GENERATED),
            evidence=evidence,
        )

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
                    request, provider, shard, at_ms=at_ms):
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

    def _valid_shard(self, request, provider, shard, *, at_ms=None) -> bool:
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


__all__ = [
    "PreSplitFirstStrategy", "ResidencyTier", "ReusableStateView",
    "SplitSpecification",
]
