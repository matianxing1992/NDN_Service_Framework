#!/usr/bin/env python3
"""Minimal real-network Spec170 V3 CPU User.

The user closes the ACK snapshot, seals a provider-specific V3 Selection
projection for four roles, and waits for the merge Provider's final Response.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import time

from ndnsf import CollaborationRole, ServiceUser
from ndnsf_distributed_inference.sdk.placement import (
    DeviceTopologyProfile,
    ExecutionDisposition,
    PlacementProposalV3,
    PlanSealerV3,
    ProviderOfferV3,
    ProviderPlanningViewV3,
    ProviderSelectionProjectionV3,
    RoleAssemblySpec,
    UNBOUND_GRAPH_DIGEST_V3,
    canonical_digest,
)


MODEL_DIGEST = "sha256:" + "1" * 64
GRAPH_DIGEST = "sha256:" + "2" * 64
ROLES = ("/Backbone", "/Head/Shard/0", "/Head/Shard/1", "/Merge")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--group", required=True)
    parser.add_argument("--controller", required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--trust-schema", required=True)
    parser.add_argument("--bootstrap-token", required=True)
    parser.add_argument("--service", default="/Inference/NativeTracer")
    parser.add_argument("--ack-timeout-ms", type=int, default=1500)
    parser.add_argument("--timeout-ms", type=int, default=15000)
    parser.add_argument("--request-id", default="spec170-v3-cpu-network")
    parser.add_argument("--device", choices=("cpu", "cuda:0"), default="cpu")
    args = parser.parse_args()

    signing_key = b"spec170-v3-network-diagnostic-key"
    expected_backend = (
        "onnxruntime-cuda" if args.device.startswith("cuda:") else "cpu")

    def verify_offer(offer: ProviderOfferV3) -> bool:
        expected = hmac.new(
            signing_key, offer.digest().encode("utf-8"), hashlib.sha256
        ).hexdigest()
        return hmac.compare_digest(expected, offer.signature)

    user = ServiceUser(
        group=args.group,
        controller=args.controller,
        user=args.user,
        trust_schema=args.trust_schema,
        permission_wait_ms=5000,
        bootstrap_token=args.bootstrap_token,
        adaptive_admission=False,
    )
    deadline_ms = int(time.time() * 1000) + args.timeout_ms
    payload = json.dumps({
        "schema": "ndnsf-di-request-envelope-v2",
        "placementProfile": "DI_PLACEMENT_V3",
        "requestId": args.request_id,
        "attempt": 1,
        "deadlineMs": deadline_ms,
        "model": {"identityDigest": MODEL_DIGEST},
        "task": {"name": "spec170-v3-cpu-control-gate"},
    }, sort_keys=True, separators=(",", ":")).encode("utf-8")

    user.start()
    try:
        allowed = user.get_allowed_services()
        if not any(str(item.service) == args.service for item in allowed):
            raise RuntimeError("requested service is absent from user permission")
        print(
            f"SPEC170_V3_USER_PERMISSION service={args.service} "
            f"allowed={len(allowed)}",
            flush=True,
        )
        collaboration = user.begin_collaboration(
            args.service,
            payload,
            mode="DEFERRED",
            ack_timeout_ms=args.ack_timeout_ms,
            timeout_ms=args.timeout_ms,
            request_id=args.request_id,
            fail_fast_terminal_selection=True,
        )
        closed = collaboration.acks_closed()
        print(
            f"SPEC170_V3_USER_ACK_CLOSED requestId={closed.request_id} "
            f"ackCount={len(closed.candidates)} digest={closed.digest}",
            flush=True,
        )
        views = {}
        for candidate in closed.candidates:
            if not candidate.status:
                continue
            offer = ProviderOfferV3.from_bytes(bytes(candidate.payload))
            if not verify_offer(offer):
                raise RuntimeError(f"invalid V3 offer signature: {offer.provider}")
            view = ProviderPlanningViewV3.from_offer(
                offer,
                request_id=args.request_id,
                model_digest=MODEL_DIGEST,
                graph_digest=GRAPH_DIGEST,
                now_ms=int(time.time() * 1000),
                deadline_ms=deadline_ms,
                verify_signature=verify_offer,
            )
            if tuple(view.topology.devices) != (args.device,):
                raise RuntimeError(
                    f"provider {offer.provider} advertised devices "
                    f"{view.topology.devices}, expected {(args.device,)}")
            if expected_backend not in view.backends:
                raise RuntimeError(
                    f"provider {offer.provider} lacks backend "
                    f"{expected_backend}: {view.backends}")
            views[offer.provider] = view

        role_provider = {}
        role_specs = []
        for index, role in enumerate(ROLES):
            matches = [
                provider for provider, view in views.items()
                if role in view.accepted_roles
                and view.execution_disposition
                == ExecutionDisposition.ACCEPT_WITH_PREPARATION
            ]
            if len(matches) != 1:
                raise RuntimeError(f"expected one V3 Provider for {role}, got {matches}")
            provider = matches[0]
            role_provider[role] = provider
            digest = "sha256:" + hashlib.sha256(role.encode()).hexdigest()
            role_specs.append(RoleAssemblySpec(
                role=role,
                rank=0,
                layer_begin=index,
                layer_end=index + 1,
                recipe_digest=digest,
                artifact_digest=digest,
                backend=expected_backend,
                device_set=(args.device,),
            ))

        proposal = PlacementProposalV3(
            request_id=args.request_id,
            attempt=1,
            model_digest=MODEL_DIGEST,
            graph_digest=GRAPH_DIGEST,
            roles=tuple(role_specs),
            provider_by_role=role_provider,
            dependencies=(),
            candidate_digest=canonical_digest({"roles": list(ROLES)}),
            strategy_name="spec170-v3-device-gate",
            strategy_version="1",
            strategy_state_digest=canonical_digest({"device": args.device}),
        )
        request = {
            "request_id": args.request_id,
            "attempt": 1,
            "ack_closed_digest": closed.digest,
            "candidate_digest": proposal.candidate_digest,
            "now_ms": int(time.time() * 1000),
            "deadline_ms": deadline_ms,
        }
        core = PlanSealerV3.seal_core(request, proposal, views)
        security_digest = canonical_digest(
            {"policy": "spec170-v3-device-gate", "device": args.device})
        grants = tuple(
            PlanSealerV3.grant_view(core, provider, views[provider], security_digest)
            for provider in sorted(set(role_provider.values()))
        )
        plan_digest = PlanSealerV3.finalize_security(
            core, grants, security_digest)
        assignment_payloads = {}
        for provider in sorted(set(role_provider.values())):
            provider_roles = tuple(
                role for role in role_specs if role_provider[role.role] == provider)
            assignment_payloads.update({
                role.role: ProviderSelectionProjectionV3(
                    provider=provider,
                    request_id=args.request_id,
                    attempt=1,
                    plan_core_digest=core.plan_core_digest,
                    plan_digest=plan_digest,
                    roles=provider_roles,
                    dependencies=(),
                    deadline_ms=deadline_ms,
                ).to_bytes()
                for role in provider_roles
            })
        committed = collaboration.commit_plan(
            ack_closed_digest=closed.digest,
            roles=[CollaborationRole(
                role=role,
                service=args.service,
                min_providers=1,
                max_providers=1,
            ) for role in ROLES],
            key_scopes={},
            dependencies=[],
            role_scopes={},
            role_provider_assignments=role_provider,
            assignment_payloads_by_role=assignment_payloads,
        )
        if not committed:
            raise RuntimeError("V3 commit_plan returned false")
        print(
            f"SPEC170_V3_USER_SELECTION_COMMITTED requestId={args.request_id} "
            f"device={args.device} "
            f"planCoreDigest={core.plan_core_digest} planDigest={plan_digest}",
            flush=True,
        )
        response = collaboration.result(args.timeout_ms)
        if not response.status:
            raise RuntimeError(f"V3 response rejected: {response.error}")
        print(
            f"SPEC170_V3_USER_RESPONSE requestId={response.request_id} "
            f"bytes={len(response.payload)} payload={response.payload.decode(errors='replace')}",
            flush=True,
        )
        return 0
    finally:
        user.stop()


if __name__ == "__main__":
    raise SystemExit(main())
