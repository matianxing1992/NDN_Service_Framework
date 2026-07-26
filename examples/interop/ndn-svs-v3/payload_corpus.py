#!/usr/bin/env python3
"""Deterministic byte oracle for C++/NDNts SVS-PS interoperability."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any, Iterable


SCHEMA_VERSION = "spec117-payload-corpus-v1"
SUMMARY_SCHEMA_VERSION = "spec117-payload-summary-v1"
APPLICATION_PREFIX = "/ndnsf/svs-pubsub-interop/payload"
SENDERS = ("cpp", "ndnts")
DIRECTIONS = (("cpp", "ndnts"), ("ndnts", "cpp"))


def _deterministic_bytes(label: str, length: int) -> bytes:
    output = bytearray()
    counter = 0
    while len(output) < length:
        output.extend(hashlib.sha256(f"spec117:{label}:{counter}".encode()).digest())
        counter += 1
    return bytes(output[:length])


def corpus_payloads() -> list[tuple[str, bytes, bool, int]]:
    return [
        ("text", "NDNSF ↔ NDNts SVS-PS payload: 你好\n".encode("utf-8"), False, 4096),
        ("binary", bytes(range(256)) * 4, False, 4096),
        ("large", _deterministic_bytes("large", 4096), False, 4096),
        ("segmented", _deterministic_bytes("segmented", 32768), True, 4096),
    ]


def _manifest_case(case_id: str, payload: bytes, segmented: bool,
                   segment_hint: int) -> dict[str, Any]:
    return {
        "caseId": case_id,
        "path": f"payloads/{case_id}.bin",
        "names": {
            sender: f"{APPLICATION_PREFIX}/{sender}/{case_id}" for sender in SENDERS
        },
        "length": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
        "requiresSegmentation": segmented,
        "segmentHint": segment_hint,
    }


def create_corpus(output: Path) -> dict[str, Any]:
    output = Path(output)
    payload_dir = output / "payloads"
    payload_dir.mkdir(parents=True, exist_ok=True)
    cases = []
    for case_id, payload, segmented, segment_hint in corpus_payloads():
        case = _manifest_case(case_id, payload, segmented, segment_hint)
        (output / case["path"]).write_bytes(payload)
        cases.append(case)
    manifest = {
        "schemaVersion": SCHEMA_VERSION,
        "applicationPrefix": APPLICATION_PREFIX,
        "senders": list(SENDERS),
        "cases": cases,
    }
    (output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def load_manifest(path: Path) -> dict[str, Any]:
    path = Path(path)
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("schemaVersion") != SCHEMA_VERSION:
        raise ValueError("unsupported payload manifest schema")
    cases = manifest.get("cases")
    if not isinstance(cases, list) or [c.get("caseId") for c in cases] != \
            ["text", "binary", "large", "segmented"]:
        raise ValueError("payload manifest cases are invalid")
    seen_names: set[str] = set()
    for case in cases:
        payload_path = path.parent / str(case.get("path", ""))
        if not payload_path.is_file():
            raise ValueError(f"payload file is missing: {payload_path}")
        payload = payload_path.read_bytes()
        if len(payload) != int(case.get("length", -1)):
            raise ValueError(f"payload length mismatch: {case.get('caseId')}")
        if hashlib.sha256(payload).hexdigest() != case.get("sha256"):
            raise ValueError(f"payload digest mismatch: {case.get('caseId')}")
        names = case.get("names")
        if not isinstance(names, dict) or set(names) != set(SENDERS):
            raise ValueError(f"payload names are invalid: {case.get('caseId')}")
        for name in names.values():
            if not isinstance(name, str) or not name.startswith("/") or name in seen_names:
                raise ValueError(f"payload name is invalid or duplicated: {name!r}")
            seen_names.add(name)
        if case.get("requiresSegmentation") and int(case.get("length", 0)) <= 8800:
            raise ValueError("segmented payload does not exceed one NDN packet")
    return manifest


def _expected_receipts(manifest: dict[str, Any]) -> dict[tuple[str, str], dict[str, Any]]:
    expected: dict[tuple[str, str], dict[str, Any]] = {}
    for sender, receiver in DIRECTIONS:
        direction = f"{sender}-to-{receiver}"
        for case in manifest["cases"]:
            expected[(direction, case["caseId"])] = {
                "direction": direction,
                "caseId": case["caseId"],
                "name": case["names"][sender],
                "length": case["length"],
                "sha256": case["sha256"],
                "requiresSegmentation": bool(case["requiresSegmentation"]),
            }
    return expected


def classify_receipts(manifest: dict[str, Any], events: Iterable[dict[str, Any]]) \
        -> dict[str, Any]:
    expected = _expected_receipts(manifest)
    observed: dict[tuple[str, str], list[dict[str, Any]]] = {}
    errors = []
    for event in events:
        if event.get("event") in {"error", "reject", "infra-error"}:
            errors.append(event)
        if event.get("event") != "receive":
            continue
        key = (str(event.get("direction", "")), str(event.get("caseId", "")))
        observed.setdefault(key, []).append(event)

    duplicates = []
    mismatches = []
    verified = []
    for key, receipts in sorted(observed.items()):
        if len(receipts) > 1:
            duplicates.append({"direction": key[0], "caseId": key[1], "count": len(receipts)})
        expectation = expected.get(key)
        if expectation is None:
            mismatches.append({"direction": key[0], "caseId": key[1],
                               "reason": "unexpected receipt"})
            continue
        failed_fields: set[str] = set()
        for receipt in receipts:
            failed_fields.update(field for field in ("name", "length", "sha256")
                                 if receipt.get(field) != expectation[field])
            if expectation["requiresSegmentation"] and int(receipt.get("segments", 0)) <= 1:
                failed_fields.add("segments")
        if failed_fields:
            mismatches.append({"direction": key[0], "caseId": key[1],
                               "fields": sorted(failed_fields)})
        if len(receipts) == 1 and not failed_fields:
            verified.append({"direction": key[0], "caseId": key[1]})

    verified_keys = {(item["direction"], item["caseId"]) for item in verified}
    missing = [{"direction": key[0], "caseId": key[1]}
               for key in sorted(expected) if key not in verified_keys]
    passed = not missing and not duplicates and not mismatches and not errors
    infra_failure = any(event.get("event") == "infra-error" or
                        event.get("stage") == "orchestration" for event in errors)
    return {
        "schemaVersion": SUMMARY_SCHEMA_VERSION,
        "status": "SUCCESS" if passed else
                  ("INFRA_FAILURE" if infra_failure else "INTEROP_INCOMPATIBLE"),
        "passed": passed,
        "expectedReceiptCount": len(expected),
        "verifiedReceiptCount": len(verified),
        "verified": verified,
        "missing": missing,
        "duplicates": duplicates,
        "mismatches": mismatches,
        "errors": errors,
    }


def _read_jsonl(paths: Iterable[Path]) -> list[dict[str, Any]]:
    events = []
    for path in paths:
        if not path.is_file():
            continue
        events.extend(json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()
                      if line.strip())
    return events


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    create = sub.add_parser("create")
    create.add_argument("--output", type=Path, required=True)
    classify = sub.add_parser("classify")
    classify.add_argument("--manifest", type=Path, required=True)
    classify.add_argument("--events", type=Path, nargs="+", required=True)
    classify.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if args.command == "create":
        manifest = create_corpus(args.output)
        print(json.dumps(manifest, sort_keys=True))
        return 0

    manifest = load_manifest(args.manifest)
    summary = classify_receipts(manifest, _read_jsonl(args.events))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    print(json.dumps(summary, sort_keys=True))
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
