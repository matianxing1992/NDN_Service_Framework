#!/usr/bin/env python3
"""Execute the deterministic Spec 105 MiniNDN host-telemetry plan cells."""

from __future__ import annotations

import json

from ndnsf import to_plain
from ndnsf_distributed_inference.runtime_v1 import (
    MeasuredTelemetrySnapshotV1,
    PlanFeasibilityRequirementsV1,
    evaluate_plan_feasibility,
)


BASE = {
    "provider_name": "/provider/A",
    "provider_boot_id": "boot-a",
    "sequence": 7,
    "resource_sequence": 7,
    "measured_at_ms": 1_000,
    "sampled_at_ms": 1_000,
    "source": "controlled-minindn-host-probe",
    "status": "measured",
    "host_total_memory_bytes": 10_000,
    "host_available_memory_bytes": 5_000,
    "process_rss_bytes": 1_000,
    "ready_queue": 1,
    "waiting_dependencies": 1,
    "active_workers": 1,
    "worker_count": 4,
    "evidence_epoch": 3,
    "runner_kind": "onnxruntime-cpu",
    "runtime_version": "ort-1.26",
    "model_digest": "sha256:model",
    "plan_digest": "sha256:plan",
    "artifact_digests": {"/Stage/0": "sha256:stage0"},
    "device_id": "cpu0",
    "device_kind": "cpu",
    "membership_version": "members-v1",
    "network_profile_version": "network-v1",
    "cache_version": "cache-v1",
}

REQUIREMENTS = PlanFeasibilityRequirementsV1(
    expected_provider_name="/provider/A",
    expected_provider_boot_id="boot-a",
    minimum_evidence_epoch=3,
    expected_runner_kind="onnxruntime-cpu",
    expected_runtime_version="ort-1.26",
    expected_model_digest="sha256:model",
    expected_plan_digest="sha256:plan",
    expected_artifact_digests={"/Stage/0": "sha256:stage0"},
    expected_device_id="cpu0",
    maximum_telemetry_age_ms=2_000,
    minimum_free_host_memory_bytes=4_000,
    maximum_ready_queue=2,
    maximum_waiting_dependencies=2,
    maximum_active_workers=2,
    expected_membership_version="members-v1",
    expected_network_profile_version="network-v1",
    expected_cache_version="cache-v1",
)

CELLS = (
    ("fresh", {}, 1_100, "reuse"),
    ("stale", {}, 3_001, "defer"),
    ("memory-pressure", {"host_available_memory_bytes": 1_000}, 1_100, "defer"),
    ("queue-pressure", {"ready_queue": 9}, 1_100, "defer"),
    ("membership-mismatch", {"membership_version": "members-v2"}, 1_100, "replan"),
    ("device-mismatch", {"device_id": "cpu1"}, 1_100, "reject"),
)


def main() -> int:
    cells = []
    for name, changes, at_ms, expected in CELLS:
        snapshot = MeasuredTelemetrySnapshotV1(**{**BASE, **changes})
        decision = evaluate_plan_feasibility(snapshot, REQUIREMENTS, at_ms=at_ms)
        if decision.decision != expected:
            raise RuntimeError(
                f"{name}: expected {expected}, observed {decision.decision}")
        cells.append({
            "name": name,
            "atMs": at_ms,
            "expectedDecision": expected,
            "observedDecision": decision.decision,
            "reasonCodes": list(decision.reason_codes),
            "predicates": to_plain(decision.predicates),
        })
    print(json.dumps({
        "schema": "ndnsf-di-spec105-telemetry-plan-cells-v1",
        "scope": "MiniNDN controlled host telemetry; no physical GPU evidence",
        "physicalGpuEvidence": False,
        "cells": cells,
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
