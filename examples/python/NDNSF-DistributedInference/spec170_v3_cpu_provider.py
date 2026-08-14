#!/usr/bin/env python3
"""Minimal real-network Spec170 V3 CPU Provider.

This process deliberately exercises the public Python V3 collaboration API,
not the legacy NativeTracer V1 driver.  It advertises one CPU role, accepts a
V3 offer, and emits a final response only for the merge role.  The workload is
small by design: this is a control/selection/response gate before model and
artifact campaigns.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
from pathlib import Path
import time

from ndnsf import AckDecision, ServiceProvider
from ndnsf_distributed_inference.provider import DIProviderOfferIssuerV3
from ndnsf_distributed_inference.sdk.placement import (
    ExecutionDisposition,
    UNBOUND_GRAPH_DIGEST_V3,
)


MODEL_DIGEST = "sha256:" + "1" * 64


def _parse_request(payload: bytes) -> dict:
    try:
        value = json.loads(bytes(payload).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("Spec170 V3 request is not JSON") from exc
    if not isinstance(value, dict):
        raise ValueError("Spec170 V3 request must be an object")
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--provider", required=True)
    parser.add_argument("--role", required=True)
    parser.add_argument("--group", required=True)
    parser.add_argument("--controller", required=True)
    parser.add_argument("--trust-schema", required=True)
    parser.add_argument("--bootstrap-token", required=True)
    parser.add_argument("--service", default="/Inference/NativeTracer")
    parser.add_argument("--artifact", default="")
    parser.add_argument("--onnx", action="store_true")
    args = parser.parse_args()

    provider_id = args.provider.rstrip("/").rsplit("/", 1)[-1]
    provider = ServiceProvider(
        # The Python binding appends provider_id to provider_prefix.  Keep the
        # wire identity explicit in the issuer while passing only the final
        # component to the binding.
        provider_id=provider_id,
        provider_prefix="/NDNSF-DI/Tracer/provider",
        group=args.group,
        controller=args.controller,
        trust_schema=args.trust_schema,
        bootstrap_token=args.bootstrap_token,
    )
    signing_key = b"spec170-v3-cpu-network-diagnostic-key"
    signer_key_id = "sha256:" + hashlib.sha256(signing_key).hexdigest()
    issuer = DIProviderOfferIssuerV3(
        provider=args.provider,
        service=args.service,
        boot_epoch=provider.provider_boot_epoch,
        devices=("cpu",),
        signer_key_id=signer_key_id,
        sign_offer_digest=lambda digest: hmac.new(
            signing_key, digest.encode("utf-8"), hashlib.sha256).hexdigest(),
    )
    session = None
    np = None
    artifact_digest = ""
    if args.onnx:
        if not args.artifact:
            raise SystemExit("--onnx requires --artifact")
        try:
            import numpy as np  # type: ignore
            import onnxruntime as ort  # type: ignore
            artifact_path = Path(args.artifact)
            artifact_digest = "sha256:" + hashlib.sha256(
                artifact_path.read_bytes()).hexdigest()
            session = ort.InferenceSession(
                str(artifact_path), providers=["CPUExecutionProvider"])
            print(
                "SPEC170_V3_PROVIDER_RUNTIME_READY "
                f"provider={args.provider} role={args.role} "
                f"backend=onnxruntime device=cpu artifactDigest={artifact_digest} "
                f"providers={','.join(session.get_providers())}",
                flush=True,
            )
        except Exception as exc:  # noqa: BLE001
            raise SystemExit(f"ONNX_RUNTIME_INIT_FAIL: {exc}") from exc

    def execute_onnx() -> list[dict]:
        if session is None or np is None:
            return []
        outputs = []
        for input_meta in session.get_inputs():
            shape = tuple(
                1 if not isinstance(dim, int) or dim <= 0 else dim
                for dim in input_meta.shape
            )
            value = np.zeros(shape, dtype=np.float32)
            outputs.append((input_meta.name, value))
        result = session.run(None, dict(outputs))
        return [
            {"shape": list(getattr(value, "shape", ())),
             "bytes": int(getattr(value, "nbytes", 0))}
            for value in result
        ]

    def make_ack(context, payload: bytes) -> AckDecision:
        request = _parse_request(payload)
        request_id = str(request.get("requestId", ""))
        deadline_ms = int(request.get("deadlineMs", 0) or 0)
        if not request_id or deadline_ms <= int(time.time() * 1000):
            return AckDecision(status=False, message="DI_V3_REQUEST_INVALID")
        model = request.get("model", {})
        model_digest = str(model.get("identityDigest", MODEL_DIGEST))
        decision = issuer.issue(
            request_id=request_id,
            attempt=int(request.get("attempt", 1)),
            model_digest=model_digest,
            graph_digest=UNBOUND_GRAPH_DIGEST_V3,
            deadline_ms=deadline_ms,
            accepted_roles=(args.role,),
            backends=("cpu",),
            execution_disposition=ExecutionDisposition.ACCEPT_WITH_PREPARATION,
            preparation_accepted=True,
        )
        print(
            "SPEC170_V3_PROVIDER_ACK "
            f"provider={args.provider} role={args.role} "
            f"requestId={request_id} status={str(decision.status).lower()} "
            "placementProfile=DI_PLACEMENT_V3 executionDevice=cpu",
            flush=True,
        )
        return decision

    def handle(context, payload: bytes) -> None:
        print(
            "SPEC170_V3_PROVIDER_SELECTED "
            f"provider={args.provider} role={context.role} "
            f"requestId={context.session_id}",
            flush=True,
        )
        outputs = execute_onnx()
        if args.onnx:
            print(
                "SPEC170_V3_PROVIDER_EXECUTION "
                f"provider={args.provider} role={args.role} "
                f"backend=onnxruntime device=cpu artifactDigest={artifact_digest} "
                f"outputs={json.dumps(outputs, sort_keys=True, separators=(',', ':'))}",
                flush=True,
            )
        if args.role == "/Merge":
            context.publish_final_response(
                json.dumps({
                    "schema": "spec170-v3-cpu-response-v1",
                    "provider": args.provider,
                    "role": args.role,
                    "payload": "V3_CPU_OK",
                    "backend": "onnxruntime" if args.onnx else "diagnostic",
                    "outputs": outputs,
                }, sort_keys=True, separators=(",", ":")).encode("utf-8")
            )
            print(
                "SPEC170_V3_PROVIDER_RESPONSE "
                f"provider={args.provider} role={args.role} "
                f"requestId={context.session_id}",
                flush=True,
            )

    provider.add_collaboration_handler(
        args.service,
        [args.role],
        handle,
        ack_handler=make_ack,
        include_ack_context=True,
    )
    print(
        "SPEC170_V3_PROVIDER_READY "
        f"provider={args.provider} role={args.role} device=cpu "
        "placementProfile=DI_PLACEMENT_V3 "
        f"onnx={str(args.onnx).lower()}",
        flush=True,
    )
    return provider.run()


if __name__ == "__main__":
    raise SystemExit(main())
