#!/usr/bin/env python3
"""Resource-aware reusable LLM planner for NDNSF-DI.

The planner intentionally prefers a linear stage pipeline. It only emits
parallel shards when the minimum stage cannot fit any available provider.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
from typing import Any

try:
    from ndnsf_distributed_inference.runtime_v1 import (
        parse_ack_metadata,
        validate_linear_llm_plan,
    )
except ImportError:  # pragma: no cover - script can run before editable install
    validate_linear_llm_plan = None

    def parse_ack_metadata(payload):
        text = payload.decode("utf-8", errors="replace") if isinstance(payload, bytes) else str(payload)
        fields: dict[str, str] = {}
        for item in text.split(";"):
            if not item or "=" not in item:
                continue
            key, value = item.split("=", 1)
            key = key.strip()
            if key:
                fields[key] = value.strip()
        return fields


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def parse_semicolon_fields(payload: bytes | str) -> dict[str, str]:
    return {str(key): str(value) for key, value in parse_ack_metadata(payload).items()}


def _string_list(value: Any, default: list[str]) -> list[str]:
    if value is None:
        return list(default)
    if isinstance(value, str):
        return [item.strip() for item in value.split(",") if item.strip()]
    return [str(item) for item in value]


def as_float(payload: dict[str, Any], name: str, default: float = 0.0) -> float:
    value = payload.get(name, default)
    if value is None:
        return default
    return float(value)


def as_int(payload: dict[str, Any], name: str, default: int = 0) -> int:
    value = payload.get(name, default)
    if value is None:
        return default
    return int(value)


def normalize_provider(raw: dict[str, Any]) -> dict[str, Any]:
    gpu_memory = as_float(raw, "gpuMemoryMb")
    ram_memory = as_float(raw, "ramMemoryMb")
    explicit_stage_capacity = as_float(raw, "llmStageCapacityMb", gpu_memory)
    stage_capacity = min(gpu_memory, explicit_stage_capacity) if explicit_stage_capacity > 0 else gpu_memory
    return {
        "provider": str(raw["provider"]),
        "node": str(raw.get("node", "")),
        "gpuMemoryMb": gpu_memory,
        "ramMemoryMb": ram_memory,
        "flopsTflops": as_float(raw, "flopsTflops"),
        "llmStageCapacityMb": stage_capacity,
        "llmMaxStageLayers": as_int(raw, "llmMaxStageLayers", 10**9),
        "modelFamilies": _string_list(raw.get("modelFamilies"), ["llm"]),
        "maxContextTokens": as_int(raw, "maxContextTokens", 0),
        "kvCacheBudgetMb": as_float(raw, "kvCacheBudgetMb", 0.0),
    }


def profile_from_ack_candidates(payload: dict[str, Any]) -> dict[str, Any]:
    providers = []
    for index, candidate in enumerate(payload.get("candidates", [])):
        if not bool(candidate.get("status", True)):
            continue
        typed_fields = parse_ack_metadata(candidate.get("payload", ""))
        typed_profile = typed_fields.get("providerProfile")
        fields = {str(key): str(value) for key, value in typed_fields.items()}
        if isinstance(typed_profile, dict):
            provider = {
                "provider": str(
                    typed_profile.get("provider") or
                    candidate.get("provider") or
                    candidate.get("providerName") or ""),
                "node": str(typed_profile.get("node", candidate.get("node", ""))),
                "gpuMemoryMb": typed_profile.get("gpu_memory_mb", typed_profile.get("gpuMemoryMb", 0)),
                "ramMemoryMb": typed_profile.get("ram_memory_mb", typed_profile.get("ramMemoryMb", 0)),
                "flopsTflops": typed_profile.get("flops_tflops", typed_profile.get("flopsTflops", 0)),
                "llmStageCapacityMb": typed_profile.get(
                    "llm_stage_capacity_mb",
                    typed_profile.get("llmStageCapacityMb", 0)),
                "llmMaxStageLayers": typed_profile.get(
                    "llm_max_stage_layers",
                    typed_profile.get("llmMaxStageLayers", 10**9)),
                "modelFamilies": typed_profile.get(
                    "model_families",
                    typed_profile.get("modelFamilies", "llm")),
                "maxContextTokens": typed_profile.get(
                    "max_context_tokens",
                    typed_profile.get("maxContextTokens", 0)),
                "kvCacheBudgetMb": typed_profile.get(
                    "kv_cache_budget_mb",
                    typed_profile.get("kvCacheBudgetMb", 0)),
            }
            if not provider["provider"]:
                provider["provider"] = f"ack-candidate-{index}"
            providers.append(provider)
            continue
        if "llm" not in _string_list(fields.get("modelFamilies", "llm"), ["llm"]):
            continue
        provider = {
            "provider": str(candidate.get("provider") or candidate.get("providerName") or ""),
            "node": str(candidate.get("node", "")),
            "gpuMemoryMb": fields.get("gpuMemoryMb", candidate.get("gpuMemoryMb", 0)),
            "ramMemoryMb": fields.get("ramMemoryMb", candidate.get("ramMemoryMb", 0)),
            "flopsTflops": fields.get("flopsTflops", candidate.get("flopsTflops", 0)),
            "llmStageCapacityMb": fields.get(
                "llmStageCapacityMb",
                candidate.get("llmStageCapacityMb", fields.get("gpuMemoryMb", 0))),
            "llmMaxStageLayers": fields.get(
                "llmMaxStageLayers",
                candidate.get("llmMaxStageLayers", 10**9)),
            "modelFamilies": fields.get("modelFamilies", candidate.get("modelFamilies", "llm")),
            "maxContextTokens": fields.get(
                "maxContextTokens",
                candidate.get("maxContextTokens", 0)),
            "kvCacheBudgetMb": fields.get(
                "kvCacheBudgetMb",
                candidate.get("kvCacheBudgetMb", 0)),
        }
        if not provider["provider"]:
            provider["provider"] = f"ack-candidate-{index}"
        providers.append(provider)
    return {
        "profileId": str(payload.get("profileId", "ack-candidates")),
        "providers": providers,
        "source": "ack-candidates",
    }


def stage_memory_mb(model: dict[str, Any], layer_count: int) -> float:
    return (
        as_float(model, "fixedRuntimeMemoryMb") +
        as_float(model, "kvCacheMemoryMb") +
        as_float(model, "memoryPerLayerMb") * layer_count
    )


def stage_flops_tflop(model: dict[str, Any], layer_count: int) -> float:
    return as_float(model, "flopsPerLayerTflop") * layer_count


def provider_layer_capacity(provider: dict[str, Any], model: dict[str, Any]) -> int:
    memory_budget = min(provider["llmStageCapacityMb"], provider["ramMemoryMb"])
    fixed = as_float(model, "fixedRuntimeMemoryMb") + as_float(model, "kvCacheMemoryMb")
    per_layer = max(as_float(model, "memoryPerLayerMb"), 0.001)
    memory_layers = int(math.floor(max(0.0, memory_budget - fixed) / per_layer))
    return max(0, min(int(provider["llmMaxStageLayers"]), memory_layers))


def provider_can_run(provider: dict[str, Any], memory_mb: float, layer_count: int) -> bool:
    return (
        memory_mb <= provider["llmStageCapacityMb"] and
        memory_mb <= provider["ramMemoryMb"] and
        layer_count <= provider["llmMaxStageLayers"] and
        provider["flopsTflops"] > 0
    )


def choose_provider(providers: list[dict[str, Any]],
                    model: dict[str, Any],
                    layer_count: int,
                    previous_provider: str = "") -> dict[str, Any] | None:
    memory = stage_memory_mb(model, layer_count)
    flops = stage_flops_tflop(model, layer_count)
    candidates = [
        provider for provider in providers
        if provider_can_run(provider, memory, layer_count)
    ]
    if not candidates:
        return None
    # Favor faster providers, but add a small penalty for reusing the same
    # provider in adjacent stages so the reusable plan exposes pipeline roles.
    def score(provider: dict[str, Any]) -> tuple[float, float]:
        compute_ms = 1000.0 * flops / max(provider["flopsTflops"], 0.001)
        reuse_penalty = 2.0 if provider["provider"] == previous_provider else 0.0
        memory_slack = provider["llmStageCapacityMb"] - memory
        return (compute_ms + reuse_penalty, -memory_slack)
    return min(candidates, key=score)


def stage_latency_ms(provider: dict[str, Any], flops_tflop: float) -> float:
    return 1000.0 * flops_tflop / max(provider["flopsTflops"], 0.001)


def queue_risk(utilization: float) -> str:
    if utilization <= 0.0:
        return "idle"
    if utilization < 0.70:
        return "low"
    if utilization < 0.90:
        return "medium"
    if utilization < 1.0:
        return "high"
    return "saturated"


def queue_pressure(utilization: float) -> float | None:
    if utilization <= 0.0:
        return 0.0
    if utilization >= 1.0:
        return None
    return utilization / max(1.0 - utilization, 0.001)


def proportional_weight(provider: dict[str, Any],
                        max_memory: float,
                        max_flops: float) -> float:
    memory_ratio = provider["llmStageCapacityMb"] / max(max_memory, 0.001)
    compute_ratio = provider["flopsTflops"] / max(max_flops, 0.001)
    return max(0.0, min(memory_ratio, compute_ratio))


def proportional_layer_counts(providers: list[dict[str, Any]],
                              model: dict[str, Any],
                              layers: int) -> dict[str, int]:
    max_memory = max(provider["llmStageCapacityMb"] for provider in providers)
    max_flops = max(provider["flopsTflops"] for provider in providers)
    weighted = []
    for provider in providers:
        capacity = provider_layer_capacity(provider, model)
        weight = proportional_weight(provider, max_memory, max_flops)
        weighted.append({
            "provider": provider,
            "capacity": capacity,
            "weight": weight,
        })
    usable = [item for item in weighted if item["capacity"] > 0 and item["weight"] > 0]
    if not usable:
        return {}
    total_weight = sum(item["weight"] for item in usable)
    raw_targets = {
        item["provider"]["provider"]: layers * item["weight"] / total_weight
        for item in usable
    }
    counts = {
        item["provider"]["provider"]: min(item["capacity"], int(math.floor(
            raw_targets[item["provider"]["provider"]])))
        for item in usable
    }
    assigned = sum(counts.values())
    while assigned < layers:
        candidates = [
            item for item in usable
            if counts[item["provider"]["provider"]] < item["capacity"]
        ]
        if not candidates:
            break
        candidates.sort(
            key=lambda item: (
                raw_targets[item["provider"]["provider"]] -
                counts[item["provider"]["provider"]],
                item["weight"],
            ),
            reverse=True)
        counts[candidates[0]["provider"]["provider"]] += 1
        assigned += 1
    return counts


def build_proportional_stages(model: dict[str, Any],
                              providers: list[dict[str, Any]],
                              layers: int,
                              activation_boundary_mb: float) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    counts = proportional_layer_counts(providers, model, layers)
    if sum(counts.values()) < layers:
        return [], [
            shard_single_stage(
                model,
                providers,
                layer_index,
                stage_memory_mb(model, max(1, as_int(model, "minimumStageLayers", 1))))
            for layer_index in range(sum(counts.values()), layers)
        ]

    max_memory = max(provider["llmStageCapacityMb"] for provider in providers)
    max_flops = max(provider["flopsTflops"] for provider in providers)
    ordered = sorted(
        [provider for provider in providers if counts.get(provider["provider"], 0) > 0],
        key=lambda provider: (
            proportional_weight(provider, max_memory, max_flops),
            provider["provider"],
        ))
    stages = []
    cursor = 0
    for provider in ordered:
        layer_count = counts[provider["provider"]]
        if layer_count <= 0:
            continue
        memory = stage_memory_mb(model, layer_count)
        flops = stage_flops_tflop(model, layer_count)
        stage_id = f"stage-{len(stages)}"
        stages.append({
            "stageId": stage_id,
            "role": f"/LLM/Stage/{len(stages)}",
            "provider": provider["provider"],
            "node": provider["node"],
            "layerStart": cursor,
            "layerEnd": cursor + layer_count - 1,
            "layerCount": layer_count,
            "memoryMb": round(memory, 3),
            "flopsTflop": round(flops, 6),
            "estimatedComputeMs": round(stage_latency_ms(provider, flops), 3),
            "activationOutMb": round(activation_boundary_mb, 3) if cursor + layer_count < layers else 0.0,
            "proportionalWeight": round(proportional_weight(provider, max_memory, max_flops), 6),
            "layerCapacity": provider_layer_capacity(provider, model),
        })
        cursor += layer_count
    return stages, []


def shard_single_stage(model: dict[str, Any],
                       providers: list[dict[str, Any]],
                       layer_index: int,
                       memory_mb: float) -> dict[str, Any]:
    ordered = sorted(providers, key=lambda item: item["flopsTflops"], reverse=True)
    aggregate_capacity = 0.0
    shards = []
    for shard_index, provider in enumerate(ordered):
        aggregate_capacity += min(provider["llmStageCapacityMb"], provider["ramMemoryMb"])
        shards.append({
            "shardIndex": shard_index,
            "provider": provider["provider"],
            "node": provider["node"],
            "capacityMb": provider["llmStageCapacityMb"],
        })
        if aggregate_capacity >= memory_mb:
            break
    if aggregate_capacity < memory_mb:
        raise RuntimeError(
            f"cannot place layer {layer_index}: need {memory_mb:.1f} MB, "
            f"aggregate capacity is {aggregate_capacity:.1f} MB")
    return {
        "stageId": f"stage-{layer_index}",
        "layerStart": layer_index,
        "layerEnd": layer_index,
        "reason": "minimum single-layer stage does not fit any provider",
        "memoryMb": round(memory_mb, 3),
        "highCost": True,
        "shards": shards,
    }


def build_greedy_stages(model: dict[str, Any],
                        providers: list[dict[str, Any]],
                        layers: int,
                        min_stage_layers: int,
                        activation_boundary_mb: float) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    stages = []
    shards = []
    cursor = 0
    previous_provider = ""
    while cursor < layers:
        remaining = layers - cursor
        chosen = None
        chosen_layers = 0
        # Try the largest contiguous stage first. This keeps transfer
        # boundaries low, which is the desired LLM default.
        for layer_count in range(remaining, min_stage_layers - 1, -1):
            provider = choose_provider(providers, model, layer_count, previous_provider)
            if provider is not None:
                chosen = provider
                chosen_layers = layer_count
                break
        if chosen is None:
            minimum_memory = stage_memory_mb(model, min_stage_layers)
            shards.append(shard_single_stage(model, providers, cursor, minimum_memory))
            cursor += min_stage_layers
            previous_provider = ""
            continue

        memory = stage_memory_mb(model, chosen_layers)
        flops = stage_flops_tflop(model, chosen_layers)
        stage_id = f"stage-{len(stages)}"
        stages.append({
            "stageId": stage_id,
            "role": f"/LLM/Stage/{len(stages)}",
            "provider": chosen["provider"],
            "node": chosen["node"],
            "layerStart": cursor,
            "layerEnd": cursor + chosen_layers - 1,
            "layerCount": chosen_layers,
            "memoryMb": round(memory, 3),
            "flopsTflop": round(flops, 6),
            "estimatedComputeMs": round(stage_latency_ms(chosen, flops), 3),
            "activationOutMb": round(activation_boundary_mb, 3) if cursor + chosen_layers < layers else 0.0,
        })
        cursor += chosen_layers
        previous_provider = chosen["provider"]
    return stages, shards


def summarize_prediction(stages: list[dict[str, Any]],
                         dependencies: list[dict[str, Any]],
                         providers: list[dict[str, Any]],
                         target_rps: float,
                         provider_workers: int,
                         activation_bandwidth_mbps: float,
                         prediction_compute_scale: float,
                         prediction_fixed_stage_ms: float) -> dict[str, Any]:
    provider_map = {str(provider["provider"]): provider for provider in providers}
    provider_load: dict[str, dict[str, Any]] = {
        provider_name: {
            "provider": provider_name,
            "node": provider.get("node", ""),
            "roles": [],
            "layerCount": 0,
            "stageCount": 0,
            "estimatedServiceMsPerRequest": 0.0,
            "estimatedComputeMsPerRequest": 0.0,
            "dependencyIngressCount": 0,
            "dependencyEgressCount": 0,
            "dependencyIngressMbPerRequest": 0.0,
            "dependencyEgressMbPerRequest": 0.0,
            "providerWorkers": provider_workers,
        }
        for provider_name, provider in provider_map.items()
    }
    stage_by_id = {str(stage["stageId"]): stage for stage in stages}
    for stage in stages:
        provider_name = str(stage["provider"])
        load = provider_load.setdefault(provider_name, {
            "provider": provider_name,
            "node": str(stage.get("node", "")),
            "roles": [],
            "layerCount": 0,
            "stageCount": 0,
            "estimatedServiceMsPerRequest": 0.0,
            "estimatedComputeMsPerRequest": 0.0,
            "dependencyIngressCount": 0,
            "dependencyEgressCount": 0,
            "dependencyIngressMbPerRequest": 0.0,
            "dependencyEgressMbPerRequest": 0.0,
            "providerWorkers": provider_workers,
        })
        load["roles"].append(str(stage["role"]))
        load["layerCount"] += int(stage["layerCount"])
        load["stageCount"] += 1
        compute_ms = (
            prediction_fixed_stage_ms
            if prediction_fixed_stage_ms > 0.0 else
            float(stage["estimatedComputeMs"]) * prediction_compute_scale
        )
        load["estimatedComputeMsPerRequest"] += compute_ms
        load["estimatedServiceMsPerRequest"] += compute_ms

    for dependency in dependencies:
        from_stage = stage_by_id.get(str(dependency.get("from", "")))
        to_stage = stage_by_id.get(str(dependency.get("to", "")))
        if from_stage is None or to_stage is None:
            continue
        estimated_mb = float(dependency.get("estimatedBytes", 0)) / (1024.0 * 1024.0)
        from_provider = str(from_stage["provider"])
        to_provider = str(to_stage["provider"])
        if from_provider == to_provider:
            continue
        provider_load[from_provider]["dependencyEgressCount"] += 1
        provider_load[from_provider]["dependencyEgressMbPerRequest"] += estimated_mb
        provider_load[to_provider]["dependencyIngressCount"] += 1
        provider_load[to_provider]["dependencyIngressMbPerRequest"] += estimated_mb

    bandwidth = max(activation_bandwidth_mbps, 0.001)
    total_transfer_mb = sum(
        float(dependency.get("estimatedBytes", 0)) / (1024.0 * 1024.0)
        for dependency in dependencies)
    total_transfer_ms = 1000.0 * total_transfer_mb * 8.0 / bandwidth
    for load in provider_load.values():
        transfer_mb = (
            float(load["dependencyIngressMbPerRequest"]) +
            float(load["dependencyEgressMbPerRequest"]))
        transfer_ms = 1000.0 * transfer_mb * 8.0 / bandwidth
        load["estimatedDependencyTransferMsPerRequest"] = round(transfer_ms, 3)
        load["estimatedNetworkCostMsPerRequest"] = round(transfer_ms, 3)
        load["estimatedEndToEndCostMsPerRequest"] = round(
            float(load["estimatedServiceMsPerRequest"]) + transfer_ms, 3)
        load["estimatedServiceMsPerRequest"] = round(
            float(load["estimatedServiceMsPerRequest"]), 3)
        load["estimatedComputeMsPerRequest"] = round(
            float(load["estimatedComputeMsPerRequest"]), 3)
        load["dependencyIngressMbPerRequest"] = round(
            float(load["dependencyIngressMbPerRequest"]), 3)
        load["dependencyEgressMbPerRequest"] = round(
            float(load["dependencyEgressMbPerRequest"]), 3)
        utilization = (
            target_rps * float(load["estimatedServiceMsPerRequest"]) /
            (1000.0 * max(provider_workers, 1))
            if target_rps > 0.0 else
            0.0
        )
        load["predictedUtilization"] = round(utilization, 6)
        pressure = queue_pressure(utilization)
        load["predictedQueuePressure"] = (
            None if pressure is None else round(pressure, 6))
        load["predictedQueueRisk"] = queue_risk(utilization)
        load["roles"] = sorted(load["roles"])

    active_loads = [
        load for load in provider_load.values()
        if int(load.get("stageCount", 0)) > 0
    ]
    if target_rps > 0.0:
        bottleneck = max(
            active_loads,
            key=lambda load: float(load["predictedUtilization"]),
            default={})
        max_utilization = float(bottleneck.get("predictedUtilization", 0.0))
    else:
        bottleneck = max(
            active_loads,
            key=lambda load: float(load["estimatedServiceMsPerRequest"]),
            default={})
        max_utilization = 0.0
    max_compute_ms = max(
        [float(load["estimatedComputeMsPerRequest"]) for load in active_loads] or [0.0])
    limit_kind = (
        "transfer-limited"
        if total_transfer_ms > max_compute_ms else
        "compute-limited"
    )
    return {
        "targetRps": round(target_rps, 6),
        "providerWorkers": max(provider_workers, 1),
        "activationBandwidthMbps": round(bandwidth, 3),
        "predictionComputeScale": round(prediction_compute_scale, 6),
        "predictionFixedStageMs": round(prediction_fixed_stage_ms, 3),
        "providerLoad": {
            name: {
                key: value
                for key, value in load.items()
                if key != "provider"
            }
            for name, load in sorted(provider_load.items())
        },
        "bottleneckProvider": str(bottleneck.get("provider", "")),
        "maxPredictedUtilization": round(max_utilization, 6),
        "dependencyTransferCount": len(dependencies),
        "crossProviderDependencyCount": sum(
            1 for dependency in dependencies
            if stage_by_id.get(str(dependency.get("from", "")), {}).get("provider") !=
            stage_by_id.get(str(dependency.get("to", "")), {}).get("provider")),
        "totalActivationTransferMb": round(total_transfer_mb, 3),
        "estimatedTotalTransferMsPerRequest": round(total_transfer_ms, 3),
        "limitKind": limit_kind,
    }


def build_plan(model: dict[str, Any],
               profile: dict[str, Any],
               mode: str = "greedy",
               target_rps: float = 0.0,
               provider_workers: int = 1,
               activation_bandwidth_mbps: float = 1000.0,
               prediction_compute_scale: float = 1.0,
               prediction_fixed_stage_ms: float = 0.0) -> dict[str, Any]:
    providers = sorted(
        [normalize_provider(item) for item in profile.get("providers", [])],
        key=lambda item: item["provider"])
    if not providers:
        raise RuntimeError("provider profile must contain at least one provider")
    layers = as_int(model, "layers")
    if layers <= 0:
        raise RuntimeError("model spec must contain a positive layers value")
    min_stage_layers = max(1, as_int(model, "minimumStageLayers", 1))
    activation_boundary_mb = as_float(model, "activationBoundaryMb")
    if mode not in ("greedy", "proportional"):
        raise RuntimeError(f"unsupported planner mode: {mode}")

    if mode == "proportional":
        stages, shards = build_proportional_stages(model, providers, layers, activation_boundary_mb)
    else:
        stages, shards = build_greedy_stages(
            model, providers, layers, min_stage_layers, activation_boundary_mb)

    dependencies = []
    for index in range(len(stages) - 1):
        dependencies.append({
            "from": stages[index]["stageId"],
            "to": stages[index + 1]["stageId"],
            "tensor": f"activation-{index}-to-{index + 1}",
            "estimatedBytes": int(activation_boundary_mb * 1024 * 1024),
        })
    layer_allocation: dict[str, int] = {}
    for stage in stages:
        provider_name = str(stage["provider"])
        layer_allocation[provider_name] = (
            layer_allocation.get(provider_name, 0) + int(stage["layerCount"]))
    prediction = summarize_prediction(
        stages,
        dependencies,
        providers,
        max(0.0, target_rps),
        max(1, provider_workers),
        max(0.001, activation_bandwidth_mbps),
        max(0.0, prediction_compute_scale),
        max(0.0, prediction_fixed_stage_ms))

    reusable_key = {
        "modelId": model.get("modelId"),
        "revision": model.get("revision"),
        "plannerVersion": model.get("plannerVersion", "llm-resource-aware-v1"),
        "plannerMode": mode,
        "profileId": profile.get("profileId", "unknown-profile"),
        "providers": [
            {
                "provider": provider["provider"],
                "gpuMemoryMb": provider["gpuMemoryMb"],
                "ramMemoryMb": provider["ramMemoryMb"],
                "flopsTflops": provider["flopsTflops"],
                "llmStageCapacityMb": provider["llmStageCapacityMb"],
            }
            for provider in providers
        ],
    }
    plan_id = hashlib.sha256(
        json.dumps(reusable_key, sort_keys=True).encode("utf-8")
    ).hexdigest()[:16]
    return {
        "planId": plan_id,
        "reusable": True,
        "reuseKey": reusable_key,
        "plannerKind": "llm-pipeline",
        "plannerMode": mode,
        "modelFamily": "llm",
        "modelId": model.get("modelId"),
        "modelRevision": model.get("revision"),
        "context": {
            "contextWindowTokens": as_int(model, "contextWindowTokens", 0),
            "tokenizerId": str(model.get("tokenizerId", "")),
            "kvCacheBytesPerTokenPerLayer": as_int(
                model,
                "kvCacheBytesPerTokenPerLayer",
                as_int(model, "kvCacheBytesPerToken", 0)),
            "supportsPrefill": bool(model.get("supportsPrefill", True)),
            "supportsDecode": bool(model.get("supportsDecode", True)),
            "supportsStreaming": bool(model.get("supportsStreaming", False)),
            "stateObjects": [
                "PromptChunk",
                "PrefixState",
                "SessionState",
                "KvBlock",
                "GenerationChunk",
            ],
        },
        "strategy": "proportional-linear-stage" if mode == "proportional" else "linear-stage-first",
        "strategyReason": (
            "LLM layers are assigned in proportion to normalized provider "
            "memory/compute capacity; sharding is used only when a minimum "
            "stage cannot fit one provider."
            if mode == "proportional" else
            "LLM inference is planned as contiguous linear stages first; "
            "stage sharding is used only when a minimum stage cannot fit one provider."
        ),
        "stages": stages,
        "dependencies": dependencies,
        "shards": shards,
        "resourceProfiles": providers,
        "prediction": prediction,
        "summary": {
            "stageCount": len(stages),
            "dependencyCount": len(dependencies),
            "shardedStageCount": len(shards),
            "totalEstimatedComputeMs": round(
                sum(float(stage["estimatedComputeMs"]) for stage in stages), 3),
            "totalActivationTransferMb": round(
                activation_boundary_mb * max(0, len(stages) - 1), 3),
            "layerAllocation": layer_allocation,
            "maxStageComputeMs": round(
                max([float(stage["estimatedComputeMs"]) for stage in stages] or [0.0]), 3),
            "predictedBottleneckProvider": prediction["bottleneckProvider"],
            "maxPredictedUtilization": prediction["maxPredictedUtilization"],
            "predictionLimitKind": prediction["limitKind"],
            "crossProviderDependencyCount": prediction["crossProviderDependencyCount"],
        },
    }


def validate_plan(plan: dict[str, Any], *, expect_shards: bool | None) -> None:
    if validate_linear_llm_plan is not None:
        validate_linear_llm_plan(plan)
    stages = plan.get("stages", [])
    if not stages and not plan.get("shards"):
        raise RuntimeError("plan produced neither stages nor shards")
    if stages:
        previous_end = -1
        for stage in stages:
            if int(stage["layerStart"]) <= previous_end:
                raise RuntimeError(f"non-monotonic layer range: {stage}")
            previous_end = int(stage["layerEnd"])
    if expect_shards is True and not plan.get("shards"):
        raise RuntimeError("expected forced sharding, but plan had no shards")
    if expect_shards is False and plan.get("shards"):
        raise RuntimeError("expected linear pipeline, but plan had shards")


def cache_path_for_plan(cache_dir: Path, plan: dict[str, Any]) -> Path:
    return cache_dir / f"{plan['planId']}.json"


def write_plan_with_cache(out: Path, plan: dict[str, Any], cache_dir: Path | None) -> dict[str, Any]:
    cache_info = {
        "enabled": cache_dir is not None,
        "hit": False,
        "path": "",
    }
    if cache_dir is not None:
        cache_dir.mkdir(parents=True, exist_ok=True)
        plan_cache_path = cache_path_for_plan(cache_dir, plan)
        cache_info["path"] = str(plan_cache_path)
        if plan_cache_path.exists():
            cache_info["hit"] = True
            cached = load_json(plan_cache_path)
            write_json(out, cached)
            return cache_info
        write_json(plan_cache_path, plan)
    write_json(out, plan)
    return cache_info


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-spec", type=Path, required=True)
    parser.add_argument("--provider-profiles", type=Path)
    parser.add_argument("--ack-candidates-json", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cache-dir", type=Path)
    parser.add_argument("--mode", choices=["greedy", "proportional"], default="greedy")
    parser.add_argument("--target-rps", type=float, default=0.0)
    parser.add_argument("--provider-workers", type=int, default=1)
    parser.add_argument("--activation-bandwidth-mbps", type=float, default=1000.0)
    parser.add_argument("--prediction-compute-scale", type=float, default=1.0)
    parser.add_argument("--prediction-fixed-stage-ms", type=float, default=0.0)
    parser.add_argument("--validate", action="store_true")
    parser.add_argument("--expect-shards", choices=["yes", "no", "any"], default="any")
    args = parser.parse_args()

    if args.provider_profiles is None and args.ack_candidates_json is None:
        raise RuntimeError("one of --provider-profiles or --ack-candidates-json is required")
    if args.provider_profiles is not None and args.ack_candidates_json is not None:
        raise RuntimeError("use only one of --provider-profiles or --ack-candidates-json")
    profile = (
        load_json(args.provider_profiles)
        if args.provider_profiles is not None else
        profile_from_ack_candidates(load_json(args.ack_candidates_json))
    )
    if args.target_rps < 0.0:
        raise RuntimeError("--target-rps must be non-negative")
    if args.provider_workers <= 0:
        raise RuntimeError("--provider-workers must be positive")
    if args.activation_bandwidth_mbps <= 0.0:
        raise RuntimeError("--activation-bandwidth-mbps must be positive")
    if args.prediction_compute_scale < 0.0:
        raise RuntimeError("--prediction-compute-scale must be non-negative")
    if args.prediction_fixed_stage_ms < 0.0:
        raise RuntimeError("--prediction-fixed-stage-ms must be non-negative")
    plan = build_plan(
        load_json(args.model_spec),
        profile,
        mode=args.mode,
        target_rps=args.target_rps,
        provider_workers=args.provider_workers,
        activation_bandwidth_mbps=args.activation_bandwidth_mbps,
        prediction_compute_scale=args.prediction_compute_scale,
        prediction_fixed_stage_ms=args.prediction_fixed_stage_ms)
    cache = write_plan_with_cache(args.out, plan, args.cache_dir)
    if args.validate:
        expectation = None
        if args.expect_shards == "yes":
            expectation = True
        elif args.expect_shards == "no":
            expectation = False
        validate_plan(plan, expect_shards=expectation)
    print(
        f"wrote {args.out} planId={plan['planId']} "
        f"stages={plan['summary']['stageCount']} "
        f"shards={plan['summary']['shardedStageCount']} "
        f"cacheHit={str(cache['hit']).lower()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
