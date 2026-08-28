#!/usr/bin/env python3
"""NDNSF user for the Spec170 one-Provider/two-GPU D2a gate."""

from __future__ import annotations

import hashlib
import argparse
import hmac
import json
import time
from pathlib import Path

import numpy as np
import onnxruntime as ort

from ndnsf import CollaborationRole, ServiceUser

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


INDEPENDENT_ROLES = ("/D2A/Independent/0", "/D2A/Independent/1")
LOCAL_GROUP_ROLE = "/D2A/LocalGroup"
SERVICE = "/Inference/Spec170LocalTwoGpu"
MODEL_DIGEST = "sha256:" + "3" * 64
GRAPH_DIGEST = "sha256:" + "4" * 64
SIGNING_KEY = b"spec170-v3-local-two-gpu-provider-key"


def _digest(value: str) -> str:
    return "sha256:" + hashlib.sha256(value.encode("utf-8")).hexdigest()


def build_d2a_role_contract(
        provider: str,
        backend: str = "onnxruntime-cuda",
) -> tuple[tuple[RoleAssemblySpec, ...], dict[str, str]]:
    """Build the four sealed execution items required by the D2a topology."""
    if not provider.startswith("/"):
        raise ValueError("D2a Provider must be a canonical NDN name")
    roles = (
        RoleAssemblySpec(
            role=INDEPENDENT_ROLES[0], rank=0, layer_begin=0, layer_end=1,
            recipe_digest=_digest("d2a:independent:0"),
            artifact_digest=_digest("d2a:artifact:independent:0"),
            backend=backend, device_set=("cuda:0",),
            role_kind="COMPONENT_SET",
        ),
        RoleAssemblySpec(
            role=INDEPENDENT_ROLES[1], rank=0, layer_begin=0, layer_end=1,
            recipe_digest=_digest("d2a:independent:1"),
            artifact_digest=_digest("d2a:artifact:independent:1"),
            backend=backend, device_set=("cuda:1",),
            role_kind="COMPONENT_SET",
        ),
        RoleAssemblySpec(
            role=LOCAL_GROUP_ROLE, rank=0, layer_begin=0, layer_end=1,
            recipe_digest=_digest("d2a:local-group:rank:0"),
            artifact_digest=_digest("d2a:artifact:local-group:rank:0"),
            backend=backend, device_set=("cuda:0",),
            role_kind="TENSOR_RANK",
        ),
        RoleAssemblySpec(
            role=LOCAL_GROUP_ROLE, rank=1, layer_begin=0, layer_end=1,
            recipe_digest=_digest("d2a:local-group:rank:1"),
            artifact_digest=_digest("d2a:artifact:local-group:rank:1"),
            backend=backend, device_set=("cuda:1",),
            role_kind="TENSOR_RANK",
        ),
    )
    provider_by_role = {
        INDEPENDENT_ROLES[0]: provider,
        INDEPENDENT_ROLES[1]: provider,
        f"{LOCAL_GROUP_ROLE}#0": provider,
        f"{LOCAL_GROUP_ROLE}#1": provider,
    }
    return roles, provider_by_role


def _role_key(role: RoleAssemblySpec, roles: tuple[RoleAssemblySpec, ...]) -> str:
    return (role.role if sum(item.role == role.role for item in roles) == 1
            else f"{role.role}#{role.rank}")


def _verify_offer(offer: ProviderOfferV3) -> bool:
    expected = hmac.new(
        SIGNING_KEY, offer.digest().encode("utf-8"), hashlib.sha256).hexdigest()
    return hmac.compare_digest(expected, offer.signature)


def _independent_oracle(artifact_root: str | Path) -> list[float]:
    path = Path(artifact_root) / "qwen-native-tracer-backbone.onnx"
    if not path.is_file():
        raise FileNotFoundError(path)
    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(
        str(path), sess_options=options, providers=["CPUExecutionProvider"])
    image = np.arange(1, 13, dtype=np.float32).reshape(1, 3, 2, 2) / 16.0
    result = session.run(None, {session.get_inputs()[0].name: image})[0]
    return [float(value) for value in np.asarray(result, dtype=np.float32).reshape(-1)]


def _close(left: list[float], right: list[float], tolerance: float = 1e-5) -> bool:
    return len(left) == len(right) and all(
        abs(actual - expected) <= tolerance
        for actual, expected in zip(left, right)
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact-root", required=True)
    parser.add_argument("--group", required=True)
    parser.add_argument("--controller", required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--trust-schema", required=True)
    parser.add_argument("--bootstrap-token", required=True)
    parser.add_argument("--provider", required=True)
    parser.add_argument("--service", default=SERVICE)
    parser.add_argument("--ack-timeout-ms", type=int, default=2500)
    parser.add_argument("--timeout-ms", type=int, default=20000)
    parser.add_argument("--request-id", default="spec170-v3-local-two-gpu")
    parser.add_argument("--case", choices=("positive", "missing-rank"),
                        default="positive")
    args = parser.parse_args()
    args.request_id = "/" + args.request_id.lstrip("/")
    roles, provider_by_role = build_d2a_role_contract(args.provider)
    role_keys = tuple(_role_key(role, roles) for role in roles)
    user = ServiceUser(
        group=args.group, controller=args.controller, user=args.user,
        trust_schema=args.trust_schema, permission_wait_ms=10000,
        bootstrap_token=args.bootstrap_token, adaptive_admission=False,
    )
    user.start()
    deadline_ms = int(time.time() * 1000) + args.timeout_ms
    candidate_digest = canonical_digest({
        "provider": args.provider,
        "roles": role_keys,
        "devices": ["cuda:0", "cuda:1"],
    })
    request_payload = json.dumps({
        "schema": "ndnsf-di-request-envelope-v2",
        "placementProfile": "DI_PLACEMENT_V3",
        "requestId": args.request_id,
        "attempt": 1,
        "deadlineMs": deadline_ms,
        "model": {"identityDigest": MODEL_DIGEST},
        "task": {"name": "spec170-v3-local-two-gpu"},
        "providerByRole": provider_by_role,
        "negativeCase": "" if args.case == "positive" else "missing_rank",
    }, sort_keys=True, separators=(",", ":")).encode("utf-8")

    try:
        if not any(str(item.service) == args.service
                   for item in user.get_allowed_services()):
            raise RuntimeError("D2a service is absent from user permission")
        collaboration = user.begin_collaboration(
            args.service, request_payload, mode="DEFERRED",
            ack_timeout_ms=args.ack_timeout_ms, timeout_ms=args.timeout_ms,
            request_id=args.request_id, fail_fast_terminal_selection=True,
        )
        closed = collaboration.acks_closed()
        print(
            f"SPEC170_D2A_USER_ACK_CLOSED requestId={closed.request_id} "
            f"ackCount={len(closed.candidates)} digest={closed.digest}",
            flush=True,
        )
        views: dict[str, ProviderPlanningViewV3] = {}
        for candidate in closed.candidates:
            print(
                f"SPEC170_D2A_ACK status={str(candidate.status).lower()} "
                f"message={candidate.message} payloadBytes={len(bytes(candidate.payload))}",
                flush=True,
            )
            if not candidate.status:
                continue
            offer = ProviderOfferV3.from_bytes(bytes(candidate.payload))
            if not _verify_offer(offer):
                raise RuntimeError(f"invalid D2a offer signature: {offer.provider}")
            view = ProviderPlanningViewV3.from_offer(
                offer, request_id=args.request_id, model_digest=MODEL_DIGEST,
                graph_digest=GRAPH_DIGEST, now_ms=int(time.time() * 1000),
                deadline_ms=deadline_ms, verify_signature=_verify_offer,
            )
            if (tuple(view.topology.devices) != ("cuda:0", "cuda:1")
                    or "onnxruntime-cuda" not in view.backends):
                raise RuntimeError(
                    f"D2a offer lacks exact two-GPU CUDA topology: {offer.provider}")
            views[offer.provider] = view
        if set(views) != {args.provider}:
            raise RuntimeError(
                "D2a requires exactly the configured Provider offer: "
                f"expected={args.provider} actual={sorted(views)}")

        proposal = PlacementProposalV3(
            request_id=args.request_id, attempt=1,
            model_digest=MODEL_DIGEST, graph_digest=GRAPH_DIGEST,
            roles=roles, provider_by_role=provider_by_role,
            dependencies=(), candidate_digest=candidate_digest,
            strategy_name="spec170-v3-local-two-gpu-fixed",
            strategy_version="1",
            strategy_state_digest=canonical_digest({
                "provider": args.provider, "devices": ["cuda:0", "cuda:1"]}),
        )
        core = PlanSealerV3.seal_core({
            "request_id": args.request_id, "attempt": 1,
            "ack_closed_digest": closed.digest,
            "candidate_digest": candidate_digest,
            "now_ms": int(time.time() * 1000), "deadline_ms": deadline_ms,
        }, proposal, views)
        security_digest = canonical_digest({
            "policy": "spec170-v3-local-two-gpu",
            "provider": args.provider,
        })
        plan_digest = PlanSealerV3.finalize_security(
            core, (), security_digest)
        projection = ProviderSelectionProjectionV3(
            provider=args.provider, request_id=args.request_id, attempt=1,
            plan_core_digest=core.plan_core_digest, plan_digest=plan_digest,
            roles=roles, dependencies=(), deadline_ms=deadline_ms,
        ).to_bytes()
        assignment_payloads_by_role = {key: projection for key in role_keys}
        committed = collaboration.commit_plan(
            ack_closed_digest=closed.digest,
            roles=[CollaborationRole(
                role=key, service=args.service, min_providers=1, max_providers=1,
                app_requirement=f"planDigest={plan_digest};".encode("utf-8"),
            ) for key in role_keys],
            key_scopes={}, dependencies=[],
            role_scopes={key: [] for key in role_keys},
            role_provider_assignments=provider_by_role,
            assignment_payloads_by_role=assignment_payloads_by_role,
        )
        if not committed:
            raise RuntimeError("D2a commit_plan returned false")
        print(
            f"SPEC170_D2A_USER_SELECTION_COMMITTED requestId={args.request_id} "
            f"planDigest={plan_digest} roles={len(role_keys)}",
            flush=True,
        )
        try:
            response = collaboration.result(args.timeout_ms)
        except TimeoutError as exc:
            if args.case == "missing-rank":
                print(
                    f"SPEC170_D2A_GROUP_FAILURE_PASS case=missing-rank reason={exc}",
                    flush=True,
                )
                return 0
            raise
        if args.case == "missing-rank":
            if response.status:
                raise RuntimeError("missing-rank case published a partial response")
            print(
                "SPEC170_D2A_GROUP_FAILURE_PASS case=missing-rank "
                f"responseStatus=false reason={response.error}",
                flush=True,
            )
            return 0
        if not response.status:
            raise RuntimeError(f"D2a response rejected: {response.error}")
        result = json.loads(response.payload.decode("utf-8"))
        if (not result.get("complete") or result.get("provider") != args.provider
                or result.get("collectiveBackend") != "nccl"):
            raise RuntimeError(f"incomplete D2a response: {result}")
        oracle = _independent_oracle(args.artifact_root)
        independent = result.get("independent", {})
        for index, role in enumerate(INDEPENDENT_ROLES):
            item = independent.get(role, {})
            if item.get("device") != f"cuda:{index}" or not _close(
                    list(item.get("values", [])), oracle):
                raise RuntimeError(f"D2a independent oracle failed: {role}")
        local = result.get("localGroup", {})
        if (local.get("unsplitOracle") != [11.0, 22.0, 33.0, 44.0]
                or float(local.get("maxAbsoluteError", 1.0)) > 1e-6):
            raise RuntimeError(f"D2a unsplit collective oracle failed: {local}")
        print(
            f"SPEC170_D2A_NUMERIC_ORACLE_PASS requestId={response.request_id} "
            "independentRoles=2 localRanks=2 collective=nccl maxAbsoluteError=0.0",
            flush=True,
        )
        print(
            f"SPEC170_D2A_USER_RESPONSE requestId={response.request_id} "
            f"bytes={len(response.payload)} complete=true",
            flush=True,
        )
        return 0
    finally:
        user.stop()


if __name__ == "__main__":
    raise SystemExit(main())
