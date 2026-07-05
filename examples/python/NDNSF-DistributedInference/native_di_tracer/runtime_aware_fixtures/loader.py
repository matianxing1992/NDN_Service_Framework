#!/usr/bin/env python3
"""Fixture loader for NDNSF-DI runtime-aware planner examples."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from ndnsf_distributed_inference.runtime_v1 import (
    GenericAckMetadata,
    GenericAdmissionLease,
    GenericProviderRuntimeHint,
    ProviderNetworkMatrix,
)


FIXTURE_DIR = Path(__file__).resolve().parent


def read_fixture(name: str) -> dict[str, Any]:
    return json.loads((FIXTURE_DIR / name).read_text(encoding="utf-8"))


def load_provider_ack_metadata(name: str = "provider_fragments.json") -> dict[str, GenericAckMetadata]:
    payload = read_fixture(name)
    result: dict[str, GenericAckMetadata] = {}
    for provider in payload.get("providers", []):
        generic_hint = GenericProviderRuntimeHint.from_dict(dict(provider["genericHint"]))
        lease_payloads = provider.get("leaseOffers", [])
        metadata = GenericAckMetadata(
            provider_runtime_hint=generic_hint,
            lease_offers=tuple(GenericAdmissionLease.from_dict(dict(item)) for item in lease_payloads),
            service_payload_schema="ndnsf-di-runtime-ack-v1",
            service_payload=dict(provider["diState"]),
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
