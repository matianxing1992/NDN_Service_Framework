#!/usr/bin/env python3
"""Provider half of the Spec170 V3 two-rank NDNSF_DATA_V1 gate."""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
import struct
import time
from pathlib import Path

from ndnsf import AckDecision, ServiceProvider
from ndnsf_distributed_inference.provider import DIProviderOfferIssuerV3
from ndnsf_distributed_inference.sdk.placement import (
    ExecutionDisposition,
    ProviderSelectionProjectionV3,
    UNBOUND_GRAPH_DIGEST_V3,
)


MODEL_DIGEST = "sha256:" + "1" * 64
COLLECTIVE_SCOPE = "collective-epoch"


def _parse_request(payload: bytes) -> dict:
    value = json.loads(bytes(payload).decode("utf-8"))
    if not isinstance(value, dict):
        raise ValueError("D2b request must be a JSON object")
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--provider", required=True)
    parser.add_argument("--peer", required=True)
    parser.add_argument("--rank", type=int, choices=(0, 1), required=True)
    parser.add_argument("--role", required=True)
    parser.add_argument("--group", required=True)
    parser.add_argument("--controller", required=True)
    parser.add_argument("--trust-schema", required=True)
    parser.add_argument("--bootstrap-token", required=True)
    parser.add_argument("--service", default="/Inference/Spec170Collective")
    parser.add_argument("--device", choices=("cpu", "cuda:0"), default="cpu")
    args = parser.parse_args()
    if args.device.startswith("cuda:"):
        try:
            import torch  # type: ignore
            if not torch.cuda.is_available():
                raise RuntimeError("torch reports no CUDA device")
        except Exception as exc:  # noqa: BLE001
            raise SystemExit(f"D2B_CUDA_REQUIRED_FAIL: {exc}") from exc

    provider_id = args.provider.rstrip("/").rsplit("/", 1)[-1]
    provider = ServiceProvider(
        provider_id=provider_id,
        provider_prefix="/NDNSF-DI/Tracer/provider",
        group=args.group,
        controller=args.controller,
        trust_schema=args.trust_schema,
        bootstrap_token=args.bootstrap_token,
    )
    signing_key = b"spec170-v3-cross-provider-diagnostic-key"
    signer_key_id = "sha256:" + hashlib.sha256(signing_key).hexdigest()
    backend = (
        "onnxruntime-cuda" if args.device.startswith("cuda:") else "cpu")

    def negative_marker(name: str) -> None:
        evidence_root = os.environ.get("SPEC170_D2B_EVIDENCE_DIR", "")
        if evidence_root:
            Path(evidence_root, name).touch()
    issuer = DIProviderOfferIssuerV3(
        provider=args.provider,
        service=args.service,
        boot_epoch=provider.provider_boot_epoch,
        devices=(args.device,),
        signer_key_id=signer_key_id,
        sign_offer_digest=lambda digest: hmac.new(
            signing_key, digest.encode("utf-8"), hashlib.sha256).hexdigest(),
    )

    def make_ack(context, payload: bytes) -> AckDecision:
        request = _parse_request(payload)
        request_id = str(request.get("requestId", ""))
        deadline_ms = int(request.get("deadlineMs", 0) or 0)
        if not request_id or deadline_ms <= int(time.time() * 1000):
            return AckDecision(status=False, message="D2B_REQUEST_INVALID")
        decision = issuer.issue(
            request_id=request_id,
            attempt=int(request.get("attempt", 1)),
            model_digest=str(request.get("model", {}).get(
                "identityDigest", MODEL_DIGEST)),
            graph_digest=UNBOUND_GRAPH_DIGEST_V3,
            deadline_ms=deadline_ms,
            accepted_roles=(args.role,),
            backends=(backend,),
            execution_disposition=ExecutionDisposition.ACCEPT_WITH_PREPARATION,
            preparation_accepted=True,
        )
        print(
            f"SPEC170_D2B_PROVIDER_ACK provider={args.provider} rank={args.rank} "
            f"requestId={request_id} status={str(decision.status).lower()}",
            flush=True,
        )
        return decision

    def handle(context, payload: bytes) -> None:
        request = _parse_request(payload)
        negative_case = str(request.get("negativeCase", "") or "")
        projection = ProviderSelectionProjectionV3.from_bytes(
            context.assignment.assignment_payload)
        if projection.provider != args.provider or projection.request_id != context.session_id:
            context.fail("D2B_SELECTION_BINDING_MISMATCH")
            return
        if len(projection.roles) != 1 or projection.roles[0].rank != args.rank:
            print(
                f"SPEC170_D2B_NEGATIVE_PEER_REJECTED provider={args.provider} "
                f"rank={args.rank} assignedRank={projection.roles[0].rank if projection.roles else -1}",
                flush=True,
            )
            context.fail("D2B_RANK_ASSIGNMENT_MISMATCH")
            return

        plan_digest = projection.plan_digest
        group_id = "g-" + plan_digest.split(":", 1)[-1][:24]
        epoch = "epoch-1"
        capability = "d2b-capability-" + plan_digest.split(":", 1)[-1][:24]
        local = struct.pack("<4f", 10.0, 20.0, 30.0, 40.0)
        producer = args.peer if args.rank == 1 else args.provider
        consumer = args.provider if args.rank == 1 else args.peer
        source = struct.pack("<4f", 1.0, 2.0, 3.0, 4.0)
        payload_bytes = source if args.rank == 0 else None
        total_bytes = len(source)
        digest = "sha256:" + hashlib.sha256(source).hexdigest()
        manifest = {
            "request_id": context.session_id,
            "attempt_epoch": int(projection.attempt),
            "plan_digest": plan_digest,
            "group_id": group_id,
            "epoch": epoch,
            "operation_index": 0,
            "producer_rank": 0,
            "consumer_rank": 1,
            "producer_peer": producer,
            "consumer_peer": consumer,
            "tensor_digest": digest,
            "total_bytes": total_bytes,
            "segment_count": 1,
            "max_segment_bytes": 7000,
            "max_in_flight": 8,
        }
        try:
            if args.rank == 0:
                name = context.publish_ndnsf_data_v1(
                    COLLECTIVE_SCOPE, manifest, capability, payload_bytes)
                print(
                    f"SPEC170_D2B_DATA_PUBLISHED provider={args.provider} "
                    f"rank=0 dataName={name} bytes={len(payload_bytes)} "
                    "schema=NDNSF_DATA_V1",
                    flush=True,
                )
            else:
                received = context.fetch_ndnsf_data_v1(
                    COLLECTIVE_SCOPE, manifest, capability, 15000)
                if received is None or bytes(received) != source:
                    context.fail("D2B_DATA_FETCH_OR_DIGEST_FAIL")
                    return
                if negative_case == "replay":
                    # Re-fetch the same published wire with a different
                    # capability binding.  The name is unchanged, but the
                    # manifest signature must reject the replayed envelope.
                    replayed = context.fetch_ndnsf_data_v1(
                        COLLECTIVE_SCOPE, manifest, capability + "-replay", 15000)
                    if replayed is not None:
                        print(
                            f"SPEC170_D2B_NEGATIVE_REPLAY_ACCEPTED provider={args.provider}",
                            flush=True,
                        )
                        context.fail("D2B_REPLAY_NOT_REJECTED")
                        return
                    print(
                        f"SPEC170_D2B_NEGATIVE_REPLAY_REJECTED provider={args.provider}",
                        flush=True,
                    )
                    negative_marker("negative-replay.rejected")
                    context.fail("D2B_REPLAY_REJECTED")
                    return
                if negative_case == "partial":
                    print(
                        f"SPEC170_D2B_NEGATIVE_PARTIAL_OUTPUT_REJECTED provider={args.provider} "
                        "fetched=1 finalResponse=0",
                        flush=True,
                    )
                    negative_marker("negative-partial.rejected")
                    context.fail("D2B_PARTIAL_OUTPUT_REJECTED")
                    return
                values = struct.unpack("<4f", received)
                reduced = [left + right for left, right in zip(values, struct.unpack("<4f", local))]
                response = {
                    "schema": "ndnsf-data-v1-collective-result",
                    "groupId": group_id,
                    "epoch": epoch,
                    "rank": 1,
                    "sourceDigest": digest,
                    "reduced": reduced,
                    "complete": True,
                }
                context.publish_final_response(
                    json.dumps(response, sort_keys=True, separators=(",", ":")).encode())
                print(
                    f"SPEC170_D2B_DATA_FETCHED provider={args.provider} rank=1 "
                    f"bytes={len(received)} sourceDigest={digest} "
                    "schema=NDNSF_DATA_V1",
                    flush=True,
                )
                print(
                    f"SPEC170_D2B_PROVIDER_RESPONSE provider={args.provider} rank=1 "
                    f"requestId={context.session_id} complete=true",
                    flush=True,
                )
        except Exception as exc:  # noqa: BLE001
            context.fail("D2B_COLLECTIVE_FAIL:" + str(exc))
            print(f"SPEC170_D2B_COLLECTIVE_FAIL provider={args.provider} error={exc}", flush=True)

    provider.add_collaboration_handler(
        args.service,
        [args.role],
        handle,
        ack_handler=make_ack,
        include_ack_context=True,
    )
    print(
        f"SPEC170_D2B_PROVIDER_READY provider={args.provider} rank={args.rank} "
        f"peer={args.peer} device={args.device} backend={backend} "
        "placementProfile=DI_PLACEMENT_V3",
        flush=True,
    )
    return provider.run()


if __name__ == "__main__":
    raise SystemExit(main())

