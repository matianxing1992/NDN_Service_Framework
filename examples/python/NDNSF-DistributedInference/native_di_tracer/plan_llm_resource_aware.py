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


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def parse_semicolon_fields(payload: bytes | str) -> dict[str, str]:
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
    }


def profile_from_ack_candidates(payload: dict[str, Any]) -> dict[str, Any]:
    providers = []
    for index, candidate in enumerate(payload.get("candidates", [])):
        if not bool(candidate.get("status", True)):
            continue
        fields = parse_semicolon_fields(candidate.get("payload", ""))
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


def build_plan(model: dict[str, Any], profile: dict[str, Any]) -> dict[str, Any]:
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

    dependencies = []
    for index in range(len(stages) - 1):
        dependencies.append({
            "from": stages[index]["stageId"],
            "to": stages[index + 1]["stageId"],
            "tensor": f"activation-{index}-to-{index + 1}",
            "estimatedBytes": int(activation_boundary_mb * 1024 * 1024),
        })

    reusable_key = {
        "modelId": model.get("modelId"),
        "revision": model.get("revision"),
        "plannerVersion": model.get("plannerVersion", "llm-resource-aware-v1"),
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
        "modelFamily": "llm",
        "modelId": model.get("modelId"),
        "modelRevision": model.get("revision"),
        "strategy": "linear-stage-first",
        "strategyReason": (
            "LLM inference is planned as contiguous linear stages first; "
            "stage sharding is used only when a minimum stage cannot fit one provider."
        ),
        "stages": stages,
        "dependencies": dependencies,
        "shards": shards,
        "resourceProfiles": providers,
        "summary": {
            "stageCount": len(stages),
            "dependencyCount": len(dependencies),
            "shardedStageCount": len(shards),
            "totalEstimatedComputeMs": round(
                sum(float(stage["estimatedComputeMs"]) for stage in stages), 3),
            "totalActivationTransferMb": round(
                activation_boundary_mb * max(0, len(stages) - 1), 3),
        },
    }


def validate_plan(plan: dict[str, Any], *, expect_shards: bool | None) -> None:
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
    plan = build_plan(load_json(args.model_spec), profile)
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
