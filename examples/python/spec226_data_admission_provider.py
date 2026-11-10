#!/usr/bin/env python3
"""Python consumer for the Spec226 direct input-admission MiniNDN check."""

from __future__ import annotations

import argparse
import gc
import hashlib
from pathlib import Path
import signal
import threading
from typing import Mapping

from ndn.encoding import parse_data
import ndnsf

from ndnsf import (  # noqa: E402
    CollaborationInputSpec,
    DataProductReference,
    ServiceProvider,
    VerifiedInputObject,
    _ndnsf,
)


GROUP = "/example/uav"
CONTROLLER = GROUP + "/controller"
CONSUMER = GROUP + "/compute"
SERVICE = "/Spec226/Admission"
SOURCES = {
    "source-a": GROUP + "/source-adapter/UAV1",
    "source-b": GROUP + "/source-adapter/UAV2",
}
EXPECTED_CONTENT = {"source-a": b"\x03", "source-b": b"\x07"}
REPO_ROOT = Path(__file__).resolve().parents[2]


def input_name(producer: str, version: int) -> str:
    return f"{producer}/NDNSF/INPUT/v={version}"


def make_spec(slot: str, producer: str, version: int) -> CollaborationInputSpec:
    return CollaborationInputSpec(
        slot_id=slot,
        reference=DataProductReference(
            object_name=input_name(producer, version),
            producer_name=producer,
        ),
        producer_role="/source",
        key_scope="input",
        max_bytes=4096,
        max_packets=1,
        encoding="signed_exact_packets",
        segment_count=1,
        digest_kind="ordered_wire_sha256",
    )


def verify_admitted_object(row: str, slot: str, value: VerifiedInputObject,
                           version: int) -> bytes:
    producer = SOURCES[slot]
    expected_name = input_name(producer, version)
    packets = value.packets
    if len(packets) != 1:
        raise RuntimeError(f"Spec226 Python packet count mismatch for slot {slot}")
    _, _, packet_content, _ = parse_data(bytes(packets[0]))
    content = bytes(packet_content)
    if (value.name != expected_name or value.producer != producer or
            content != EXPECTED_CONTENT[slot] or not packets[0].readonly):
        raise RuntimeError(f"Spec226 Python input mismatch for slot {slot}")
    wire = bytes(packets[0])
    digest = hashlib.sha256(wire).hexdigest()
    print(
        f"SPEC226_Q1_PYTHON_INPUT row={row} slot={slot} name={value.name} "
        f"producer={value.producer} contentByte={content[0]} "
        f"wireSha256={digest} readonly=true",
        flush=True,
    )
    return content


def build_provider(trust_schema: str) -> ServiceProvider:
    provider = ServiceProvider(
        group=GROUP,
        controller=CONTROLLER,
        provider_prefix=CONSUMER,
        trust_schema=trust_schema,
    )

    @provider.collaboration_handler(SERVICE, ["/source", "/consumer"])
    def consume(context, payload: bytes) -> None:
        row = payload.decode("ascii")
        if row not in {"q1-warm", "q1-missing"}:
            context.fail("SPEC226_PYTHON_UNEXPECTED_ROW")
            return
        version = 2 if row == "q1-missing" else 1
        validator_calls = 0
        validated_contents: dict[str, bytes] = {}

        def validate(by_slot: Mapping[str, VerifiedInputObject]) -> bool:
            nonlocal validator_calls
            validator_calls += 1
            if set(by_slot) != set(SOURCES):
                return False
            try:
                by_slot["unexpected"] = by_slot["source-a"]
            except TypeError:
                pass
            else:
                raise RuntimeError("Spec226 Python validator mapping is mutable")
            for slot in SOURCES:
                validated_contents[slot] = verify_admitted_object(
                    row, slot, by_slot[slot], version
                )
            return True

        inputs = [
            make_spec(slot, producer, version)
            for slot, producer in SOURCES.items()
        ]
        try:
            admitted = context.require_inputs(inputs, validate)
        except Exception as error:
            expected_rejection = row == "q1-missing"
            reason = str(error).replace(" ", "_")
            missing_input_rejection = reason in {
                "INPUT_DEADLINE", "INPUT_TRANSFER_FAILED",
            }
            print(
                f"SPEC226_Q1_ADMISSION status=REJECTED row={row} "
                f"reason={reason} validatorCount={validator_calls}",
                flush=True,
            )
            print(
                f"SPEC226_Q1_COMPUTE row={row} count=0 result=none",
                flush=True,
            )
            context.publish_final_failure_response(b"", str(error))
            expected_outcome = "REJECTED" if expected_rejection else "READY"
            passed = (expected_rejection and validator_calls == 0 and
                      missing_input_rejection)
            print(
                f"SPEC226_Q1_PYTHON_RESULT row={row} "
                f"status={'PASS' if passed else 'FAIL'} "
                f"expected={expected_outcome} validatorCount={validator_calls} "
                "compute=0",
                flush=True,
            )
            print(f"SPEC226_Q1_CONSUMER_DONE row={row}", flush=True)
            return

        if row == "q1-missing":
            context.publish_final_failure_response(
                b"", "SPEC226_MISSING_INPUT_WAS_ADMITTED")
            print(
                f"SPEC226_Q1_PYTHON_RESULT row={row} status=FAIL "
                "expected=REJECTED validatorCount=1 compute=0",
                flush=True,
            )
            print(f"SPEC226_Q1_CONSUMER_DONE row={row}", flush=True)
            return
        if validator_calls != 1 or not admitted.is_current:
            raise RuntimeError("Spec226 Python admitted set is not current and complete")
        source_a = admitted.at("source-a")
        source_b = admitted.at("source-b")
        if (source_a.name != input_name(SOURCES["source-a"], version) or
                source_b.name != input_name(SOURCES["source-b"], version)):
            raise RuntimeError("Spec226 Python slot lookup changed input identity")
        packet_views = [source_a.packets[0], source_b.packets[0]]
        packet_bytes = [bytes(view) for view in packet_views]
        result = sum(validated_contents[slot][0]
                     for slot in ("source-a", "source-b"))
        del source_a, source_b, admitted
        gc.collect()
        if [bytes(view) for view in packet_views] != packet_bytes:
            raise RuntimeError("Spec226 Python packet views lost their owners")
        print(
            f"SPEC226_Q1_ADMISSION status=READY row={row} reason=none "
            f"validatorCount={validator_calls}",
            flush=True,
        )
        print(
            f"SPEC226_Q1_COMPUTE row={row} count=1 result={result}",
            flush=True,
        )
        context.publish_final_response(bytes([result]))
        print(
            f"SPEC226_Q1_PYTHON_RESULT row={row} status="
            f"{'PASS' if result == 10 else 'FAIL'} expected=READY "
            f"validatorCount={validator_calls} compute=1 result={result} "
            "readonly=true lifetime=true slots=source-a,source-b",
            flush=True,
        )
        print(f"SPEC226_Q1_CONSUMER_DONE row={row}", flush=True)

    return provider


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trust-schema", required=True, type=Path)
    args = parser.parse_args()
    extension = Path(_ndnsf.__file__).resolve()
    package = Path(ndnsf.__file__).resolve().parent
    source_package = REPO_ROOT / "pythonWrapper" / "ndnsf"
    if (extension.parent != package or source_package == extension.parent or
            source_package in extension.parents):
        raise RuntimeError("Spec226 Python gate requires the installed ndnsf extension")

    provider = build_provider(str(args.trust_schema.resolve(strict=True)))
    stop_event = threading.Event()
    signal.signal(signal.SIGTERM, lambda _signum, _frame: stop_event.set())
    signal.signal(signal.SIGINT, lambda _signum, _frame: stop_event.set())
    run_thread = provider.start()
    print(f"SPEC226_Q1_PYTHON_EXTENSION path={extension}", flush=True)
    print("SPEC226_Q1_ACTOR_READY role=consumer", flush=True)
    try:
        stop_event.wait()
    finally:
        provider.stop()
        if run_thread is not None:
            run_thread.join(timeout=10)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
