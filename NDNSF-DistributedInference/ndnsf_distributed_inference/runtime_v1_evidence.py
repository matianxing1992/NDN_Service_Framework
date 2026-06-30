"""Runtime v1 evidence writers for NDNSF-DI experiments."""

from __future__ import annotations

from pathlib import Path
from typing import Any

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
