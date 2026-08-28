#!/usr/bin/env python3
"""Minimal two-Provider Spec170 V3 cross-Provider collective user.

This workload seals two distinct ranks onto two Providers, distributes one
request-scoped epoch key through the normal deferred collaboration plan, and
waits for rank 1 to publish the complete collective result.  It is a
correctness gate for the NDNSF_DATA_V1 path, not a throughput benchmark.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import time

from ndnsf import CollaborationDependency, CollaborationRole, ServiceUser
from ndnsf_distributed_inference.sdk.placement import (
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
ROLES = ("/Tensor/Rank/0", "/Tensor/Rank/1")
COLLECTIVE_SCOPE = "collective-epoch"
COLLECTIVE_TOPIC = "/NDNSF-DI/DATA"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--group", required=True)
    parser.add_argument("--controller", required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--trust-schema", required=True)
    parser.add_argument("--bootstrap-token", required=True)
    parser.add_argument("--provider0", required=True)
    parser.add_argument("--provider1", required=True)
    parser.add_argument("--service", default="/Inference/Spec170Collective")
    parser.add_argument("--ack-timeout-ms", type=int, default=1500)
    parser.add_argument("--timeout-ms", type=int, default=20000)
    parser.add_argument("--request-id", default="spec170-v3-cross-provider")
    parser.add_argument("--device", choices=("cpu", "cuda:0"), default="cpu")
    parser.add_argument(
        "--case", choices=("positive", "peer-mismatch", "replay", "partial"),
        default="positive",
    )
    args = parser.parse_args()
    # Native CollaborationContext exposes request IDs as canonical NDN Name
    # URIs.  Keep the same representation in ACK offers, the sealed plan, and
    # the per-Provider Selection projection.
    args.request_id = "/" + args.request_id.lstrip("/")

    signing_key = b"spec170-v3-cross-provider-diagnostic-key"
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
    user.start()
    deadline_ms = int(time.time() * 1000) + args.timeout_ms
    request_payload = json.dumps({
        "schema": "ndnsf-di-request-envelope-v2",
        "placementProfile": "DI_PLACEMENT_V3",
        "requestId": args.request_id,
        "attempt": 1,
        "deadlineMs": deadline_ms,
        "model": {"identityDigest": MODEL_DIGEST},
        "task": {"name": "spec170-v3-cross-provider-collective"},
        "negativeCase": "" if args.case == "positive" else args.case.replace("-", "_"),
    }, sort_keys=True, separators=(",", ":")).encode("utf-8")

    try:
        allowed = user.get_allowed_services()
        if not any(str(item.service) == args.service for item in allowed):
            raise RuntimeError("requested service is absent from user permission")
        collaboration = user.begin_collaboration(
            args.service,
            request_payload,
            mode="DEFERRED",
            ack_timeout_ms=args.ack_timeout_ms,
            timeout_ms=args.timeout_ms,
            request_id=args.request_id,
            fail_fast_terminal_selection=True,
        )
        closed = collaboration.acks_closed()
        print(
            f"SPEC170_D2B_USER_ACK_CLOSED requestId={closed.request_id} "
            f"ackCount={len(closed.candidates)} digest={closed.digest}",
            flush=True,
        )

        views = {}
        for candidate in closed.candidates:
            print(
                f"SPEC170_D2B_ACK status={str(candidate.status).lower()} "
                f"message={candidate.message} payloadBytes={len(bytes(candidate.payload))}",
                flush=True,
            )
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

        provider_by_role = {
            ROLES[0]: args.provider0,
            ROLES[1]: args.provider1,
        }
        if args.case == "peer-mismatch":
            provider_by_role = {
                ROLES[0]: args.provider1,
                ROLES[1]: args.provider0,
            }
        if any(provider not in views for provider in provider_by_role.values()):
            raise RuntimeError("both cross-Provider offers are required")

        role_specs = tuple(
            RoleAssemblySpec(
                role=role,
                rank=rank,
                layer_begin=rank,
                layer_end=rank + 1,
                recipe_digest="sha256:" + hashlib.sha256(
                    role.encode()).hexdigest(),
                artifact_digest="sha256:" + hashlib.sha256(
                    role.encode()).hexdigest(),
                backend=expected_backend,
                device_set=(args.device,),
            )
            for rank, role in enumerate(ROLES)
        )
        dependency = {
            "producers": [ROLES[0]],
            "consumers": [ROLES[1]],
            "key_scope": COLLECTIVE_SCOPE,
            "topic_prefix": COLLECTIVE_TOPIC,
            "required": True,
        }
        proposal = PlacementProposalV3(
            request_id=args.request_id,
            attempt=1,
            model_digest=MODEL_DIGEST,
            graph_digest=GRAPH_DIGEST,
            roles=role_specs,
            provider_by_role=provider_by_role,
            dependencies=(dependency,),
            candidate_digest=canonical_digest({
                "roles": list(ROLES), "providers": provider_by_role}),
            strategy_name="spec170-v3-cross-provider-collective",
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
        security_digest = canonical_digest({
            "policy": "spec170-v3-cross-provider-collective",
            "device": args.device,
        })
        plan_digest = PlanSealerV3.finalize_security(
            core, (), security_digest)
        assignment_payloads = {
            role.role: ProviderSelectionProjectionV3(
                provider=provider_by_role[role.role],
                request_id=args.request_id,
                attempt=1,
                plan_core_digest=core.plan_core_digest,
                plan_digest=plan_digest,
                roles=(role,),
                dependencies=(dependency,),
                deadline_ms=deadline_ms,
            ).to_bytes()
            for role in role_specs
        }
        committed = collaboration.commit_plan(
            ack_closed_digest=closed.digest,
            roles=[CollaborationRole(
                role=role,
                service=args.service,
                min_providers=1,
                max_providers=1,
            ) for role in ROLES],
            key_scopes={COLLECTIVE_SCOPE: list(ROLES)},
            dependencies=[CollaborationDependency(
                producers=[ROLES[0]],
                consumers=[ROLES[1]],
                key_scope=COLLECTIVE_SCOPE,
                topic_prefix=COLLECTIVE_TOPIC,
                required=True,
            )],
            role_scopes={role: [COLLECTIVE_SCOPE] for role in ROLES},
            role_provider_assignments=provider_by_role,
            assignment_payloads_by_role=assignment_payloads,
        )
        if not committed:
            raise RuntimeError("D2b commit_plan returned false")
        print(
            f"SPEC170_D2B_USER_SELECTION_COMMITTED requestId={args.request_id} "
            f"planDigest={plan_digest} group={COLLECTIVE_SCOPE}",
            flush=True,
        )
        try:
            response = collaboration.result(args.timeout_ms)
        except TimeoutError as exc:
            if args.case == "positive":
                raise
            print(
                f"SPEC170_D2B_NEGATIVE_PASS case={args.case} reason={exc}",
                flush=True,
            )
            return 0
        if args.case != "positive":
            if response.status:
                raise RuntimeError(
                    f"negative case unexpectedly produced a successful response: {args.case}")
            print(
                f"SPEC170_D2B_NEGATIVE_PASS case={args.case} "
                f"responseStatus=false error={response.error}",
                flush=True,
            )
            return 0
        if not response.status:
            raise RuntimeError(f"D2b response rejected: {response.error}")
        print(
            f"SPEC170_D2B_USER_RESPONSE requestId={response.request_id} "
            f"bytes={len(response.payload)} payload="
            f"{response.payload.decode(errors='replace')}",
            flush=True,
        )
        return 0
    finally:
        user.stop()


if __name__ == "__main__":
    raise SystemExit(main())
