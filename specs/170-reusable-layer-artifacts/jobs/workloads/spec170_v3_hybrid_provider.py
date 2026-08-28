#!/usr/bin/env python3
"""Provider side of the Spec170 heterogeneous hybrid CUDA gate.

This is intentionally a small real model workload: the four tiny ONNX
artifacts are executed with ONNX Runtime, while every inter-role activation is
carried by the request-scoped NDNSF_DATA_V1 path.  The script is a workload,
not a second placement authority; the User seals the role/provider mapping.
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

from ndnsf import AckDecision, ServiceProvider
from ndnsf_distributed_inference.provider import DIProviderOfferIssuerV3
from ndnsf_distributed_inference.sdk.placement import (
    ExecutionDisposition,
    UNBOUND_GRAPH_DIGEST_V3,
)


MODEL_DIGEST = "sha256:" + "1" * 64
SERVICE = "/Inference/Spec170Hybrid"
DATA_TOPIC_ROOT = "/NDNSF-DI/DATA/"
# Keep activation digests stable across CPU and CUDA ORT.  The provider still
# executes the real CUDA graph; only the cross-provider wire representation is
# canonicalized to remove backend-specific float32 ulps.
CANONICAL_TENSOR_DECIMALS = 5


def _canonical_tensor(value: np.ndarray) -> np.ndarray:
    rounded = np.round(np.asarray(value, dtype=np.float64),
                       decimals=CANONICAL_TENSOR_DECIMALS)
    return np.ascontiguousarray(rounded.astype(np.float32))


ARTIFACT_FILES = {
    "backbone": "qwen-native-tracer-backbone.onnx",
    "head0": "qwen-native-tracer-head0.onnx",
    "head1": "qwen-native-tracer-head1.onnx",
    "merge": "qwen-native-tracer-merge.onnx",
}


def _request(payload: bytes) -> dict:
    value = json.loads(bytes(payload).decode("utf-8"))
    if not isinstance(value, dict):
        raise ValueError("hybrid request must be an object")
    return value


def _rank(role: str) -> int:
    return int(role.rsplit("R", 1)[1])


def _stage(role: str) -> int:
    return int(role.split("/S", 1)[1].split("/", 1)[0])


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
    raise ValueError(f"unsupported hybrid mapping {mapping}")


class OnnxRoles:
    def __init__(self, artifact_root: str, device: str, roles: tuple[str, ...]):
        self.root = Path(artifact_root)
        self.device = device
        self.cuda = device.startswith("cuda:")
        self.sessions: dict[str, ort.InferenceSession] = {}
        for role in roles:
            kind = self.kind_for(role)
            if kind not in self.sessions:
                self.sessions[kind] = self._load(kind)
        # A real warmup is part of the readiness evidence, not just an import.
        for kind, session in self.sessions.items():
            if kind == "backbone":
                session.run(None, {session.get_inputs()[0].name: np.zeros((1, 3, 2, 2), dtype=np.float32)})
            elif kind in ("head0", "head1"):
                session.run(None, {session.get_inputs()[0].name: np.zeros((1, 16), dtype=np.float32)})
            else:
                session.run(None, {
                    session.get_inputs()[0].name: np.zeros((1, 8), dtype=np.float32),
                    session.get_inputs()[1].name: np.zeros((1, 8), dtype=np.float32),
                })

    @staticmethod
    def kind_for(role: str) -> str:
        stage = _stage(role)
        if stage == 0:
            return "backbone"
        if stage == 1:
            return "head0" if role.endswith("R0") else "head1"
        return "merge"

    def _load(self, kind: str) -> ort.InferenceSession:
        path = self.root / ARTIFACT_FILES[kind]
        if not path.is_file():
            raise RuntimeError(f"missing hybrid artifact: {path}")
        runtime_provider = "CUDAExecutionProvider" if self.cuda else "CPUExecutionProvider"
        if runtime_provider not in ort.get_available_providers():
            raise RuntimeError(f"required ONNX Runtime provider unavailable: {runtime_provider}")
        options = ort.SessionOptions()
        options.intra_op_num_threads = 1
        options.inter_op_num_threads = 1
        if self.cuda:
            if not hasattr(options, "add_session_config_entry"):
                raise RuntimeError("ONNX Runtime cannot disable CPU execution fallback")
            options.add_session_config_entry("session.disable_cpu_ep_fallback", "1")
            provider_spec = (runtime_provider, {"device_id": int(self.device.rsplit(":", 1)[1])})
        else:
            provider_spec = runtime_provider
        session = ort.InferenceSession(str(path), sess_options=options,
                                        providers=[provider_spec])
        available = tuple(session.get_providers())
        if not available or available[0] != runtime_provider:
            raise RuntimeError(f"ONNX Runtime provider was not selected for {path}: {available}")
        return session

    def run(self, role: str, inputs: dict[str, np.ndarray]) -> dict[str, np.ndarray]:
        session = self.sessions[self.kind_for(role)]
        values = session.run(None, inputs)
        return {output.name: _canonical_tensor(value)
                for output, value in zip(session.get_outputs(), values)}


def _expected_scope(request: dict, scope_name: str) -> dict:
    values = request.get("expectedScopes", {})
    value = values.get(scope_name)
    if not isinstance(value, dict):
        raise ValueError(f"missing expected activation manifest: {scope_name}")
    digest = str(value.get("tensor_digest", ""))
    total_bytes = int(value.get("total_bytes", 0) or 0)
    if not digest.startswith("sha256:") or len(digest) != 71 or total_bytes <= 0:
        raise ValueError(f"invalid expected activation manifest: {scope_name}")
    return {"tensor_digest": digest, "total_bytes": total_bytes}


def _assignment_fields(payload: bytes) -> dict[str, str]:
    text = bytes(payload or b"").decode("utf-8", errors="replace")
    result: dict[str, str] = {}
    for item in text.split(";"):
        if "=" not in item:
            continue
        key, value = item.split("=", 1)
        if key:
            result[key] = value
    return result


def _manifest(context, request: dict, plan_digest: str, mapping: str,
              scope_name: str, scope: dict, payload: bytes | None = None) -> tuple[dict, str]:
    expected = _expected_scope(request, scope_name)
    provider_by_role = request.get("providerByRole", {})
    if not isinstance(provider_by_role, dict):
        raise ValueError("missing providerByRole binding")
    def full_role(short: str) -> str:
        return f"/Pipeline/{short[0:2]}/{short[2:]}"

    producer_peer = str(provider_by_role.get(full_role(scope["producer"]), ""))
    consumer_peers = tuple(
        str(provider_by_role.get(full_role(role), ""))
        for role in scope["consumers"])
    if not producer_peer or any(not peer for peer in consumer_peers):
        raise ValueError(f"incomplete Provider mapping for {scope_name}")
    if payload is not None:
        actual_digest = "sha256:" + hashlib.sha256(payload).hexdigest()
        if actual_digest != expected["tensor_digest"] or len(payload) != expected["total_bytes"]:
            raise RuntimeError(
                f"activation digest mismatch scope={scope_name} "
                f"expected={expected['tensor_digest']}/{expected['total_bytes']} "
                f"actual={actual_digest}/{len(payload)}")
    group_id = "g-" + plan_digest.split(":", 1)[-1][:24]
    manifest = {
        "request_id": context.session_id,
        "attempt_epoch": 1,
        "plan_digest": plan_digest,
        "group_id": group_id,
        "epoch": "epoch-1",
        "operation_index": int(scope["op"]),
        "producer_rank": _rank(scope["producer"]),
        "consumer_rank": _rank(scope["consumers"][0]),
        "producer_peer": producer_peer,
        "consumer_peer": ",".join(consumer_peers),
        "tensor_digest": expected["tensor_digest"],
        "total_bytes": expected["total_bytes"],
        "segment_count": 1,
        "max_segment_bytes": 7000,
        "max_in_flight": 8,
        "mapping": mapping,
        "scope": scope_name,
    }
    capability = "spec170-hybrid-" + plan_digest.split(":", 1)[-1][:24] + "-" + scope_name
    return manifest, capability


def _full_role(short: str) -> str:
    """Expand the compact role spelling used by the frozen hybrid plan."""
    return f"/Pipeline/{short[0:2]}/{short[2:]}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--provider", required=True)
    parser.add_argument("--peer", required=True)
    parser.add_argument("--roles", required=True, help="comma-separated roles")
    parser.add_argument("--group", required=True)
    parser.add_argument("--controller", required=True)
    parser.add_argument("--trust-schema", required=True)
    parser.add_argument("--bootstrap-token", required=True)
    parser.add_argument("--service", default=SERVICE)
    parser.add_argument("--mapping", choices=("121", "212"), required=True)
    parser.add_argument("--artifact-root", default="/artifacts")
    parser.add_argument("--device", choices=("cpu", "cuda:0"), default="cpu")
    args = parser.parse_args()
    roles = tuple(item for item in args.roles.split(",") if item)
    if not roles:
        raise SystemExit("at least one hybrid role is required")
    if args.device.startswith("cuda:"):
        try:
            import torch  # type: ignore
            if not torch.cuda.is_available():
                raise RuntimeError("torch reports no CUDA device")
        except Exception as exc:  # noqa: BLE001
            raise SystemExit(f"SPEC170_D2H_CUDA_REQUIRED_FAIL: {exc}") from exc

    provider = ServiceProvider(
        provider_id=args.provider.rstrip("/").rsplit("/", 1)[-1],
        provider_prefix="/NDNSF-DI/Tracer/provider",
        group=args.group, controller=args.controller,
        trust_schema=args.trust_schema, bootstrap_token=args.bootstrap_token,
    )
    signing_key = b"spec170-v3-hybrid-provider-key"
    signer_key_id = "sha256:" + hashlib.sha256(signing_key).hexdigest()
    backend = "onnxruntime-cuda" if args.device.startswith("cuda:") else "onnxruntime-cpu"
    # Mapping 212 assigns both stage-1 heads to the same provider.  The
    # executable role is S1/R0, but that provider must still materialize the
    # companion head1 session because the role publishes both branch outputs.
    executor_roles = roles
    if args.mapping == "212" and "/Pipeline/S1/R0" in roles:
        executor_roles = tuple(dict.fromkeys((*roles, "/Pipeline/S1/R1")))
    executor = OnnxRoles(args.artifact_root, args.device, executor_roles)
    for role in roles:
        print(f"SPEC170_D2H_PROVIDER_WARMUP provider={args.provider} role={role} "
              f"backend={backend} realCompute=true cpuFallbackUsed=false", flush=True)

    def make_ack(_context, payload: bytes) -> AckDecision:
        request = _request(payload)
        request_roles = tuple(str(item) for item in request.get("roles", ()))
        if request.get("mapping") != args.mapping or not request_roles:
            return AckDecision(status=False, message="D2H_REQUEST_INVALID")
        issuer = getattr(make_ack, "issuer", None)
        if issuer is None:
            make_ack.issuer = issuer = DIProviderOfferIssuerV3(
                provider=args.provider, service=args.service,
                boot_epoch=provider.provider_boot_epoch, devices=(args.device,),
                signer_key_id=signer_key_id,
                sign_offer_digest=lambda digest: hmac.new(
                    signing_key, digest.encode("utf-8"), hashlib.sha256).hexdigest(),
            )
        decision = issuer.issue(
            request_id=str(request.get("requestId", "")),
            attempt=int(request.get("attempt", 1)),
            model_digest=str(request.get("model", {}).get("identityDigest", MODEL_DIGEST)),
            graph_digest=UNBOUND_GRAPH_DIGEST_V3,
            deadline_ms=int(request.get("deadlineMs", 0)),
            accepted_roles=roles, backends=(backend,),
            execution_disposition=ExecutionDisposition.ACCEPT_WITH_PREPARATION,
            preparation_accepted=True,
        )
        print(f"SPEC170_D2H_PROVIDER_ACK provider={args.provider} roles={','.join(roles)} "
              f"mapping={args.mapping} status={str(decision.status).lower()}", flush=True)
        return decision

    def handle(context, payload: bytes) -> None:
        request = _request(payload)
        assignment = context.assignment
        selected_provider_by_role = dict(assignment.role_providers)
        requested_provider_by_role = request.get("providerByRole", {})
        if (assignment.role != context.role or
                selected_provider_by_role.get(context.role) != args.provider or
                selected_provider_by_role != requested_provider_by_role or
                context.role not in roles or
                assignment.selection_digest == ""):
            context.fail("D2H_SELECTION_BINDING_MISMATCH")
            return
        negative = str(request.get("negativeCase", ""))
        missing_role = ("/Pipeline/S1/R1" if args.mapping == "121"
                        else "/Pipeline/S0/R1")
        if negative == "missing" and missing_role in roles:
            print(f"SPEC170_D2H_NEGATIVE_MISSING role={context.role} published=0", flush=True)
            return
        assignment_fields = _assignment_fields(assignment.assignment_payload)
        plan_digest = assignment_fields.get("planDigest", "")
        if not plan_digest.startswith("sha256:") or len(plan_digest) != 71:
            context.fail("D2H_PLAN_DIGEST_MISSING")
            return
        scopes = _scopes(args.mapping)

        # The current CollaborationContext API requires the Provider to
        # explicitly register each request-scoped receive binding before it
        # tries to decrypt peer Data.  The legacy V1 workload helpers were
        # removed; retaining them in a bundle makes a current SIF fail only
        # after the real Provider has already started.
        assigned_roles = set(roles)
        for scope_name, scope in scopes.items():
            members = {_full_role(scope["producer"]),
                       *(_full_role(role) for role in scope["consumers"])}
            if members.intersection(assigned_roles):
                context.allow_data(scope_name, DATA_TOPIC_ROOT + scope_name)

        # SVS publication is intentionally not looped back through the local
        # receive queue.  Keep a request-local copy for same-Provider edges;
        # only peer edges go through wait_one and the encrypted wire path.
        local_payloads: dict[str, bytes] = {}

        def publish(scope_name: str, data: bytes) -> None:
            scope = scopes[scope_name]
            manifest, capability = _manifest(
                context, request, plan_digest, args.mapping,
                scope_name, scope, data)
            topic = DATA_TOPIC_ROOT + scope_name
            # publish() is the current request-scoped NDNSF collaboration
            # Data path.  The operation manifest/capability remain validated
            # locally against the sealed request and the encrypted payload is
            # bound to this scope/topic by the native layer.
            context.publish(scope_name, topic, data)
            local_payloads[scope_name] = bytes(data)
            print(f"SPEC170_D2H_DATA_PUBLISHED role={context.role} scope={scope_name} "
                  f"bytes={len(data)} topic={topic} "
                  f"manifestDigest={hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()} "
                  f"capability={capability} schema=NDNSF_DATA_V1", flush=True)

        def fetch(scope_name: str, consumer_role: str) -> bytes:
            scope = scopes[scope_name]
            topic = DATA_TOPIC_ROOT + scope_name
            expected_producer = _full_role(scope["producer"])
            payload = local_payloads.get(scope_name)
            source = "local"
            if payload is None:
                value = context.wait_one(scope_name, topic, 15000)
                if value is None:
                    context.fail(f"D2H_DATA_FETCH_FAIL:{scope_name}")
                    raise RuntimeError(scope_name)
                provider_by_role = request.get("providerByRole", {})
                expected_provider = str(provider_by_role.get(expected_producer, ""))
                wire_role_provider = str(provider_by_role.get(value.producer_role, ""))
                if (value.key_scope != scope_name or
                        value.topic != topic or
                        value.producer != expected_provider or
                        not wire_role_provider or
                        wire_role_provider != expected_provider):
                    context.fail(f"D2H_DATA_BINDING_MISMATCH:{scope_name}")
                    raise RuntimeError(
                        f"unexpected collaboration binding scope={value.key_scope} "
                        f"topic={value.topic} producer={value.producer} "
                        f"wireRole={value.producer_role}")
                payload = bytes(value.payload)
                source = "peer"
            # Re-run the sealed manifest checks on the received plaintext;
            # this rejects wrong-scope, wrong-producer, truncated, or
            # substituted tensors before ONNX Runtime sees them.
            _manifest(context, request, plan_digest, args.mapping,
                      scope_name, scope, payload)
            print(f"SPEC170_D2H_DATA_FETCHED role={consumer_role} scope={scope_name} "
                  f"bytes={len(payload)} topic={topic} source={source} "
                  f"schema=NDNSF_DATA_V1", flush=True)
            return payload

        try:
            # One Provider Selection carries a role bundle.  Execute every
            # role in that bundle in dependency order; context.role identifies
            # the role used for authorization, not a limit of one role per
            # callback.  The previous workload handled only context.role and
            # silently dropped the remaining local stages.
            assigned = assigned_roles

            def has(role: str) -> bool:
                return role in assigned

            if args.mapping == "121":
                if has("/Pipeline/S0/R0"):
                    image = np.arange(1, 13, dtype=np.float32).reshape(1, 3, 2, 2) / 16.0
                    output = executor.run("/Pipeline/S0/R0", {"images": image})["features"]
                    publish("s0-to-s1", output.astype(np.float32).tobytes())

                if has("/Pipeline/S1/R0"):
                    features = np.frombuffer(
                        fetch("s0-to-s1", "/Pipeline/S1/R0"),
                        dtype=np.float32).reshape(1, 16)
                    output = executor.run(
                        "/Pipeline/S1/R0", {"features": features})["detections0"]
                    publish("s1r0-to-s2", output.astype(np.float32).tobytes())

                if has("/Pipeline/S1/R1"):
                    features = np.frombuffer(
                        fetch("s0-to-s1", "/Pipeline/S1/R1"),
                        dtype=np.float32).reshape(1, 16)
                    output = executor.run(
                        "/Pipeline/S1/R1", {"features": features})["detections1"]
                    publish("s1r1-to-s2", output.astype(np.float32).tobytes())

                if has("/Pipeline/S2/R0"):
                    d0 = np.frombuffer(
                        fetch("s1r0-to-s2", "/Pipeline/S2/R0"),
                        dtype=np.float32).reshape(1, 8)
                    d1 = np.frombuffer(
                        fetch("s1r1-to-s2", "/Pipeline/S2/R0"),
                        dtype=np.float32).reshape(1, 8)
                    prediction = executor.run(
                        "/Pipeline/S2/R0",
                        {"detections0": d0, "detections1": d1},
                    )["predictions"]
                    response = {
                        "schema": "ndnsf-di-hybrid-result", "mapping": args.mapping,
                        "complete": True, "planDigest": plan_digest,
                        "predictions": [float(value) for value in prediction.reshape(-1)],
                    }
                    context.publish_final_response(
                        json.dumps(response, sort_keys=True, separators=(",", ":")).encode())
                    print("SPEC170_D2H_RESPONSE role=/Pipeline/S2/R0 complete=true "
                          f"mapping={args.mapping}", flush=True)

            else:  # mapping 212
                if has("/Pipeline/S0/R0"):
                    image = np.arange(1, 13, dtype=np.float32).reshape(1, 3, 2, 2) / 16.0
                    output = executor.run(
                        "/Pipeline/S0/R0", {"images": image})["features"]
                    publish("s0r0-to-s1", output.astype(np.float32).tobytes())

                if has("/Pipeline/S0/R1"):
                    image = np.arange(1, 13, dtype=np.float32).reshape(1, 3, 2, 2) / 16.0
                    output = executor.run(
                        "/Pipeline/S0/R1", {"images": image})["features"]
                    publish("s0r1-to-s1", output.astype(np.float32).tobytes())

                if has("/Pipeline/S1/R0"):
                    left = np.frombuffer(
                        fetch("s0r0-to-s1", "/Pipeline/S1/R0"),
                        dtype=np.float32).reshape(1, 16)
                    right = np.frombuffer(
                        fetch("s0r1-to-s1", "/Pipeline/S1/R0"),
                        dtype=np.float32).reshape(1, 16)
                    features = ((left + right) / 2.0).astype(np.float32)
                    out0 = executor.run(
                        "/Pipeline/S1/R0", {"features": features})["detections0"]
                    out1 = executor.sessions["head1"].run(
                        None,
                        {executor.sessions["head1"].get_inputs()[0].name: features},
                    )[0]
                    publish("s1-to-s2-d0", _canonical_tensor(out0).tobytes())
                    publish("s1-to-s2-d1", _canonical_tensor(out1).tobytes())

                if has("/Pipeline/S2/R1"):
                    d0 = np.frombuffer(
                        fetch("s1-to-s2-d0", "/Pipeline/S2/R1"),
                        dtype=np.float32).reshape(1, 8)
                    d1 = np.frombuffer(
                        fetch("s1-to-s2-d1", "/Pipeline/S2/R1"),
                        dtype=np.float32).reshape(1, 8)
                    prediction = executor.run(
                        "/Pipeline/S2/R1",
                        {"detections0": d0, "detections1": d1},
                    )["predictions"]
                    publish("s2r1-to-s2r0", prediction.astype(np.float32).tobytes())

                if has("/Pipeline/S2/R0"):
                    d0 = np.frombuffer(
                        fetch("s1-to-s2-d0", "/Pipeline/S2/R0"),
                        dtype=np.float32).reshape(1, 8)
                    d1 = np.frombuffer(
                        fetch("s1-to-s2-d1", "/Pipeline/S2/R0"),
                        dtype=np.float32).reshape(1, 8)
                    prediction = executor.run(
                        "/Pipeline/S2/R0",
                        {"detections0": d0, "detections1": d1},
                    )["predictions"]
                    peer = np.frombuffer(
                        fetch("s2r1-to-s2r0", "/Pipeline/S2/R0"),
                        dtype=np.float32).reshape(1, 4)
                    prediction = ((prediction + peer) / 2.0).astype(np.float32)
                    response = {
                        "schema": "ndnsf-di-hybrid-result", "mapping": args.mapping,
                        "complete": True, "planDigest": plan_digest,
                        "predictions": [float(value) for value in prediction.reshape(-1)],
                    }
                    context.publish_final_response(
                        json.dumps(response, sort_keys=True, separators=(",", ":")).encode())
                    print("SPEC170_D2H_RESPONSE role=/Pipeline/S2/R0 complete=true "
                          f"mapping={args.mapping}", flush=True)
        except Exception as exc:  # noqa: BLE001
            context.fail("D2H_HYBRID_FAIL:" + str(exc))
            print(f"SPEC170_D2H_HYBRID_FAIL role={context.role} error={exc}", flush=True)

    provider.add_collaboration_handler(
        args.service, list(roles), handle, ack_handler=make_ack,
        include_ack_context=True)
    print(f"SPEC170_D2H_PROVIDER_READY provider={args.provider} roles={','.join(roles)} "
          f"mapping={args.mapping} device={args.device} backend={backend} "
          "realCompute=true cpuFallbackUsed=false placementProfile=DI_PLACEMENT_V3",
          flush=True)
    return provider.run()


if __name__ == "__main__":
    raise SystemExit(main())
