#!/usr/bin/env python3
"""Fixture loader for NDNSF-DI runtime-aware planner examples."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from ndnsf_distributed_inference.runtime_v1 import (
    GenericAckMetadata,
    GenericAdmissionLease,
    FragmentResidency,
    ModelFragmentKey,
    ProviderFragmentInventoryManager,
    ProviderNetworkMatrix,
)


FIXTURE_DIR = Path(__file__).resolve().parent


def read_fixture(name: str) -> dict[str, Any]:
    return json.loads((FIXTURE_DIR / name).read_text(encoding="utf-8"))


def load_provider_ack_metadata(name: str = "provider_fragments.json") -> dict[str, GenericAckMetadata]:
    payload = read_fixture(name)
    result: dict[str, GenericAckMetadata] = {}
    for provider in payload.get("providers", []):
        provider_name = str(provider["providerName"])
        hint = dict(provider.get("genericHint", {}))
        di_state = dict(provider.get("diState", {}))
        manager = ProviderFragmentInventoryManager(
            provider_name,
            supported_backends=di_state.get("supportedBackends", ()),
            free_gpu_memory_mb=di_state.get("freeGpuMemoryMb", 0.0),
            free_cpu_memory_mb=di_state.get("freeCpuMemoryMb", 0.0),
            active_role_count=di_state.get("activeRoleCount", 0),
            queue_length=hint.get("queueLength", di_state.get("queueLength", 0)),
            estimated_queue_wait_ms=hint.get(
                "estimatedQueueWaitMs",
                di_state.get("estimatedQueueWaitMs", 0.0)),
            confidence=hint.get("confidence", di_state.get("confidence", 1.0)),
        )
        for state in di_state.get("fragmentStates", []):
            fragment_key = ModelFragmentKey.from_dict(dict(state["fragmentKey"]))
            disk_path = state.get("diskPath", state.get("disk_path", ""))
            if disk_path and not Path(str(disk_path)).is_absolute():
                disk_path = FIXTURE_DIR / str(disk_path)
            manager.register_fragment(
                fragment_key,
                disk_path=disk_path,
                memory_footprint_mb=state.get("memoryFootprintMb", 0.0),
                repo_available=(
                    FragmentResidency(state.get("residency", FragmentResidency.MISSING.value))
                    == FragmentResidency.REPO_AVAILABLE
                ),
                pinned=state.get("pinned", False),
                confidence=state.get("confidence", 1.0),
            )
            residency = FragmentResidency(state.get("residency", FragmentResidency.MISSING.value))
            if residency == FragmentResidency.CPU_RESIDENT:
                manager.mark_cpu_resident(fragment_key)
            elif residency == FragmentResidency.GPU_LOADED:
                manager.mark_gpu_loaded(fragment_key)
        lease_payloads = provider.get("leaseOffers", [])
        metadata = manager.ack_metadata(
            lease_offers=tuple(GenericAdmissionLease.from_dict(dict(item)) for item in lease_payloads),
        )
        result[str(provider["providerName"])] = metadata
    return result


def load_provider_network_matrix(name: str = "provider_network_matrix.json") -> ProviderNetworkMatrix:
    return ProviderNetworkMatrix.from_dict(read_fixture(name))


def load_multi_user_requests(name: str = "multi_user_requests.json") -> list[dict[str, Any]]:
    return list(read_fixture(name).get("requests", []))


if __name__ == "__main__":
    print(json.dumps({
        "providers": sorted(load_provider_ack_metadata()),
        "requests": len(load_multi_user_requests()),
        "networkMetrics": len(load_provider_network_matrix().metrics),
    }, indent=2, sort_keys=True))
