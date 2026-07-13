"""Runtime v1 evidence writers for NDNSF-DI experiments."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
from pathlib import PurePosixPath
import re
from typing import Any, Mapping

from .runtime_v1 import (
    ExactForwardCacheEntry,
    ExactForwardCacheManager,
    KvCacheTelemetry,
    ModelManifestV1,
    RuntimeTelemetryV1,
    build_local_llm_plan,
    exact_forward_cache_key_for_stage,
    export_telemetry_csv,
    load_provider_profiles,
    read_json,
    simulate_prefill_decode,
    to_plain,
    write_json,
    write_runtime_report,
)


def _policy_layer_allocation(policy_summary: dict[str, Any] | None) -> dict[str, int]:
    if not policy_summary:
        return {}
    summary = policy_summary.get("summary", {})
    if not isinstance(summary, dict):
        return {}
    allocation = summary.get("layerAllocation", {})
    if not isinstance(allocation, dict):
        return {}
    return {str(provider): int(layers) for provider, layers in allocation.items()}


def _decision_table(lease_layout: dict[str, Any], *, prefix_id: str,
                    generated_tokens: int) -> list[dict[str, Any]]:
    stages = list(lease_layout.get("stages", []))
    providers = sorted({str(stage.get("provider", "")) for stage in stages if stage.get("provider")})
    return [
        {
            "choice": "linear-stage-split",
            "selected": len(providers) > 1,
            "reason": "LLM stages are assigned across providers by normalized memory and compute capacity.",
        },
        {
            "choice": "single-provider",
            "selected": len(providers) == 1,
            "reason": "Avoid cross-provider dependency transfer when one provider can hold the full plan.",
        },
        {
            "choice": "sharded-stage",
            "selected": False,
            "reason": "Only use shard splits when no provider can fit a complete stage.",
        },
        {
            "choice": "exact-forward-cache",
            "selected": bool(prefix_id),
            "reason": "Reuse only when token prefix, model, plan, stage definition, runtime, and security epoch match.",
        },
        {
            "choice": "streaming-decode",
            "selected": generated_tokens > 0,
            "reason": "Return GenerationChunk objects instead of waiting for a full final response.",
        },
    ]


def write_minindn_runtime_v1_evidence(*,
                                      out_dir: str | Path,
                                      model_path: str | Path,
                                      provider_profiles_path: str | Path,
                                      target_rps: float = 0.0,
                                      context_tokens: int = 1024,
                                      generated_tokens: int = 32,
                                      context_class: str = "",
                                      prefix_id: str = "",
                                      session_id: str = "",
                                      policy_summary: dict[str, Any] | None = None) -> dict[str, Any]:
    """Write Runtime v1 plan/telemetry evidence next to a MiniNDN run.

    This helper is intentionally deterministic.  It does not replace the real
    network run; it records the Runtime v1 contracts that the network run used
    or should be able to consume: plan lease, provider telemetry, cache
    placement, and streaming decode timing.
    """

    target = Path(out_dir)
    target.mkdir(parents=True, exist_ok=True)
    model = ModelManifestV1.from_dict(read_json(model_path))
    providers = load_provider_profiles(provider_profiles_path)
    if not providers:
        raise ValueError("Runtime v1 evidence requires at least one provider profile")
    resolved_context_class = context_class or ("long" if context_tokens > 4096 else "short")
    lease = build_local_llm_plan(
        model,
        providers,
        target_rps=target_rps,
        context_class=resolved_context_class,
        prefix_id=prefix_id,
        session_id=session_id,
    )
    allocation = lease.layout.get("summary", {}).get("layerAllocation", {})
    cache_provider = lease.cache_placement.provider if lease.cache_placement else ""
    plan_hash = lease.plan_key.digest()
    split_layout_hash = lease.plan_id
    cache_stage = next(
        (stage for stage in lease.layout.get("stages", [])
         if str(stage.get("provider", "")) == cache_provider),
        (lease.layout.get("stages") or [{}])[0],
    )
    exact_cache_key = exact_forward_cache_key_for_stage(
        model,
        cache_stage,
        token_ids=range(context_tokens),
        plan_hash=plan_hash,
        split_layout_hash=split_layout_hash,
        runtime_backend="minindn-native-tracer",
        dtype="float16",
        quantization="none",
        security_epoch="minindn",
    )
    exact_cache = ExactForwardCacheManager()
    if prefix_id and cache_provider:
        exact_cache.put(ExactForwardCacheEntry(
            key=exact_cache_key,
            provider=cache_provider,
            object_name=exact_cache_key.data_name(provider=f"/{cache_provider}"),
            byte_count=int(model.kv_cache_mb(context_tokens, max(1, int(cache_stage.get("layerCount", 1)))) *
                           1024 * 1024),
            token_count=context_tokens,
        ))
    exact_cache_hit = exact_cache.get(exact_cache_key) is not None
    telemetry: dict[str, RuntimeTelemetryV1] = {}
    for provider in providers:
        assigned_layers = int(allocation.get(provider.provider, 0))
        kv_used = 0.0
        hits = 0
        misses = 0
        if provider.provider == cache_provider:
            kv_used = model.kv_cache_mb(context_tokens, max(1, assigned_layers))
            hits = 1 if exact_cache_hit else 0
            misses = 0 if exact_cache_hit else 1
        telemetry[provider.provider] = RuntimeTelemetryV1(
            provider=provider.provider,
            active_workers=min(max(1, provider.max_workers), 1),
            free_memory_mb=max(0.0, provider.gpu_memory_mb - assigned_layers * model.memory_per_layer_mb),
            model_loaded=True,
            runtime_backend="minindn-native-tracer",
            service_time_ewma_ms=round(assigned_layers * model.flops_per_layer_tflop * 10.0, 6),
            queue_wait_ewma_ms=0.0,
            kv_cache=KvCacheTelemetry(
                budget_mb=provider.kv_cache_budget_mb,
                used_mb=kv_used,
                max_context_tokens=provider.max_context_tokens,
                resident_prefix_ids=(prefix_id,) if prefix_id and provider.provider == cache_provider else (),
                resident_session_ids=(session_id,) if session_id and provider.provider == cache_provider else (),
                resident_exact_cache_key_digests=(
                    (exact_cache_key.digest(),)
                    if exact_cache_hit and provider.provider == cache_provider else ()
                ),
                hits=hits,
                misses=misses,
            ),
        )

    generation_provider = next(
        (provider for provider in providers if provider.provider == cache_provider),
        max(providers, key=lambda item: item.flops_tflops),
    )
    generation = simulate_prefill_decode(
        request_id="minindn-runtime-v1-evidence",
        provider=generation_provider,
        model=model,
        prompt_tokens=context_tokens,
        generated_tokens=generated_tokens,
        microbatch=4,
    )
    lease_path = target / "runtime-v1-plan-lease.json"
    telemetry_path = target / "runtime-v1-telemetry.csv"
    report_path = target / "runtime-v1-report.json"
    summary_path = target / "runtime-v1-minindn-evidence-summary.json"
    write_json(lease_path, lease)
    export_telemetry_csv(telemetry_path, telemetry.values())
    decisions = _decision_table(lease.layout, prefix_id=prefix_id, generated_tokens=generated_tokens)
    write_runtime_report(
        report_path,
        lease=lease,
        telemetry=telemetry,
        decision_table=decisions,
    )

    policy_allocation = _policy_layer_allocation(policy_summary)
    plain_generation = to_plain(generation)
    evidence = {
        "status": "available",
        "schema": "ndnsf-di-runtime-v1-minindn-evidence",
        "modelId": model.model_id,
        "modelRevision": model.revision,
        "planId": lease.plan_id,
        "providerSet": [provider.provider for provider in providers],
        "layerAllocation": allocation,
        "policyLayerAllocation": policy_allocation,
        "allocationMatchesPolicy": (
            allocation == policy_allocation if policy_allocation else None
        ),
        "contextClass": resolved_context_class,
        "contextTokens": context_tokens,
        "generatedTokens": generated_tokens,
        "cacheProvider": cache_provider,
        "cacheExpectedHit": bool(lease.cache_placement.expected_hit) if lease.cache_placement else False,
        "exactForwardCacheKeyDigest": exact_cache_key.digest(),
        "exactForwardCacheHit": exact_cache_hit,
        "exactForwardCacheObjectName": exact_cache_key.data_name(
            provider=f"/{cache_provider}" if cache_provider else ""),
        "exactForwardCacheSource": "provider-local",
        "fallbackPlanIds": list(lease.fallback_plan_ids),
        "timeToFirstTokenMs": plain_generation["time_to_first_token_ms"],
        "interTokenMs": plain_generation["inter_token_ms"],
        "generationChunkCount": len(plain_generation["chunks"]),
        "decisionTable": decisions,
        "leasePath": str(lease_path),
        "telemetryCsv": str(telemetry_path),
        "reportPath": str(report_path),
        "summaryPath": str(summary_path),
    }
    write_json(summary_path, evidence)
    return evidence


# Spec 107 evidence contracts are kept here because this module already owns
# runtime evidence serialization. They do not introduce network or policy state.

_SHA256_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
_CANDIDATE_RE = re.compile(r"^spec107-c1(?:-[0-9a-f]{12}){6}$")
_FORBIDDEN_EVIDENCE_FIELDS = frozenset({
    "prompt", "payload", "tensor", "kv", "kvvalue", "token", "tokenvalue",
    "secret", "privatekey", "usertoken", "providertoken",
})
_CLEANUP_KEYS = ("threads", "waits", "leases", "routes", "processes", "attempts")
SPEC107_RELEASE_DIMENSIONS = (
    "lineage",
    "evidenceIntegrity",
    "correctness",
    "performance",
    "recovery",
    "applicationSecurity",
    "localOperations",
)
_SPEC107_CAMPAIGN_KINDS = frozenset({
    "diagnostic", "correctness", "performance", "fault", "canary",
    "operations", "soak", "release-gate",
})


def _reject_forbidden_evidence_fields(value: object) -> None:
    if isinstance(value, dict):
        for key, nested in value.items():
            if str(key).lower() in _FORBIDDEN_EVIDENCE_FIELDS:
                raise ValueError(f"EVIDENCE_FORBIDDEN_FIELD:{key}")
            _reject_forbidden_evidence_fields(nested)
    elif isinstance(value, list):
        for nested in value:
            _reject_forbidden_evidence_fields(nested)


def _required_text(payload: Mapping[str, object], key: str, code: str) -> str:
    value = payload.get(key)
    if not isinstance(value, str) or not value:
        raise ValueError(f"{code}:{key}")
    return value


def _positive_int(payload: Mapping[str, object], key: str, code: str) -> int:
    value = payload.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f"{code}:{key}")
    return value


def _nonnegative_int(payload: Mapping[str, object], key: str, code: str) -> int:
    value = payload.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"{code}:{key}")
    return value


@dataclass(frozen=True)
class OwnedProcessV1:
    pid: int
    process_group_id: int
    proc_start_time_ticks: int
    parent_pid: int
    campaign_id: str
    role: str
    provider_name: str
    provider_boot_id: str
    command_digest: str
    executable_digest: str

    @classmethod
    def from_dict(cls, payload: Mapping[str, object]) -> "OwnedProcessV1":
        _reject_forbidden_evidence_fields(payload)
        campaign = _required_text(payload, "campaignId", "OWNED_PROCESS_FIELD_INVALID")
        if not campaign.startswith("spec107-c1-fault-") or "spec105" in campaign.lower():
            raise ValueError("OWNED_PROCESS_CAMPAIGN_INVALID")
        command = _required_text(payload, "commandDigest", "OWNED_PROCESS_FIELD_INVALID")
        executable = _required_text(payload, "executableDigest", "OWNED_PROCESS_FIELD_INVALID")
        if _SHA256_RE.fullmatch(command) is None or _SHA256_RE.fullmatch(executable) is None:
            raise ValueError("OWNED_PROCESS_DIGEST_INVALID")
        return cls(
            pid=_positive_int(payload, "pid", "OWNED_PROCESS_FIELD_INVALID"),
            process_group_id=_positive_int(
                payload, "processGroupId", "OWNED_PROCESS_FIELD_INVALID"),
            proc_start_time_ticks=_positive_int(
                payload, "procStartTimeTicks", "OWNED_PROCESS_FIELD_INVALID"),
            parent_pid=_positive_int(payload, "parentPid", "OWNED_PROCESS_FIELD_INVALID"),
            campaign_id=campaign,
            role=_required_text(payload, "role", "OWNED_PROCESS_FIELD_INVALID"),
            provider_name=_required_text(
                payload, "providerName", "OWNED_PROCESS_FIELD_INVALID"),
            provider_boot_id=_required_text(
                payload, "providerBootId", "OWNED_PROCESS_FIELD_INVALID"),
            command_digest=command,
            executable_digest=executable,
        )

    def to_dict(self) -> dict[str, object]:
        return {
            "pid": self.pid,
            "processGroupId": self.process_group_id,
            "procStartTimeTicks": self.proc_start_time_ticks,
            "parentPid": self.parent_pid,
            "campaignId": self.campaign_id,
            "role": self.role,
            "providerName": self.provider_name,
            "providerBootId": self.provider_boot_id,
            "commandDigest": self.command_digest,
            "executableDigest": self.executable_digest,
        }


@dataclass(frozen=True)
class FaultCleanupV1:
    proven: bool
    baseline: dict[str, int]
    after: dict[str, int]

    @classmethod
    def from_dict(cls, payload: Mapping[str, object]) -> "FaultCleanupV1":
        if payload.get("proven") is not True:
            raise ValueError("FAULT_CLEANUP_UNPROVEN")
        baseline_raw = payload.get("baseline")
        after_raw = payload.get("after")
        if not isinstance(baseline_raw, dict) or not isinstance(after_raw, dict):
            raise ValueError("FAULT_CLEANUP_COUNTS_INVALID")
        if set(baseline_raw) != set(_CLEANUP_KEYS) or set(after_raw) != set(_CLEANUP_KEYS):
            raise ValueError("FAULT_CLEANUP_COUNTS_INVALID")
        baseline: dict[str, int] = {}
        after: dict[str, int] = {}
        for key in _CLEANUP_KEYS:
            before_value = baseline_raw[key]
            after_value = after_raw[key]
            if (
                isinstance(before_value, bool) or not isinstance(before_value, int)
                or before_value < 0 or isinstance(after_value, bool)
                or not isinstance(after_value, int) or after_value < 0
            ):
                raise ValueError(f"FAULT_CLEANUP_COUNTS_INVALID:{key}")
            baseline[key] = before_value
            after[key] = after_value
            if after_value > before_value:
                raise ValueError(f"FAULT_CLEANUP_BOUND_EXCEEDED:{key}")
        return cls(True, baseline, after)

    def to_dict(self) -> dict[str, object]:
        return {"proven": self.proven, "baseline": self.baseline, "after": self.after}


@dataclass(frozen=True)
class LiveFaultRecordV1:
    candidate_id: str
    campaign_id: str
    cell_id: str
    command_digest: str
    target: OwnedProcessV1
    trigger: str
    trigger_monotonic_us: int
    injection_monotonic_us: int
    injection_applied: bool
    network_injection: bool
    intended_effect: str
    observed_effect: str
    attempt_epoch_before: int
    attempt_epoch_after: int
    provider_boot_before: str
    provider_boot_after: str
    replacement_count: int
    original_deadline_epoch_ms: int
    current_deadline_epoch_ms: int
    cancel_supersede_authenticated: bool
    authoritative_terminal_count: int
    terminal_reason: str
    cleanup: FaultCleanupV1
    verdict: str
    schema: str = "ndnsf-di-spec107-live-fault-v1"

    @classmethod
    def from_dict(cls, payload: Mapping[str, object]) -> "LiveFaultRecordV1":
        _reject_forbidden_evidence_fields(payload)
        if payload.get("schema") != "ndnsf-di-spec107-live-fault-v1":
            raise ValueError("FAULT_SCHEMA_INVALID")
        candidate = _required_text(payload, "candidateId", "FAULT_FIELD_INVALID")
        if _CANDIDATE_RE.fullmatch(candidate) is None:
            raise ValueError("FAULT_CANDIDATE_INVALID")
        target_raw = payload.get("target")
        cleanup_raw = payload.get("cleanup")
        if not isinstance(target_raw, dict) or not isinstance(cleanup_raw, dict):
            raise ValueError("FAULT_FIELD_INVALID:nested")
        target = OwnedProcessV1.from_dict(target_raw)
        campaign = _required_text(payload, "campaignId", "FAULT_FIELD_INVALID")
        if campaign != target.campaign_id:
            raise ValueError("FAULT_TARGET_CAMPAIGN_MISMATCH")
        command = _required_text(payload, "commandDigest", "FAULT_FIELD_INVALID")
        if _SHA256_RE.fullmatch(command) is None:
            raise ValueError("FAULT_COMMAND_DIGEST_INVALID")
        trigger_time = _nonnegative_int(
            payload, "triggerMonotonicUs", "FAULT_FIELD_INVALID")
        injection_time = _nonnegative_int(
            payload, "injectionMonotonicUs", "FAULT_FIELD_INVALID")
        if injection_time < trigger_time:
            raise ValueError("FAULT_INJECTION_BEFORE_TRIGGER")
        if payload.get("injectionApplied") is not True:
            raise ValueError("FAULT_INJECTION_NOT_APPLIED")
        if payload.get("networkInjection") is not True:
            raise ValueError("FAULT_NETWORK_INJECTION_REQUIRED")
        observed = _required_text(payload, "observedEffect", "FAULT_OBSERVED_EFFECT_MISSING")
        replacement = _nonnegative_int(
            payload, "replacementCount", "FAULT_FIELD_INVALID")
        if replacement > 1:
            raise ValueError("FAULT_REPLACEMENT_BOUND_EXCEEDED")
        original_deadline = _positive_int(
            payload, "originalDeadlineEpochMs", "FAULT_FIELD_INVALID")
        current_deadline = _positive_int(
            payload, "currentDeadlineEpochMs", "FAULT_FIELD_INVALID")
        if original_deadline != current_deadline:
            raise ValueError("FAULT_DEADLINE_CHANGED")
        attempt_before = _nonnegative_int(
            payload, "attemptEpochBefore", "FAULT_FIELD_INVALID")
        attempt_after = _nonnegative_int(
            payload, "attemptEpochAfter", "FAULT_FIELD_INVALID")
        if attempt_after < attempt_before:
            raise ValueError("FAULT_ATTEMPT_EPOCH_STALE")
        if target.provider_boot_id != _required_text(
            payload, "providerBootBefore", "FAULT_FIELD_INVALID"
        ):
            raise ValueError("FAULT_TARGET_BOOT_MISMATCH")
        authenticated = payload.get("cancelSupersedeAuthenticated") is True
        if (replacement > 0 or attempt_after != attempt_before) and not authenticated:
            raise ValueError("FAULT_SUPERSEDE_UNAUTHENTICATED")
        authority_count = _nonnegative_int(
            payload, "authoritativeTerminalCount", "FAULT_FIELD_INVALID")
        if authority_count != 1:
            raise ValueError("FAULT_TERMINAL_AUTHORITY_INVALID")
        verdict = _required_text(payload, "verdict", "FAULT_FIELD_INVALID")
        if verdict not in {"PASS", "BLOCK", "INVALID"}:
            raise ValueError("FAULT_VERDICT_INVALID")
        return cls(
            candidate_id=candidate,
            campaign_id=campaign,
            cell_id=_required_text(payload, "cellId", "FAULT_FIELD_INVALID"),
            command_digest=command,
            target=target,
            trigger=_required_text(payload, "trigger", "FAULT_FIELD_INVALID"),
            trigger_monotonic_us=trigger_time,
            injection_monotonic_us=injection_time,
            injection_applied=True,
            network_injection=True,
            intended_effect=_required_text(
                payload, "intendedEffect", "FAULT_FIELD_INVALID"),
            observed_effect=observed,
            attempt_epoch_before=attempt_before,
            attempt_epoch_after=attempt_after,
            provider_boot_before=_required_text(
                payload, "providerBootBefore", "FAULT_FIELD_INVALID"),
            provider_boot_after=_required_text(
                payload, "providerBootAfter", "FAULT_FIELD_INVALID"),
            replacement_count=replacement,
            original_deadline_epoch_ms=original_deadline,
            current_deadline_epoch_ms=current_deadline,
            cancel_supersede_authenticated=authenticated,
            authoritative_terminal_count=authority_count,
            terminal_reason=_required_text(
                payload, "terminalReason", "FAULT_FIELD_INVALID"),
            cleanup=FaultCleanupV1.from_dict(cleanup_raw),
            verdict=verdict,
        )

    def to_dict(self) -> dict[str, object]:
        return {
            "schema": self.schema,
            "candidateId": self.candidate_id,
            "campaignId": self.campaign_id,
            "cellId": self.cell_id,
            "commandDigest": self.command_digest,
            "target": self.target.to_dict(),
            "trigger": self.trigger,
            "triggerMonotonicUs": self.trigger_monotonic_us,
            "injectionMonotonicUs": self.injection_monotonic_us,
            "injectionApplied": self.injection_applied,
            "networkInjection": self.network_injection,
            "intendedEffect": self.intended_effect,
            "observedEffect": self.observed_effect,
            "attemptEpochBefore": self.attempt_epoch_before,
            "attemptEpochAfter": self.attempt_epoch_after,
            "providerBootBefore": self.provider_boot_before,
            "providerBootAfter": self.provider_boot_after,
            "replacementCount": self.replacement_count,
            "originalDeadlineEpochMs": self.original_deadline_epoch_ms,
            "currentDeadlineEpochMs": self.current_deadline_epoch_ms,
            "cancelSupersedeAuthenticated": self.cancel_supersede_authenticated,
            "authoritativeTerminalCount": self.authoritative_terminal_count,
            "terminalReason": self.terminal_reason,
            "cleanup": self.cleanup.to_dict(),
            "verdict": self.verdict,
        }


@dataclass(frozen=True)
class Spec107EvidenceBindingV1:
    candidate_id: str
    campaign_id: str
    campaign_kind: str
    source_digest: str
    profile_digest: str
    model_digest: str
    plan_digest: str
    artifact_digest: str
    lineage_digest: str
    physical_production_overall: str = "DEFERRED"
    schema: str = "ndnsf-di-spec107-evidence-binding-v1"

    @classmethod
    def from_dict(cls, payload: Mapping[str, object]) -> "Spec107EvidenceBindingV1":
        _reject_forbidden_evidence_fields(payload)
        if payload.get("schema") != "ndnsf-di-spec107-evidence-binding-v1":
            raise ValueError("SPEC107_EVIDENCE_SCHEMA_INVALID")
        candidate = _required_text(payload, "candidateId", "SPEC107_EVIDENCE_FIELD_INVALID")
        if _CANDIDATE_RE.fullmatch(candidate) is None or "spec105" in candidate.lower():
            raise ValueError("SPEC107_EVIDENCE_CANDIDATE_INVALID")
        campaign = _required_text(payload, "campaignId", "SPEC107_EVIDENCE_FIELD_INVALID")
        if not campaign.startswith("spec107-c1-") or "spec105" in campaign.lower():
            raise ValueError("SPEC107_EVIDENCE_CAMPAIGN_INVALID")
        kind = _required_text(payload, "campaignKind", "SPEC107_EVIDENCE_FIELD_INVALID")
        if kind not in _SPEC107_CAMPAIGN_KINDS:
            raise ValueError("SPEC107_EVIDENCE_CAMPAIGN_KIND_INVALID")
        digest_fields = {
            "source_digest": "sourceDigest",
            "profile_digest": "profileDigest",
            "model_digest": "modelDigest",
            "plan_digest": "planDigest",
            "artifact_digest": "artifactDigest",
            "lineage_digest": "lineageDigest",
        }
        digests: dict[str, str] = {}
        for target, source in digest_fields.items():
            value = _required_text(payload, source, "SPEC107_EVIDENCE_FIELD_INVALID")
            if _SHA256_RE.fullmatch(value) is None:
                raise ValueError(f"SPEC107_EVIDENCE_DIGEST_INVALID:{source}")
            digests[target] = value
        if payload.get("physicalProductionOverall") != "DEFERRED":
            raise ValueError("PHYSICAL_STATUS_MUST_BE_DEFERRED")
        return cls(
            candidate_id=candidate,
            campaign_id=campaign,
            campaign_kind=kind,
            physical_production_overall="DEFERRED",
            **digests,
        )

    def to_dict(self) -> dict[str, object]:
        return {
            "schema": self.schema,
            "candidateId": self.candidate_id,
            "campaignId": self.campaign_id,
            "campaignKind": self.campaign_kind,
            "sourceDigest": self.source_digest,
            "profileDigest": self.profile_digest,
            "modelDigest": self.model_digest,
            "planDigest": self.plan_digest,
            "artifactDigest": self.artifact_digest,
            "lineageDigest": self.lineage_digest,
            "physicalProductionOverall": self.physical_production_overall,
        }


def _release_path(value: object, root: Path) -> tuple[str, Path] | None:
    if not isinstance(value, str) or not value or "\\" in value:
        return None
    pure = PurePosixPath(value)
    if pure.is_absolute() or any(part in ("", ".", "..") for part in pure.parts):
        return None
    path = (root / Path(*pure.parts)).resolve(strict=False)
    try:
        path.relative_to(root)
    except ValueError:
        return None
    return pure.as_posix(), path


def evaluate_spec107_release_input(
    payload: Mapping[str, object],
    *,
    evidence_root: str | Path,
) -> dict[str, object]:
    """Validate release inputs without producing the final Spec 107 gate."""

    errors: list[str] = []
    try:
        _reject_forbidden_evidence_fields(payload)
    except ValueError as exc:
        errors.append(str(exc))
    if payload.get("schema") != "ndnsf-di-spec107-release-input-v1":
        errors.append("RELEASE_INPUT_SCHEMA_INVALID")
    candidate = payload.get("candidateId")
    if not isinstance(candidate, str) or _CANDIDATE_RE.fullmatch(candidate) is None:
        errors.append("RELEASE_CANDIDATE_INVALID")
        candidate = ""
    predecessor = payload.get("predecessor")
    if not isinstance(predecessor, dict):
        errors.append("PREDECESSOR_INVALID")
    else:
        if predecessor.get("releaseId") != "spec105-local-minindn-candidate-r2":
            errors.append("PREDECESSOR_RELEASE_ID_INVALID")
        if predecessor.get("minindnCandidateOverall") != "BLOCK":
            errors.append("PREDECESSOR_BLOCK_NOT_PRESERVED")
        if predecessor.get("physicalProductionOverall") != "DEFERRED":
            errors.append("PREDECESSOR_PHYSICAL_STATUS_INVALID")
    if payload.get("physicalProductionOverall") != "DEFERRED":
        errors.append("PHYSICAL_STATUS_MUST_BE_DEFERRED")

    dimensions = payload.get("dimensions")
    expected_dimensions = set(SPEC107_RELEASE_DIMENSIONS)
    if not isinstance(dimensions, dict) or set(dimensions) != expected_dimensions:
        errors.append("DIMENSION_SET_INVALID")
        dimensions = dimensions if isinstance(dimensions, dict) else {}
    referenced_paths: set[str] = set()
    for name in SPEC107_RELEASE_DIMENSIONS:
        dimension = dimensions.get(name)
        if not isinstance(dimension, dict):
            continue
        if dimension.get("status") != "PASS":
            errors.append(f"DIMENSION_BLOCK:{name}")
        artifacts = dimension.get("artifacts")
        if not isinstance(artifacts, list) or not all(
                isinstance(value, str) for value in artifacts):
            errors.append(f"DIMENSION_ARTIFACTS_INVALID:{name}")
        else:
            referenced_paths.update(artifacts)

    root = Path(evidence_root).resolve()
    manifest = payload.get("evidenceManifest")
    if not isinstance(manifest, list):
        manifest = []
        errors.append("EVIDENCE_MANIFEST_INVALID")
    bound_paths: set[str] = set()
    for row in manifest:
        if not isinstance(row, dict):
            errors.append("EVIDENCE_ROW_INVALID")
            continue
        value = row.get("path")
        resolved = _release_path(value, root)
        if resolved is None:
            errors.append(f"EVIDENCE_PATH_INVALID:{value}")
            continue
        relative, path = resolved
        if relative in bound_paths:
            errors.append(f"EVIDENCE_PATH_DUPLICATE:{relative}")
            continue
        bound_paths.add(relative)
        if row.get("candidateId") != candidate:
            errors.append(f"EVIDENCE_CANDIDATE_MISMATCH:{relative}")
        if row.get("eligibility") != "EVIDENCE_ELIGIBLE":
            errors.append(f"EVIDENCE_INELIGIBLE:{relative}")
        expected_digest = row.get("sha256")
        if not isinstance(expected_digest, str) or _SHA256_RE.fullmatch(expected_digest) is None:
            errors.append(f"EVIDENCE_DIGEST_INVALID:{relative}")
        if not path.is_file():
            errors.append(f"EVIDENCE_MISSING:{relative}")
            continue
        actual = "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected_digest:
            errors.append(f"EVIDENCE_DIGEST_MISMATCH:{relative}")
    for relative in sorted(referenced_paths - bound_paths):
        errors.append(f"EVIDENCE_UNBOUND:{relative}")
    unique_errors = sorted(set(errors))
    return {
        "schema": "ndnsf-di-spec107-release-input-evaluation-v1",
        "candidateId": candidate,
        "eligible": not unique_errors,
        "errors": unique_errors,
        "physicalProductionOverall": "DEFERRED",
    }
