#!/usr/bin/env python3
"""Generate planner optimization evidence for the NativeTracer DI plan."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


SERVICE = "/Inference/NativeTracer"
MODEL = "/Model/NativeTracer/Qwen2.5-0.5B-Minimal/v1"
SOURCE_MODEL = "Qwen/Qwen2.5-0.5B-Instruct"
CONTRACT_VERSION = "di-plan-v2"

ROLE_BACKBONE = "/Backbone"
ROLE_HEAD0 = "/Head/Shard/0"
ROLE_HEAD1 = "/Head/Shard/1"
ROLE_MERGE = "/Merge"
ROLES = [ROLE_BACKBONE, ROLE_HEAD0, ROLE_HEAD1, ROLE_MERGE]

DEFAULT_PROFILES = {
    ROLE_BACKBONE: {
        "provider": "/NDNSF-DI/Tracer/provider/backbone",
        "node": "ucla",
        "computeScore": 1.0,
        "queueDepth": 0,
        "roleComputeMs": 4.0,
    },
    ROLE_HEAD0: {
        "provider": "/NDNSF-DI/Tracer/provider/head0",
        "node": "arizona",
        "computeScore": 1.0,
        "queueDepth": 0,
        "roleComputeMs": 2.5,
    },
    ROLE_HEAD1: {
        "provider": "/NDNSF-DI/Tracer/provider/head1",
        "node": "wustl",
        "computeScore": 1.0,
        "queueDepth": 0,
        "roleComputeMs": 2.5,
    },
    ROLE_MERGE: {
        "provider": "/NDNSF-DI/Tracer/provider/merge",
        "node": "neu",
        "computeScore": 1.0,
        "queueDepth": 0,
        "roleComputeMs": 1.5,
    },
}

DEFAULT_NETWORK = {
    "defaultRttMs": 6.0,
    "defaultBandwidthMbps": 100.0,
    "dependencyExchangeOverheadMs": 50.0,
    "pairOverrides": [],
}


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_sha256_sidecar(path: Path) -> None:
    path.with_suffix(path.suffix + ".sha256").write_text(
        sha256_file(path) + "\n",
        encoding="utf-8")


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def service_from_plan(plan: dict[str, Any]) -> dict[str, Any]:
    for service in plan.get("services", []):
        if service.get("service") == SERVICE:
            return service
    raise RuntimeError(f"service {SERVICE} not found in native execution plan")


def service_from_manifest(manifest: dict[str, Any]) -> dict[str, Any]:
    for service in manifest.get("services", []):
        if service.get("name") == SERVICE:
            return service
    raise RuntimeError(f"service {SERVICE} not found in service manifest")


def load_profiles(path: str) -> dict[str, dict[str, Any]]:
    profiles = json.loads(Path(path).read_text(encoding="utf-8")) if path else {}
    result = {role: dict(values) for role, values in DEFAULT_PROFILES.items()}
    for role, values in profiles.get("roles", profiles).items():
        if role in result and isinstance(values, dict):
            result[role].update(values)
    return result


def load_network(path: str) -> dict[str, Any]:
    network = dict(DEFAULT_NETWORK)
    if path:
        raw = json.loads(Path(path).read_text(encoding="utf-8"))
        if isinstance(raw, dict):
            network.update(raw)
            dynamic_overrides = dynamic_pair_overrides(
                raw,
                float(network.get("dynamicProfileMinConfidence", 0.5)))
            if dynamic_overrides:
                existing = list(network.get("pairOverrides", []) or [])
                seen = {
                    (item.get("from"), item.get("to"), item.get("keyScope", ""))
                    for item in existing
                    if isinstance(item, dict)
                }
                for override in dynamic_overrides:
                    key = (override.get("from"), override.get("to"),
                           override.get("keyScope", ""))
                    if key not in seen:
                        existing.append(override)
                        seen.add(key)
                network["pairOverrides"] = existing
    return network


def dynamic_pair_overrides(raw: dict[str, Any],
                           min_confidence: float) -> list[dict[str, Any]]:
    overrides = []
    for edge in raw.get("edges", []) or []:
        if not isinstance(edge, dict):
            continue
        confidence = float(edge.get("confidence", 0.0) or 0.0)
        if confidence < min_confidence:
            continue
        producer = edge.get("producerProvider") or edge.get("producer")
        consumer = edge.get("consumerProvider") or edge.get("consumer")
        goodput = float(edge.get("goodputMbps", 0.0) or 0.0)
        if not producer or not consumer or goodput <= 0.0:
            continue
        rtt_ms = float(edge.get("rttMs",
                       edge.get("firstByteMs",
                       edge.get("elapsedMs", DEFAULT_NETWORK["defaultRttMs"]))) or 0.0)
        overrides.append({
            "from": producer,
            "to": consumer,
            "rttMs": rtt_ms,
            "bandwidthMbps": goodput,
            "confidence": confidence,
            "sampleCount": int(edge.get("sampleCount", 0) or 0),
            "source": raw.get("schema", "dynamic-network-profile"),
            "keyScope": edge.get("keyScope", ""),
        })
    return overrides


def dependency_edges(service_plan: dict[str, Any]) -> list[dict[str, Any]]:
    edges = []
    for dep in service_plan.get("dependencies", []):
        producers = dep.get("producers", [])
        consumers = dep.get("consumers", [])
        if not producers or not consumers:
            continue
        edges.append({
            "scope": dep.get("keyScope", ""),
            "producer": producers[0],
            "consumer": consumers[0],
            "expectedBytes": int(dep.get("expectedBytes", 0)),
            "expectedSegments": int(dep.get("expectedSegments", 0)),
        })
    return edges


def provider_for(role: str, placement: dict[str, str], profiles: dict[str, dict[str, Any]]) -> str:
    return placement.get(role) or str(profiles[role]["provider"])


def profile_for_provider(provider: str, profiles: dict[str, dict[str, Any]]) -> dict[str, Any]:
    for profile in profiles.values():
        if profile.get("provider") == provider:
            return profile
    return {
        "provider": provider,
        "node": "estimated",
        "computeScore": 1.0,
        "queueDepth": 0,
        "roleComputeMs": 2.0,
    }


def pair_network(producer: str, consumer: str, network: dict[str, Any]) -> tuple[float, float]:
    for override in network.get("pairOverrides", []):
        if (override.get("from") == producer and override.get("to") == consumer):
            return (
                float(override.get("rttMs", network["defaultRttMs"])),
                float(override.get("bandwidthMbps", network["defaultBandwidthMbps"])),
            )
    return float(network["defaultRttMs"]), float(network["defaultBandwidthMbps"])


def transfer_ms(bytes_count: int,
                rtt_ms: float,
                bandwidth_mbps: float,
                exchange_overhead_ms: float = 0.0) -> float:
    if bandwidth_mbps <= 0:
        raise RuntimeError("bandwidthMbps must be positive")
    serialization_ms = (bytes_count * 8.0) / (bandwidth_mbps * 1000.0)
    return rtt_ms + serialization_ms + max(0.0, exchange_overhead_ms)


def role_compute_ms(role: str, provider: str, profiles: dict[str, dict[str, Any]]) -> float:
    role_profile = profiles.get(role, {})
    provider_profile = profile_for_provider(provider, profiles)
    base = float(role_profile.get("roleComputeMs", provider_profile.get("roleComputeMs", 2.0)))
    score = max(float(provider_profile.get("computeScore", 1.0)), 0.05)
    return base / score


def queue_ms(provider: str, profiles: dict[str, dict[str, Any]]) -> float:
    profile = profile_for_provider(provider, profiles)
    return float(profile.get("queueDepth", 0)) * 0.25


def compute_work_items(candidate: dict[str, Any],
                       profiles: dict[str, dict[str, Any]]) -> list[dict[str, str]]:
    placement = candidate["rolePlacement"]
    return [
        {
            "role": str(item["role"]),
            "provider": str(item.get("provider") or provider_for(str(item["role"]), placement, profiles)),
        }
        for item in (
            candidate.get("computeWork") or [
                {"role": role, "provider": provider_for(role, placement, profiles)}
                for role in candidate["activeRoles"]
            ]
        )
    ]


def edge_transfer_ms(edge: dict[str, Any],
                     placement: dict[str, str],
                     profiles: dict[str, dict[str, Any]],
                     network: dict[str, Any]) -> float:
    producer_provider = provider_for(edge["producer"], placement, profiles)
    consumer_provider = provider_for(edge["consumer"], placement, profiles)
    if producer_provider == consumer_provider:
        return 0.0
    rtt_ms, bandwidth_mbps = pair_network(producer_provider, consumer_provider, network)
    return transfer_ms(
        int(edge["expectedBytes"]),
        rtt_ms,
        bandwidth_mbps,
        float(network.get("dependencyExchangeOverheadMs", 0.0) or 0.0))


def crossing_dependency_pressure(candidate: dict[str, Any],
                                 profiles: dict[str, dict[str, Any]],
                                 network: dict[str, Any],
                                 target_rps: float) -> dict[str, float]:
    placement = candidate["rolePlacement"]
    crossing_bytes = 0
    max_link_utilization = 0.0
    rate_pressure = 0.0
    for edge in candidate["dependencies"]:
        producer_provider = provider_for(edge["producer"], placement, profiles)
        consumer_provider = provider_for(edge["consumer"], placement, profiles)
        if producer_provider == consumer_provider:
            continue
        edge_bytes = int(edge.get("expectedBytes", 0) or 0)
        crossing_bytes += edge_bytes
        if target_rps <= 0.0:
            continue
        _rtt_ms, bandwidth_mbps = pair_network(producer_provider, consumer_provider, network)
        edge_rate_mbps = (edge_bytes * target_rps * 8.0) / 1_000_000.0
        utilization = edge_rate_mbps / bandwidth_mbps if bandwidth_mbps > 0.0 else 1.0
        max_link_utilization = max(max_link_utilization, utilization)
        transfer = edge_transfer_ms(edge, placement, profiles, network)
        if utilization < 0.95:
            rate_pressure += (utilization * transfer) / max(1e-6, 1.0 - utilization)
        else:
            rate_pressure += transfer * (20.0 + max(0.0, utilization - 0.95) * 100.0)

    return {
        "dependencyCrossingBytes": float(crossing_bytes),
        "dependencyByteRateMbps": (crossing_bytes * target_rps * 8.0) / 1_000_000.0,
        "dependencyMaxLinkUtilization": max_link_utilization,
        "dependencyRatePressureMs": rate_pressure,
    }


def critical_path_ms(candidate: dict[str, Any],
                     role_durations: dict[str, float],
                     profiles: dict[str, dict[str, Any]],
                     network: dict[str, Any]) -> float:
    placement = candidate["rolePlacement"]
    active_roles = [str(role) for role in candidate["activeRoles"]]
    dependencies = list(candidate.get("dependencies") or [])
    if not dependencies:
        providers = {
            str(item["provider"])
            for item in compute_work_items(candidate, profiles)
        }
        if len(providers) == 1:
            return sum(role_durations.values())
        return max(role_durations.values()) if role_durations else 0.0

    incoming: dict[str, list[dict[str, Any]]] = {role: [] for role in active_roles}
    for edge in dependencies:
        incoming.setdefault(str(edge["consumer"]), []).append(edge)
    visiting: set[str] = set()
    memo: dict[str, float] = {}

    def finish(role: str) -> float:
        if role in memo:
            return memo[role]
        if role in visiting:
            raise RuntimeError(f"dependency cycle includes role {role}")
        visiting.add(role)
        predecessor_ms = 0.0
        for edge in incoming.get(role, []):
            producer = str(edge["producer"])
            predecessor_ms = max(
                predecessor_ms,
                finish(producer) + edge_transfer_ms(edge, placement, profiles, network))
        visiting.remove(role)
        memo[role] = predecessor_ms + role_durations.get(role, 0.0)
        return memo[role]

    return max((finish(role) for role in active_roles), default=0.0)


def score_candidate(candidate: dict[str, Any],
                    edges: list[dict[str, Any]],
                    profiles: dict[str, dict[str, Any]],
                    network: dict[str, Any],
                    workload_concurrency: int = 1,
                    target_rps: float = 0.0) -> dict[str, float]:
    placement = candidate["rolePlacement"]
    compute_total = 0.0
    queue_total = 0.0
    role_durations: dict[str, float] = {}
    provider_work: dict[str, float] = {}
    compute_work = compute_work_items(candidate, profiles)
    for item in compute_work:
        role = str(item["role"])
        provider = str(item.get("provider") or provider_for(role, placement, profiles))
        duration = role_compute_ms(role, provider, profiles)
        compute_total += duration
        role_durations[role] = max(role_durations.get(role, 0.0), duration)
        provider_work[provider] = provider_work.get(provider, 0.0) + duration
        queue_total += queue_ms(provider, profiles)

    transfer_total = 0.0
    for edge in candidate["dependencies"]:
        transfer_total += edge_transfer_ms(edge, placement, profiles, network)

    critical_total = critical_path_ms(candidate, role_durations, profiles, network)
    provider_bottleneck = max(provider_work.values(), default=0.0)
    concurrency = max(1, int(workload_concurrency))
    concurrency_queue = max(0, concurrency - 1) * provider_bottleneck * 0.5
    provider_count = len(provider_work)
    roles_per_provider: dict[str, int] = {}
    for item in compute_work:
        provider = str(item.get("provider") or provider_for(str(item["role"]), placement, profiles))
        roles_per_provider[provider] = roles_per_provider.get(provider, 0) + 1
    max_roles_per_provider = max(roles_per_provider.values(), default=0)
    mean_role_compute = compute_total / len(compute_work) if compute_work else 0.0
    provider_ready_queue = (
        max(0, max_roles_per_provider - 1) *
        mean_role_compute *
        (1.0 - (1.0 / concurrency)) *
        0.25
    )
    average_provider_work = compute_total / provider_count if provider_count else 0.0
    provider_imbalance = max(0.0, provider_bottleneck - average_provider_work)
    target_rps = max(0.0, float(target_rps))
    provider_max_utilization = (provider_bottleneck * target_rps) / 1000.0
    provider_capacity_queue = 0.0
    if provider_max_utilization > 0.0:
        if provider_max_utilization < 0.95:
            provider_capacity_queue = (
                provider_max_utilization * provider_bottleneck /
                max(1e-6, 1.0 - provider_max_utilization)
            )
        else:
            provider_capacity_queue = provider_bottleneck * (
                20.0 + max(0.0, provider_max_utilization - 0.95) * 100.0)
    dependency_pressure = crossing_dependency_pressure(
        candidate,
        profiles,
        network,
        target_rps)
    total = (
        critical_total +
        concurrency_queue +
        queue_total +
        provider_capacity_queue +
        dependency_pressure["dependencyRatePressureMs"]
    )
    return {
        "computeMs": round(compute_total, 3),
        "transferMs": round(transfer_total, 3),
        "criticalPathMs": round(critical_total, 3),
        "providerCount": round(float(provider_count), 3),
        "maxRolesPerProvider": round(float(max_roles_per_provider), 3),
        "providerBottleneckMs": round(provider_bottleneck, 3),
        "providerWorkImbalanceMs": round(provider_imbalance, 3),
        "providerReadyQueuePressureMs": round(provider_ready_queue, 3),
        "targetRps": round(target_rps, 3),
        "providerMaxUtilization": round(provider_max_utilization, 6),
        "providerCapacityQueuePressureMs": round(provider_capacity_queue, 3),
        "dependencyCrossingBytes": round(dependency_pressure["dependencyCrossingBytes"], 3),
        "dependencyByteRateMbps": round(dependency_pressure["dependencyByteRateMbps"], 6),
        "dependencyMaxLinkUtilization": round(
            dependency_pressure["dependencyMaxLinkUtilization"], 6),
        "dependencyRatePressureMs": round(dependency_pressure["dependencyRatePressureMs"], 3),
        "queueMs": round(queue_total, 3),
        "concurrencyQueueMs": round(concurrency_queue, 3),
        "totalEstimatedMs": round(total, 3),
    }


def current_placement(profiles: dict[str, dict[str, Any]]) -> dict[str, str]:
    return {role: str(profiles[role]["provider"]) for role in ROLES}


def candidate_templates(edges: list[dict[str, Any]],
                        profiles: dict[str, dict[str, Any]],
                        runtime_candidate: str) -> list[dict[str, Any]]:
    current = current_placement(profiles)
    single_provider = {
        role: str(profiles[ROLE_BACKBONE]["provider"])
        for role in ROLES
    }
    merge_centered = dict(current)
    merge_centered[ROLE_HEAD0] = str(profiles[ROLE_MERGE]["provider"])
    merge_centered[ROLE_HEAD1] = str(profiles[ROLE_MERGE]["provider"])
    replicated_edges = [
        edge for edge in edges
        if edge["producer"] in {ROLE_HEAD0, ROLE_HEAD1}
    ]
    replicated_compute = [
        {"role": ROLE_BACKBONE, "provider": current[ROLE_HEAD0]},
        {"role": ROLE_BACKBONE, "provider": current[ROLE_HEAD1]},
        {"role": ROLE_HEAD0, "provider": current[ROLE_HEAD0]},
        {"role": ROLE_HEAD1, "provider": current[ROLE_HEAD1]},
        {"role": ROLE_MERGE, "provider": current[ROLE_MERGE]},
    ]
    pipeline_edges = [
        {
            "scope": "backbone-to-head0",
            "producer": ROLE_BACKBONE,
            "consumer": ROLE_HEAD0,
            "expectedBytes": 256,
            "expectedSegments": 1,
        },
        {
            "scope": "head0-to-head1",
            "producer": ROLE_HEAD0,
            "consumer": ROLE_HEAD1,
            "expectedBytes": 128,
            "expectedSegments": 1,
        },
        {
            "scope": "head1-to-merge",
            "producer": ROLE_HEAD1,
            "consumer": ROLE_MERGE,
            "expectedBytes": 128,
            "expectedSegments": 1,
        },
    ]
    return [
        {
            "id": "shared-backbone-current",
            "label": "current shared backbone with two head shards",
            "supportedByCurrentRuntime": True,
            "activeRoles": list(ROLES),
            "rolePlacement": current,
            "dependencies": list(edges),
            "reason": (
                "Uses the current executable NativeTracer role graph and real "
                "NDNSF large-data dependency exchange."
            ),
        },
        {
            "id": "single-provider-serial",
            "label": "single provider serial execution",
            "supportedByCurrentRuntime": True,
            "activeRoles": list(ROLES),
            "rolePlacement": single_provider,
            "dependencies": [],
            "reason": (
                "Runs every NativeTracer role on one provider. When selected as "
                "the active runtime candidate, one provider advertises all roles "
                "and the NDNSF collaboration selector assigns all roles to it."
            ),
        },
        {
            "id": "replicated-backbone-estimated",
            "label": "replicated backbone at both head providers",
            "supportedByCurrentRuntime": False,
            "activeRoles": [ROLE_HEAD0, ROLE_HEAD1, ROLE_MERGE],
            "computeWork": replicated_compute,
            "rolePlacement": current,
            "dependencies": replicated_edges,
            "reason": (
                "Estimates a layout that saves backbone-to-head transfer by "
                "duplicating backbone compute near each head shard."
            ),
        },
        {
            "id": "pipeline-stages-estimated",
            "label": "linear pipeline across providers",
            "supportedByCurrentRuntime": False,
            "activeRoles": list(ROLES),
            "rolePlacement": current,
            "dependencies": pipeline_edges,
            "reason": (
                "Estimates a stage pipeline where each role hands one output to "
                "the next role."
            ),
        },
        {
            "id": "merge-centered-heads-estimated",
            "label": "head shards colocated with merge",
            "supportedByCurrentRuntime": False,
            "activeRoles": list(ROLES),
            "rolePlacement": merge_centered,
            "dependencies": list(edges),
            "reason": (
                "Keeps a remote backbone but moves both head shards near merge "
                "to reduce final aggregation transfer."
            ),
        },
    ]


def manifest_source_model(service_manifest: dict[str, Any]) -> str:
    values = set()
    metadata = service_manifest.get("metadata", {})
    if isinstance(metadata, dict) and metadata.get("sourceModel"):
        values.add(str(metadata["sourceModel"]))
    for artifact in service_manifest.get("artifacts", []):
        item_metadata = artifact.get("metadata", {})
        if isinstance(item_metadata, dict) and item_metadata.get("sourceModel"):
            values.add(str(item_metadata["sourceModel"]))
    if len(values) != 1:
        raise RuntimeError(f"expected exactly one sourceModel, observed {sorted(values)}")
    return next(iter(values))


def validate_minimal_qwen(service_manifest: dict[str, Any]) -> tuple[bool, list[str]]:
    filenames = sorted(str(item.get("filename", "")) for item in service_manifest.get("artifacts", []))
    expected = sorted([
        "qwen-native-tracer-backbone.onnx",
        "qwen-native-tracer-head0.onnx",
        "qwen-native-tracer-head1.onnx",
        "qwen-native-tracer-merge.onnx",
    ])
    source = manifest_source_model(service_manifest)
    return filenames == expected and source == SOURCE_MODEL, filenames


def apply_manifest_profile_metadata(profiles: dict[str, dict[str, Any]],
                                    service_manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    result = {role: dict(values) for role, values in profiles.items()}
    metadata = dict(service_manifest.get("metadata", {}) or {})
    manifest_delay = float(metadata.get("roleExecutionDelayMs", 0.0) or 0.0)
    if manifest_delay <= 0.0:
        return result
    for role, profile in result.items():
        if "roleExecutionDelayMs" in profile:
            continue
        base = float(profile.get("baseRoleComputeMs", profile.get("roleComputeMs", 0.0)) or 0.0)
        profile["baseRoleComputeMs"] = base
        profile["roleExecutionDelayMs"] = manifest_delay
        profile["roleComputeMs"] = base + manifest_delay
    return result


def build_evidence(plan: dict[str, Any],
                   manifest: dict[str, Any],
                   profiles: dict[str, dict[str, Any]],
                   network: dict[str, Any],
                   runtime_candidate: str,
                   workload_concurrency: int = 1,
                   target_rps: float = 0.0) -> dict[str, Any]:
    service_plan = service_from_plan(plan)
    service_manifest = service_from_manifest(manifest)
    profiles = apply_manifest_profile_metadata(profiles, service_manifest)
    edges = dependency_edges(service_plan)
    model_unchanged, artifact_filenames = validate_minimal_qwen(service_manifest)
    candidates = candidate_templates(edges, profiles, runtime_candidate)
    for candidate in candidates:
        candidate["cost"] = score_candidate(
            candidate,
            edges,
            profiles,
            network,
            workload_concurrency,
            target_rps)
        candidate["estimatedOnly"] = not bool(candidate["supportedByCurrentRuntime"])

    executable = [item for item in candidates if item["supportedByCurrentRuntime"]]
    if not executable:
        raise RuntimeError("no executable NativeTracer candidate available")
    requested = next(
        (item for item in executable if item["id"] == runtime_candidate),
        None)
    selected = requested or min(executable, key=lambda item: item["cost"]["totalEstimatedMs"])
    recommended = min(executable, key=lambda item: item["cost"]["totalEstimatedMs"])
    best_estimated = min(candidates, key=lambda item: item["cost"]["totalEstimatedMs"])

    metadata = dict(service_manifest.get("metadata", {}) or {})
    evidence = {
        "contractVersion": CONTRACT_VERSION,
        "runtimeCandidate": runtime_candidate,
        "workload": {
            "concurrency": max(1, int(workload_concurrency)),
            "targetRps": max(0.0, float(target_rps)),
        },
        "service": SERVICE,
        "model": service_manifest.get("model", MODEL),
        "sourceModel": manifest_source_model(service_manifest),
        "modelUnchanged": model_unchanged,
        "artifactFilenames": artifact_filenames,
        "compatibility": {
            "modelFamily": service_plan.get("modelFamily"),
            "modelFormat": service_plan.get("modelFormat"),
            "plannerKind": service_plan.get("plannerKind"),
            "runtimeBackend": service_plan.get("runtimeBackend"),
            "metadataModelFamily": metadata.get("modelFamily"),
            "metadataPlannerKind": metadata.get("plannerKind"),
        },
        "providerProfiles": [
            {"role": role, **dict(profiles[role])}
            for role in ROLES
        ],
        "networkProfile": network,
        "candidateCount": len(candidates),
        "candidates": candidates,
        "selectedCandidate": {
            "id": selected["id"],
            "label": selected["label"],
            "totalEstimatedMs": selected["cost"]["totalEstimatedMs"],
            "supportedByCurrentRuntime": selected["supportedByCurrentRuntime"],
        },
        "plannerRecommendedCandidate": {
            "id": recommended["id"],
            "label": recommended["label"],
            "totalEstimatedMs": recommended["cost"]["totalEstimatedMs"],
            "supportedByCurrentRuntime": recommended["supportedByCurrentRuntime"],
        },
        "bestEstimatedCandidate": {
            "id": best_estimated["id"],
            "label": best_estimated["label"],
            "totalEstimatedMs": best_estimated["cost"]["totalEstimatedMs"],
            "supportedByCurrentRuntime": best_estimated["supportedByCurrentRuntime"],
        },
        "selectionRule": (
            "selectedCandidate is the requested runtime candidate for reproducible "
            "experiments; plannerRecommendedCandidate is the lowest totalEstimatedMs "
            "executable candidate for the declared workload concurrency; unsupported "
            "layouts are recorded as estimated alternatives."
        ),
    }
    if not model_unchanged:
        raise RuntimeError("NativeTracer minimal Qwen artifact set changed")
    return evidence


def write_csv(path: Path, evidence: dict[str, Any]) -> None:
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=[
                "id",
                "label",
                "supportedByCurrentRuntime",
                "computeMs",
                "transferMs",
                "criticalPathMs",
                "providerCount",
                "maxRolesPerProvider",
                "providerBottleneckMs",
                "providerWorkImbalanceMs",
                "providerReadyQueuePressureMs",
                "targetRps",
                "providerMaxUtilization",
                "providerCapacityQueuePressureMs",
                "dependencyCrossingBytes",
                "dependencyByteRateMbps",
                "dependencyMaxLinkUtilization",
                "dependencyRatePressureMs",
                "queueMs",
                "concurrencyQueueMs",
                "totalEstimatedMs",
                "selected",
                "recommended",
            ])
        writer.writeheader()
        selected_id = evidence["selectedCandidate"]["id"]
        for candidate in evidence["candidates"]:
            cost = candidate["cost"]
            writer.writerow({
                "id": candidate["id"],
                "label": candidate["label"],
                "supportedByCurrentRuntime": candidate["supportedByCurrentRuntime"],
                "computeMs": cost["computeMs"],
                "transferMs": cost["transferMs"],
                "criticalPathMs": cost["criticalPathMs"],
                "providerCount": cost["providerCount"],
                "maxRolesPerProvider": cost["maxRolesPerProvider"],
                "providerBottleneckMs": cost["providerBottleneckMs"],
                "providerWorkImbalanceMs": cost["providerWorkImbalanceMs"],
                "providerReadyQueuePressureMs": cost["providerReadyQueuePressureMs"],
                "targetRps": cost["targetRps"],
                "providerMaxUtilization": cost["providerMaxUtilization"],
                "providerCapacityQueuePressureMs": cost["providerCapacityQueuePressureMs"],
                "dependencyCrossingBytes": cost["dependencyCrossingBytes"],
                "dependencyByteRateMbps": cost["dependencyByteRateMbps"],
                "dependencyMaxLinkUtilization": cost["dependencyMaxLinkUtilization"],
                "dependencyRatePressureMs": cost["dependencyRatePressureMs"],
                "queueMs": cost["queueMs"],
                "concurrencyQueueMs": cost["concurrencyQueueMs"],
                "totalEstimatedMs": cost["totalEstimatedMs"],
                "selected": candidate["id"] == selected_id,
                "recommended": candidate["id"] == evidence["plannerRecommendedCandidate"]["id"],
            })


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--csv-out", default="")
    parser.add_argument("--provider-profiles-json", default="")
    parser.add_argument("--network-profile-json", default="")
    parser.add_argument("--runtime-candidate", default="shared-backbone-current",
                        choices=["shared-backbone-current", "single-provider-serial"])
    parser.add_argument("--workload-concurrency", type=int, default=1)
    parser.add_argument("--target-rps", type=float, default=0.0)
    args = parser.parse_args(argv)
    if args.workload_concurrency <= 0:
        raise SystemExit("--workload-concurrency must be positive")
    if args.target_rps < 0.0:
        raise SystemExit("--target-rps must be non-negative")

    plan_path = Path(args.plan)
    manifest_path = Path(args.manifest)
    out_path = Path(args.out)
    csv_path = Path(args.csv_out) if args.csv_out else out_path.with_suffix(".csv")

    evidence = build_evidence(
        load_json(plan_path),
        load_json(manifest_path),
        load_profiles(args.provider_profiles_json),
        load_network(args.network_profile_json),
        args.runtime_candidate,
        args.workload_concurrency,
        args.target_rps,
    )
    out_path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    write_sha256_sidecar(out_path)
    write_csv(csv_path, evidence)
    write_sha256_sidecar(csv_path)
    print("NDNSF_DI_NATIVE_TRACER_OPTIMIZATION_OK")
    print("contract:", evidence["contractVersion"])
    print("candidates:", evidence["candidateCount"])
    print("selected:", evidence["selectedCandidate"]["id"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
