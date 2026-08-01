#!/usr/bin/env python3
"""Byte-payload NDNSF-DI V2 Provider for the bounded Docker lifecycle."""

from __future__ import annotations

import argparse
from pathlib import Path
import time

from ndnsf import ServiceProvider
from ndnsf_distributed_inference.core import (
    DISelectionAssignmentV2,
    DISelectionParticipant,
    GpuMiBAdmissionLedger,
    SelectionPreparationCallbacks,
)
from ndnsf_distributed_inference.provider import (
    DIProviderOfferIssuer,
    register_selection_dataflow_v2,
)


ROLES = {
    "A": ("source",),
    "B": ("left", "merge"),
    "C": ("right",),
}


def event(marker: str, **fields: object) -> None:
    rendered = " ".join(f"{key}={value}" for key, value in fields.items())
    print(f"{marker} tsNs={time.time_ns()} {rendered}".rstrip(), flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--provider-id", required=True, choices=tuple(ROLES))
    parser.add_argument("--state-dir", required=True)
    args = parser.parse_args()
    provider_id = args.provider_id
    identity = f"/example/hello/provider/{provider_id}"
    boot_epoch = f"spec163-boot-{provider_id}"
    roles = ROLES[provider_id]
    provider = ServiceProvider(provider_id=provider_id)
    boot_epoch = provider.provider_boot_epoch
    ledger = GpuMiBAdmissionLedger(
        provider=identity, boot_epoch=boot_epoch, capacity_mib=2048)
    issuer = DIProviderOfferIssuer(
        provider=identity, service="/HELLO", boot_epoch=boot_epoch,
        ledger=ledger, offered_gpu_memory_mb=2048,
        signer_key_id=f"{identity}/KEY/1",
        sign_offer_digest=lambda value: "provider-signature:" + value,
        offer_lease_ms=8000)

    def prepared(context) -> None:
        if provider_id == "C":
            time.sleep(1.0)
        event(
            "SPEC163_LOCAL_READY", provider=identity,
            role=context.role.role, transaction=context.transaction_id)

    participant = DISelectionParticipant(
        provider=identity, boot_epoch=boot_epoch, ledger=ledger,
        offer_lookup=issuer.lookup,
        callbacks=SelectionPreparationCallbacks(
            prepare_role=prepared,
            start_role=lambda role: event(
                "SPEC163_ROLE_START", provider=identity, role=role)),
        clock_ms=lambda: int(time.time() * 1000))
    state_dir = Path(args.state_dir)
    state_dir.mkdir(parents=True, exist_ok=True)
    register_selection_dataflow_v2(
        provider, service="/HELLO", participant=participant,
        wal_path=state_dir / f"provider-{provider_id}.wal",
        storage_key=bytes([65 + ord(provider_id) - ord("A")]) * 32,
        storage_key_epoch="spec163-key-epoch-1")

    def ack(payload: bytes):
        decision = issuer.issue(
            payload, accepted_roles=roles, backends=("fake-byte-runner",),
            queue_depth=0, estimated_wait_ms=0.0, rtt_ms=1.0,
            bandwidth_mbps=1000.0)
        event(
            "SPEC163_SIGNED_OFFER", provider=identity,
            status=decision.status, heldMiB=ledger.held_mib())
        return decision

    def execute(context, payload: bytes) -> None:
        selected = DISelectionAssignmentV2.from_bytes(
            context.assignment.assignment_payload)
        role_names = tuple(role.role for role in selected.roles)
        event(
            "SPEC163_COLLAB_HANDLER", provider=identity,
            roles=",".join(role_names), payloadBytes=len(payload))
        if "source" in role_names:
            participant.wait_for_preparation(timeout=5)
            participant.mark_input_ready("source")
            publication_started_ns = time.perf_counter_ns()
            context.publish(
                "fanout", "/activation/fanout/source", b"source-output")
            event(
                "SPEC163_OUTPUT_PUBLISHED", provider=identity, role="source",
                durationMs=(
                    f"{(time.perf_counter_ns() - publication_started_ns) / 1e6:.6f}"),
                payloadBytes=13)
        elif "right" in role_names:
            fetch_started_ns = time.perf_counter_ns()
            source = context.wait_one(
                "fanout", "/activation/fanout/source", 5000)
            if source is None:
                raise RuntimeError("right role did not receive source output")
            event(
                "SPEC163_INPUT_ARRIVED", provider=identity, role="right",
                fetchWaitMs=(
                    f"{(time.perf_counter_ns() - fetch_started_ns) / 1e6:.6f}"))
            # Input is deliberately latched before this slow Provider finishes
            # preparation; local-ready later triggers the atomic start.
            participant.mark_input_ready("right")
            participant.wait_for_preparation(timeout=5)
            context.publish(
                "fanin", "/activation/fanin/right", b"right-output")
            event("SPEC163_OUTPUT_PUBLISHED", provider=identity, role="right")
        else:
            # B is locally ready first and then waits for source data.
            participant.wait_for_preparation(timeout=5)
            fetch_started_ns = time.perf_counter_ns()
            source = context.wait_one(
                "fanout", "/activation/fanout/source", 5000)
            if source is None:
                raise RuntimeError("left role did not receive source output")
            event(
                "SPEC163_INPUT_ARRIVED", provider=identity, role="left",
                fetchWaitMs=(
                    f"{(time.perf_counter_ns() - fetch_started_ns) / 1e6:.6f}"))
            participant.mark_input_ready("left")
            context.publish(
                "fanin", "/activation/fanin/left", b"left-output")
            event("SPEC163_OUTPUT_PUBLISHED", provider=identity, role="left")
            remote_right = context.wait_one(
                "fanin", "/activation/fanin/right", 5000)
            if remote_right is None:
                raise RuntimeError("merge role did not receive right branch")
            event(
                "SPEC163_INPUT_ARRIVED", provider=identity,
                role="merge", count=2)
            participant.mark_input_ready("merge")
            context.publish_final_response(b"SPEC163_FAKE_INFERENCE_OK")

    # DI authorizes every role in its signed offer/assignment tuple; the Core
    # handler therefore accepts an opaque multi-role payload instead of trying
    # to parse a single generic text role.
    provider.add_collaboration_handler("/HELLO", [], execute, ack)
    event(
        "SPEC163_PROVIDER_READY", provider=identity,
        roles=",".join(roles))
    return provider.run("/HELLO")


if __name__ == "__main__":
    raise SystemExit(main())
