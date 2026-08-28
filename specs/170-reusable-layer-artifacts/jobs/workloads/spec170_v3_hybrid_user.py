#!/usr/bin/env python3
"""User-side Spec170 hybrid placement and NDNSF_DATA_V1 gate.

The User computes a small CPU oracle only to seal the expected activation
envelopes.  Providers still execute every assigned ONNX role on the requested
CUDA device and the activation bytes cross the request-scoped NDNSF data plane.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
from pathlib import Path
import time

import numpy as np
import onnxruntime as ort

from ndnsf import CollaborationDependency, CollaborationRole, ServiceUser
from ndnsf_distributed_inference.sdk.placement import (
    PlacementProposalV3,
    PlanSealerV3,
    ProviderOfferV3,
    ProviderPlanningViewV3,
    RoleAssemblySpec,
    UNBOUND_GRAPH_DIGEST_V3,
    canonical_digest,
)


MODEL_DIGEST = "sha256:" + "1" * 64
GRAPH_DIGEST = "sha256:" + "2" * 64
SERVICE = "/Inference/Spec170Hybrid"
SIGNING_KEY = b"spec170-v3-hybrid-provider-key"
# CPU and CUDA ORT can differ in the final float32 ulps even for the same
# artifact.  Hash a stable fixed-point wire representation instead.
CANONICAL_TENSOR_DECIMALS = 5
NUMERIC_ORACLE_ABSOLUTE_TOLERANCE = 1.0e-6


def _canonical_tensor(value: np.ndarray) -> np.ndarray:
    rounded = np.round(np.asarray(value, dtype=np.float64),
                       decimals=CANONICAL_TENSOR_DECIMALS)
    return np.ascontiguousarray(rounded.astype(np.float32))


def _scopes(mapping: str) -> dict[str, dict]:
    if mapping == "121":
        return {
            "s0-to-s1": {"op": 0, "producer": "S0R0", "consumers": ("S1R0", "S1R1"), "tensor": "features"},
            "s1r0-to-s2": {"op": 1, "producer": "S1R0", "consumers": ("S2R0",), "tensor": "detections0"},
            "s1r1-to-s2": {"op": 2, "producer": "S1R1", "consumers": ("S2R0",), "tensor": "detections1"},
        }
    if mapping == "212":
        return {
            "s0r0-to-s1": {"op": 0, "producer": "S0R0", "consumers": ("S1R0",), "tensor": "features0"},
            "s0r1-to-s1": {"op": 1, "producer": "S0R1", "consumers": ("S1R0",), "tensor": "features1"},
            "s1-to-s2-d0": {"op": 2, "producer": "S1R0", "consumers": ("S2R0", "S2R1"), "tensor": "detections0"},
            "s1-to-s2-d1": {"op": 3, "producer": "S1R0", "consumers": ("S2R0", "S2R1"), "tensor": "detections1"},
            "s2r1-to-s2r0": {"op": 4, "producer": "S2R1", "consumers": ("S2R0",), "tensor": "predictions"},
        }
    raise ValueError(f"unsupported hybrid mapping: {mapping}")


def _roles(mapping: str) -> tuple[str, ...]:
    if mapping == "121":
        short = ("S0R0", "S1R0", "S1R1", "S2R0")
    else:
        short = ("S0R0", "S0R1", "S1R0", "S2R0", "S2R1")
    return tuple(f"/Pipeline/{item[0:2]}/{item[2:]}" for item in short)


def _provider_by_role(mapping: str, provider0: str, provider1: str) -> dict[str, str]:
    if mapping == "121":
        owners = {"S0R0": provider0, "S1R0": provider0,
                  "S1R1": provider1, "S2R0": provider1}
    else:
        owners = {"S0R0": provider0, "S0R1": provider1,
                  "S1R0": provider0, "S2R0": provider0,
                  "S2R1": provider1}
    return {f"/Pipeline/{short[0:2]}/{short[2:]}" if len(short) == 4
            else f"/Pipeline/{short[0:2]}/{short[2:]}": provider
            for short, provider in owners.items()}


def _session(root: Path, kind: str) -> ort.InferenceSession:
    files = {
        "backbone": "qwen-native-tracer-backbone.onnx",
        "head0": "qwen-native-tracer-head0.onnx",
        "head1": "qwen-native-tracer-head1.onnx",
        "merge": "qwen-native-tracer-merge.onnx",
    }
    path = root / files[kind]
    if not path.is_file():
        raise FileNotFoundError(path)
    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    options.inter_op_num_threads = 1
    return ort.InferenceSession(str(path), sess_options=options,
                                providers=["CPUExecutionProvider"])


def _run(session: ort.InferenceSession, values: dict[str, np.ndarray]) -> np.ndarray:
    names = [item.name for item in session.get_inputs()]
    outputs = session.run(None, {name: values[name] for name in names})
    return _canonical_tensor(outputs[0])


def _expected_scopes(root: str, mapping: str) -> dict[str, dict]:
    """Return exact digest/length metadata for the fixed activation oracle."""
    path = Path(root)
    backbone = _session(path, "backbone")
    head0 = _session(path, "head0")
    head1 = _session(path, "head1")
    merge = _session(path, "merge")
    image = np.arange(1, 13, dtype=np.float32).reshape(1, 3, 2, 2) / 16.0
    features = _run(backbone, {"images": image})
    if mapping == "121":
        values = {
            "s0-to-s1": features,
            "s1r0-to-s2": _run(head0, {"features": features}),
            "s1r1-to-s2": _run(head1, {"features": features}),
        }
    else:
        averaged = (features + features) / 2.0
        values = {
            "s0r0-to-s1": features,
            "s0r1-to-s1": features,
            "s1-to-s2-d0": _run(head0, {"features": averaged}),
            "s1-to-s2-d1": _run(head1, {"features": averaged}),
        }
        prediction = _run(merge, {
            "detections0": values["s1-to-s2-d0"],
            "detections1": values["s1-to-s2-d1"],
        })
        values["s2r1-to-s2r0"] = prediction
    result = {}
    for name, value in values.items():
        payload = value.tobytes(order="C")
        result[name] = {
            "tensor_digest": "sha256:" + hashlib.sha256(payload).hexdigest(),
            "total_bytes": len(payload),
            "segment_count": 1,
        }
    return result


def evaluate_numeric_oracle(
        root: str | Path,
        mapping: str,
        observed: list[float],
        absolute_tolerance: float = NUMERIC_ORACLE_ABSOLUTE_TOLERANCE,
) -> dict[str, object]:
    """Evaluate the final response against the sealed CPU ONNX oracle."""
    if mapping not in {"121", "212"}:
        raise ValueError(f"unsupported hybrid mapping: {mapping}")
    path = Path(root)
    image = np.arange(1, 13, dtype=np.float32).reshape(1, 3, 2, 2) / 16.0
    features = _run(_session(path, "backbone"), {"images": image})
    if mapping == "212":
        # Both fixed S0 ranks receive the same deterministic request input;
        # S1/R0 averages their canonical activation bytes.
        features = _canonical_tensor((features + features) / 2.0)
    detections0 = _run(_session(path, "head0"), {"features": features})
    detections1 = _run(_session(path, "head1"), {"features": features})
    expected = _run(
        _session(path, "merge"),
        {"detections0": detections0, "detections1": detections1},
    ).reshape(-1)
    actual = np.asarray(observed, dtype=np.float32).reshape(-1)
    shape_matches = actual.shape == expected.shape
    maximum_error = (
        float(np.max(np.abs(actual - expected)))
        if shape_matches and expected.size
        else float("inf")
    )
    status = (
        "PASS"
        if shape_matches and maximum_error <= absolute_tolerance
        else "FAIL"
    )
    return {
        "status": status,
        "mapping": mapping,
        "expected": [float(value) for value in expected],
        "observed": [float(value) for value in actual],
        "absoluteTolerance": absolute_tolerance,
        "maxAbsoluteError": maximum_error,
    }


def _verify_offer(offer: ProviderOfferV3) -> bool:
    expected = hmac.new(
        SIGNING_KEY, offer.digest().encode("utf-8"), hashlib.sha256).hexdigest()
    return hmac.compare_digest(expected, offer.signature)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mapping", choices=("121", "212"), required=True)
    parser.add_argument("--artifact-root", default="/artifacts")
    parser.add_argument("--group", required=True)
    parser.add_argument("--controller", required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--trust-schema", required=True)
    parser.add_argument("--bootstrap-token", required=True)
    parser.add_argument("--provider0", required=True)
    parser.add_argument("--provider1", required=True)
    parser.add_argument("--service", default=SERVICE)
    parser.add_argument("--device", choices=("cpu", "cuda:0"), default="cuda:0")
    parser.add_argument("--ack-timeout-ms", type=int, default=2500)
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("--request-id", default="spec170-v3-hybrid")
    parser.add_argument("--case", choices=("positive", "missing"), default="positive")
    args = parser.parse_args()
    args.request_id = "/" + args.request_id.lstrip("/")
    roles = _roles(args.mapping)
    scopes = _scopes(args.mapping)
    provider_by_role = _provider_by_role(args.mapping, args.provider0, args.provider1)
    expected_scopes = _expected_scopes(args.artifact_root, args.mapping)
    backend = "onnxruntime-cuda" if args.device.startswith("cuda:") else "onnxruntime-cpu"
    user = ServiceUser(
        group=args.group, controller=args.controller, user=args.user,
        trust_schema=args.trust_schema, permission_wait_ms=10000,
        bootstrap_token=args.bootstrap_token, adaptive_admission=False,
    )
    user.start()
    deadline_ms = int(time.time() * 1000) + args.timeout_ms
    candidate_digest = canonical_digest({
        "mapping": args.mapping, "roles": list(roles),
        "providerByRole": provider_by_role, "expectedScopes": expected_scopes,
    })
    request_payload = json.dumps({
        "schema": "ndnsf-di-request-envelope-v2",
        "placementProfile": "DI_PLACEMENT_V3",
        "requestId": args.request_id,
        "attempt": 1,
        "deadlineMs": deadline_ms,
        "model": {"identityDigest": MODEL_DIGEST},
        "task": {"name": "spec170-v3-hybrid", "mapping": args.mapping},
        "mapping": args.mapping,
        "roles": list(roles),
        "providerByRole": provider_by_role,
        "expectedScopes": expected_scopes,
        "negativeCase": "" if args.case == "positive" else args.case,
    }, sort_keys=True, separators=(",", ":")).encode("utf-8")

    try:
        if not any(str(item.service) == args.service for item in user.get_allowed_services()):
            raise RuntimeError("requested hybrid service is absent from user permission")
        collaboration = user.begin_collaboration(
            args.service, request_payload, mode="DEFERRED",
            ack_timeout_ms=args.ack_timeout_ms, timeout_ms=args.timeout_ms,
            request_id=args.request_id, fail_fast_terminal_selection=True,
        )
        closed = collaboration.acks_closed()
        print(f"SPEC170_D2H_USER_ACK_CLOSED requestId={closed.request_id} "
              f"ackCount={len(closed.candidates)} digest={closed.digest}", flush=True)
        views = {}
        for candidate in closed.candidates:
            print(
                f"SPEC170_D2H_ACK status={str(candidate.status).lower()} "
                f"message={candidate.message} payloadBytes={len(bytes(candidate.payload))}",
                flush=True,
            )
            if not candidate.status:
                continue
            offer = ProviderOfferV3.from_bytes(bytes(candidate.payload))
            if not _verify_offer(offer):
                raise RuntimeError(f"invalid V3 offer signature: {offer.provider}")
            view = ProviderPlanningViewV3.from_offer(
                offer, request_id=args.request_id, model_digest=MODEL_DIGEST,
                graph_digest=GRAPH_DIGEST, now_ms=int(time.time() * 1000),
                deadline_ms=deadline_ms, verify_signature=_verify_offer)
            if tuple(view.topology.devices) != (args.device,) or backend not in view.backends:
                raise RuntimeError(f"provider offer does not satisfy {args.device}/{backend}: {offer.provider}")
            views[offer.provider] = view
        if args.provider0 not in views or args.provider1 not in views:
            raise RuntimeError("both hybrid Provider offers are required")

        role_specs = tuple(
            RoleAssemblySpec(
                role=role,
                rank=int(role.rsplit("R", 1)[1]),
                layer_begin=int(role.split("/S", 1)[1].split("/", 1)[0]),
                layer_end=int(role.split("/S", 1)[1].split("/", 1)[0]) + 1,
                recipe_digest="sha256:" + hashlib.sha256((args.mapping + role).encode()).hexdigest(),
                artifact_digest="sha256:" + hashlib.sha256((args.mapping + ":artifact:" + role).encode()).hexdigest(),
                backend=backend, device_set=(args.device,), role_kind="HYBRID_RANK",
            ) for role in roles
        )
        dependencies = []
        def full(short: str) -> str:
            return f"/Pipeline/{short[0:2]}/{short[2:]}"
        for scope_name, scope in scopes.items():
            dependencies.append({
                "producers": [full(scope["producer"])],
                "consumers": [full(item) for item in scope["consumers"]],
                "key_scope": scope_name,
                "topic_prefix": "/NDNSF-DI/DATA/" + scope_name,
                "required": True,
            })
        dependencies = tuple(dependencies)
        role_scopes = {role: [] for role in roles}
        key_scopes = {}
        for scope_name, scope in scopes.items():
            members = [full(scope["producer"])] + [full(item) for item in scope["consumers"]]
            key_scopes[scope_name] = list(dict.fromkeys(members))
            for role in members:
                role_scopes[role].append(scope_name)
        proposal = PlacementProposalV3(
            request_id=args.request_id, attempt=1, model_digest=MODEL_DIGEST,
            graph_digest=GRAPH_DIGEST, roles=role_specs,
            provider_by_role=provider_by_role, dependencies=dependencies,
            candidate_digest=candidate_digest,
            strategy_name="spec170-v3-hybrid-fixed-mapping", strategy_version="1",
            strategy_state_digest=canonical_digest({"mapping": args.mapping, "device": args.device}),
        )
        core_request = {
            "request_id": args.request_id, "attempt": 1,
            "ack_closed_digest": closed.digest, "candidate_digest": candidate_digest,
            "now_ms": int(time.time() * 1000), "deadline_ms": deadline_ms,
        }
        core = PlanSealerV3.seal_core(core_request, proposal, views)
        security_digest = canonical_digest({
            "policy": "spec170-v3-hybrid", "mapping": args.mapping,
            "expectedScopes": expected_scopes,
        })
        plan_digest = PlanSealerV3.finalize_security(core, (), security_digest)
        committed = collaboration.commit_plan(
            ack_closed_digest=closed.digest,
            roles=[CollaborationRole(role=role, service=args.service,
                                     min_providers=1, max_providers=1,
                                     app_requirement=(
                                         f"planDigest={plan_digest};"
                                     ).encode("utf-8"))
                   for role in roles],
            key_scopes=key_scopes,
            dependencies=[CollaborationDependency(
                producers=list(dep["producers"]), consumers=list(dep["consumers"]),
                key_scope=dep["key_scope"], topic_prefix=dep["topic_prefix"],
                required=True) for dep in dependencies],
            role_scopes=role_scopes,
            role_provider_assignments=provider_by_role,
        )
        if not committed:
            raise RuntimeError("D2h commit_plan returned false")
        print(f"SPEC170_D2H_USER_SELECTION_COMMITTED requestId={args.request_id} "
              f"mapping={args.mapping} planDigest={plan_digest}", flush=True)
        try:
            response = collaboration.result(args.timeout_ms)
        except TimeoutError as exc:
            if args.case == "positive":
                raise
            print(f"SPEC170_D2H_NEGATIVE_PASS case={args.case} reason={exc}", flush=True)
            return 0
        if args.case != "positive":
            if response.status:
                raise RuntimeError("negative hybrid case unexpectedly succeeded")
            print(f"SPEC170_D2H_NEGATIVE_PASS case={args.case} responseStatus=false", flush=True)
            return 0
        if not response.status:
            raise RuntimeError(f"D2h response rejected: {response.error}")
        result = json.loads(response.payload.decode("utf-8"))
        if not result.get("complete") or result.get("mapping") != args.mapping:
            raise RuntimeError(f"incomplete hybrid response: {result}")
        if not isinstance(result.get("predictions"), list) or len(result["predictions"]) != 4:
            raise RuntimeError("hybrid response prediction shape mismatch")
        oracle = evaluate_numeric_oracle(
            args.artifact_root, args.mapping, result["predictions"])
        if oracle["status"] != "PASS":
            raise RuntimeError(f"hybrid numerical oracle failed: {oracle}")
        expected = _expected_scopes(args.artifact_root, args.mapping)
        print(
            "SPEC170_D2H_NUMERIC_ORACLE_PASS "
            f"mapping={args.mapping} maxAbsoluteError={oracle['maxAbsoluteError']} "
            f"absoluteTolerance={oracle['absoluteTolerance']}",
            flush=True,
        )
        print(f"SPEC170_D2H_USER_RESPONSE requestId={response.request_id} "
              f"mapping={args.mapping} bytes={len(response.payload)} "
              f"complete=true expectedScopes={len(expected)}", flush=True)
        return 0
    finally:
        user.stop()


if __name__ == "__main__":
    raise SystemExit(main())
