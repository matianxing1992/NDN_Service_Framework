#!/usr/bin/env python3
"""Multi-user deployment lifecycle experiment — tests Merge Provider + lease + evict guard.

Does NOT deploy (needs artifact publishing); instead injects a deployment via
coordination intent, then exercises discover/acquire/release/evict through the
Merge Provider (coordinator).

Run via MiniNDN harness with --lifecycle-experiment flag.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "pythonWrapper"))
sys.path.insert(0, str(REPO / "NDNSF-DistributedInference"))
sys.path.insert(0, str(REPO / "Experiments"))

from ndnsf import (
    COORDINATION_ADVISORY_SERVICE,
    CoordinationIntent,
    CoordinationServiceClient,
    ServiceUser,
)

SERVICE = "/Inference/NativeTracer"
GROUP = "/NDNSF-DI/Tracer/group"
CONTROLLER = "/NDNSF-DI/Tracer/controller"
USER = "/NDNSF-DI/Tracer/user"


def build_parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", default="/tmp/ndnsf-lifecycle-test")
    p.add_argument("--requests", type=int, default=2)
    p.add_argument("--permission-wait-ms", type=int, default=5000)
    return p


def log(step, payload):
    print(f"NDNSF_DI_LIFECYCLE {step} " + json.dumps(payload, sort_keys=True), flush=True)


def _send_intent(user, purpose, payload, ack_ms=5000, timeout_ms=10000):
    client = CoordinationServiceClient(
        user, service_name=COORDINATION_ADVISORY_SERVICE,
        ack_timeout_ms=ack_ms, timeout_ms=timeout_ms)
    intent = CoordinationIntent(
        intent_id=f"{purpose}-{int(time.time()*1000)}",
        request_id=f"lifecycle-{purpose}",
        requester_name=USER, service_name=COORDINATION_ADVISORY_SERVICE,
        purpose=purpose, payload=payload)
    return client.request([intent])


def main():
    args = build_parser().parse_args()
    out_dir = Path(args.out).expanduser().resolve()

    user = ServiceUser(group=GROUP, controller=CONTROLLER, user=USER,
                       trust_schema="examples/trust-schema.conf",
                       permission_wait_ms=args.permission_wait_ms,
                       serve_certificates=True)
    allowed = [e.service for e in user.get_allowed_services()]
    if SERVICE not in allowed:
        log("FAIL", {"step": "permissions", "allowed": allowed})
        return 1
    log("OK", {"step": "permissions", "count": len(allowed)})

    # ── Step 1: discover — may already have deployments via SVS gossip ──
    deps = user.discover_deployments(SERVICE)
    log("DISCOVER_INITIAL", {"count": len(deps),
         "statuses": [d.get("status") for d in deps]})

    dep_id = "dep-test-lifecycle"

    # ── Step 2: inject deployment via coordinator intent ──
    resp = _send_intent(user, "deploy", {
        "deploymentId": dep_id,
        "planId": "test-plan",
        "roleAssignments": {
            "/Backbone": [{"provider": "/NDNSF-DI/Tracer/provider/backbone"}],
            "/Head/Shard/0": [{"provider": "/NDNSF-DI/Tracer/provider/head0"}],
        },
    })
    log("INJECT_DEPLOYMENT", {"deploymentId": dep_id, "suggestions": len(resp.suggestions)})

    # ── Step 3: discover — now has deployment ──
    time.sleep(1.0)  # NDNSD sync delay
    deps = user.discover_deployments(SERVICE)
    log("DISCOVER_ONE", {"count": len(deps), "status": deps[0].get("status") if deps else "NONE"})

    # ── Step 4: acquire lease (through Merge Provider) ──
    resp = _send_intent(user, "acquire-lease", {"deploymentId": dep_id})
    lease_payload = resp.suggestions[0].payload if resp.suggestions else {}
    lease_id = lease_payload.get("leaseId", "")
    ref = lease_payload.get("refCount", 0)
    log("ACQUIRE_LEASE", {"leaseId": lease_id, "deploymentId": dep_id,
                           "status": lease_payload.get("status"), "refCount": ref})

    # ── Step 5: acquire second lease (ref_count should be 2) ──
    resp2 = _send_intent(user, "acquire-lease", {"deploymentId": dep_id})
    lp2 = resp2.suggestions[0].payload if resp2.suggestions else {}
    log("ACQUIRE_LEASE_2", {"leaseId": lp2.get("leaseId"), "refCount": lp2.get("refCount")})

    # ── Step 6: evict — should be REJECTED (ref_count > 0) ──
    evict = user.evict_deployment(dep_id)
    log("EVICT_GUARDED", {"status": evict.get("status"),
                          "reason": evict.get("reason", ""),
                          "refCount": evict.get("refCount", 0)})
    ok_guard = evict.get("status") == "REJECTED"
    log("ASSERT", {"check": "evict_guard_rejected", "pass": ok_guard})

    # ── Step 7: release both leases ──
    _send_intent(user, "release-lease", {"leaseId": lease_id, "deploymentId": dep_id})
    _send_intent(user, "release-lease", {"leaseId": lp2.get("leaseId"), "deploymentId": dep_id})
    log("RELEASE_BOTH", {"done": True})

    # ── Step 8: evict — should succeed (ref_count = 0) ──
    evict2 = user.evict_deployment(dep_id)
    ok_evict = evict2.get("status") == "DISK_RESIDENT"
    log("EVICT_SUCCESS", {"status": evict2.get("status"), "pass": ok_evict})

    log("COMPLETE", {"status": "SUCCESS" if ok_guard and ok_evict else "PARTIAL",
                     "evictGuard": ok_guard, "evictSuccess": ok_evict})
    return 0 if ok_guard and ok_evict else 1


if __name__ == "__main__":
    raise SystemExit(main())
