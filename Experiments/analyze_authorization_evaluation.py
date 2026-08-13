#!/usr/bin/env python3
"""Analyze NDNSF authorization evaluation artifacts without promoting claims."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
from typing import Any, Iterable

import jsonschema
import yaml


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CASES = (
    ROOT
    / "specs/172-data-centric-authorization-evaluation/contracts/authorization-cases.yaml"
)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_manifest(input_dir: Path, root: Path = ROOT) -> list[str]:
    """Validate a completed run and every retained byte before analysis."""
    errors: list[str] = []
    manifest_path = input_dir / "manifest.json"
    if not manifest_path.is_file():
        return ["missing manifest.json"]
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        schema = json.loads(
            (
                ROOT
                / "specs/172-data-centric-authorization-evaluation/contracts/experiment-manifest.schema.json"
            ).read_text(encoding="utf-8")
        )
        jsonschema.validate(manifest, schema)
    except (json.JSONDecodeError, jsonschema.ValidationError) as error:
        return [f"invalid manifest: {error.message if hasattr(error, 'message') else error}"]

    if manifest["terminal_status"] != "success":
        errors.append(f"terminal status is {manifest['terminal_status']}")

    for relative, expected_hash in manifest["source_hashes"].items():
        path = Path(relative)
        source_path = path if path.is_absolute() else root / path
        if not source_path.is_file():
            errors.append(f"missing source: {relative}")
        elif _sha256(source_path) != expected_hash:
            errors.append(f"source hash mismatch: {relative}")

    input_root = input_dir.resolve()
    for artifact in manifest["artifacts"]:
        artifact_path = (input_dir / artifact["path"]).resolve()
        try:
            artifact_path.relative_to(input_root)
        except ValueError:
            errors.append(f"artifact escapes run directory: {artifact['path']}")
            continue
        if not artifact_path.is_file():
            errors.append(f"missing artifact: {artifact['path']}")
            continue
        if artifact_path.stat().st_size != artifact["bytes"]:
            errors.append(f"artifact size mismatch: {artifact['path']}")
        if _sha256(artifact_path) != artifact["sha256"]:
            errors.append(f"artifact hash mismatch: {artifact['path']}")
    return errors


def summarize_correctness(
    results: Iterable[dict[str, Any]], cases: Iterable[dict[str, Any]]
) -> dict[str, Any]:
    result_by_id = {item["case_id"]: item for item in results}
    case_list = list(cases)
    failures: list[str] = []
    denied_handler_executions = 0
    for case in case_list:
        case_id = case["case_id"]
        result = result_by_id.get(case_id)
        if result is None:
            failures.append(f"missing result for {case_id}")
            continue
        if result.get("terminal") != case["expected_terminal"]:
            failures.append(f"terminal mismatch for {case_id}")
        if result.get("observed_executions") != case["expected_executions"]:
            failures.append(f"execution mismatch for {case_id}")
        if case["expected_terminal"] == "deny":
            denied_handler_executions += int(result.get("observed_executions") or 0)
    extras = sorted(set(result_by_id) - {case["case_id"] for case in case_list})
    failures.extend(f"unregistered result for {case_id}" for case_id in extras)
    return {
        "registered_cases": len(case_list),
        "observed_cases": len(result_by_id),
        "denied_handler_executions": denied_handler_executions,
        "failures": failures,
        "supported": not failures and denied_handler_executions == 0,
    }


def summarize_onboarding(
    observations: Iterable[dict[str, Any]], expected_repetitions: int
) -> dict[str, Any]:
    rows = list(observations)
    failures: list[str] = []
    if len(rows) != expected_repetitions:
        failures.append(
            f"observed {len(rows)} repetitions, expected {expected_repetitions}"
        )
    reference_hashes = rows[0].get("provider_local_hashes") if rows else None
    for row in rows:
        repetition = row.get("repetition", "unknown")
        expected = {
            "stale_terminal": "deny",
            "refreshed_terminal": "allow",
            "stale_executions": 0,
            "refreshed_executions": 1,
            "provider_manual_changes": 0,
            "refresh_operations": 1,
        }
        for key, value in expected.items():
            if row.get(key) != value:
                failures.append(
                    f"repetition {repetition} {key}: observed={row.get(key)!r} expected={value!r}"
                )
        if row.get("new_epoch", 0) <= row.get("old_epoch", 0):
            failures.append(f"repetition {repetition} did not advance policy epoch")
        if row.get("control_bytes", 0) <= 0:
            failures.append(f"repetition {repetition} has no refresh control bytes")
        if row.get("provider_local_hashes") != reference_hashes:
            failures.append(f"repetition {repetition} changed Provider-local hashes")
    return {
        "repetitions": len(rows),
        "provider_hashes_unchanged": bool(rows) and all(
            row.get("provider_local_hashes") == reference_hashes for row in rows
        ),
        "manual_provider_changes": sum(
            int(row.get("provider_manual_changes", 0)) for row in rows
        ),
        "refresh_operations": sum(
            int(row.get("refresh_operations", 0)) for row in rows
        ),
        "control_bytes": [row.get("control_bytes") for row in rows],
        "time_to_first_success_us": [
            row.get("time_to_first_success_us") for row in rows
        ],
        "failures": failures,
        "supported": not failures,
    }


def summarize_minindn(observations: Iterable[dict[str, Any]]) -> dict[str, Any]:
    """Validate the paired network onboarding confirmation."""
    rows = {row.get("case_id"): row for row in observations}
    failures: list[str] = []
    expected = {
        "pre_onboarding_no_user": ("deny", "abe_key_readiness", 0, False),
        "post_onboarding_authorized": (
            "allow",
            "response_acceptance",
            1,
            True,
        ),
    }
    for case_id, (terminal, gate, executions, request_attempted) in expected.items():
        row = rows.get(case_id)
        if row is None:
            failures.append(f"missing MiniNDN case {case_id}")
            continue
        if row.get("terminal") != terminal:
            failures.append(f"{case_id} terminal mismatch")
        if row.get("terminal_gate") != gate:
            failures.append(f"{case_id} terminal gate mismatch")
        if row.get("provider_executions") != executions:
            failures.append(f"{case_id} provider execution mismatch")
        if row.get("request_send_attempted") is not request_attempted:
            failures.append(f"{case_id} request publication mismatch")
    extras = sorted(set(rows) - set(expected))
    failures.extend(f"unregistered MiniNDN case {case_id}" for case_id in extras)

    pre = rows.get("pre_onboarding_no_user", {})
    post = rows.get("post_onboarding_authorized", {})
    hashes_unchanged = bool(pre and post) and (
        pre.get("provider_local_hashes") == post.get("provider_local_hashes")
    )
    if not hashes_unchanged:
        failures.append("Provider-local hashes changed between MiniNDN cases")
    if pre and post and post.get("policy_epoch", 0) <= pre.get("policy_epoch", 0):
        failures.append("Controller policy epoch did not advance")

    return {
        "cases": len(rows),
        "provider_hashes_unchanged": hashes_unchanged,
        "pre_provider_executions": pre.get("provider_executions"),
        "post_provider_executions": post.get("provider_executions"),
        "failures": failures,
        "supported": not failures,
    }


def summarize_publication_audit(
    observations: Iterable[dict[str, Any]],
) -> dict[str, Any]:
    """Validate one composed Request/ACK/Selection/Response transaction."""
    rows = list(observations)
    unique_rows = list(
        {
            json.dumps(row, sort_keys=True, separators=(",", ":")): row
            for row in rows
        }.values()
    )
    duplicate_observations = len(rows) - len(unique_rows)
    failures: list[str] = []
    expected_roles = {
        "REQUEST": "provider",
        "ACK": "user",
        "SELECTION": "provider",
        "RESPONSE": "user",
    }
    by_type: dict[str, list[dict[str, Any]]] = {
        message_type: [] for message_type in expected_roles
    }
    for row in unique_rows:
        message_type = row.get("message_type")
        if message_type not in by_type:
            failures.append(f"unexpected publication type {message_type!r}")
            continue
        by_type[message_type].append(row)

    for message_type, matches in by_type.items():
        if len(matches) != 1:
            failures.append(
                f"expected one {message_type} publication, observed {len(matches)}"
            )

    complete_rows = [matches[0] for matches in by_type.values() if len(matches) == 1]
    request_ids = {row.get("request_id") for row in complete_rows}
    service_names = {row.get("service_name") for row in complete_rows}
    requester_names = {row.get("requester_name") for row in complete_rows}
    provider_names = {row.get("provider_name") for row in complete_rows}
    if len(request_ids) != 1 or None in request_ids:
        failures.append("publication audit does not identify one request ID")
    if len(service_names) != 1 or None in service_names:
        failures.append("publication audit does not identify one service name")
    if len(requester_names) != 1 or None in requester_names:
        failures.append("publication audit does not identify one requester")
    if len(provider_names) != 1 or None in provider_names:
        failures.append("publication audit does not identify one provider")

    for message_type, matches in by_type.items():
        if len(matches) != 1:
            continue
        row = matches[0]
        requester = str(row.get("requester_name") or "").rstrip("/")
        provider = str(row.get("provider_name") or "").rstrip("/")
        producer = requester if message_type in {"REQUEST", "SELECTION"} else provider
        if row.get("role") != expected_roles[message_type]:
            failures.append(f"{message_type} receiver role mismatch")
        if row.get("validated") is not True:
            failures.append(f"{message_type} was not validator-approved")
        if row.get("packet_present") is not True:
            failures.append(f"{message_type} lacks the validated Data packet")
        if not str(row.get("packet_name") or "").startswith(
            f"{producer}/NDNSF/{message_type}/"
        ):
            failures.append(f"{message_type} Data name is outside producer namespace")
        producer_prefix = str(row.get("producer_prefix") or "")
        if producer_prefix != producer and not producer_prefix.startswith(f"{producer}/"):
            failures.append(f"{message_type} SVS producer prefix mismatch")
        if not str(row.get("signer_key_locator") or "").startswith(
            f"{producer}/KEY/"
        ):
            failures.append(f"{message_type} KeyLocator identity mismatch")
        if int(row.get("seq_no") or 0) <= 0:
            failures.append(f"{message_type} lacks a positive SVS sequence number")
        wire_digest = str(row.get("wire_digest") or "")
        if not wire_digest.startswith("sha256:") or len(wire_digest) <= len("sha256:"):
            failures.append(f"{message_type} lacks a wire digest")

    return {
        "observations": len(rows),
        "messages": len(unique_rows),
        "duplicate_observations": duplicate_observations,
        "message_types": {
            message_type: len(matches) for message_type, matches in by_type.items()
        },
        "request_id": next(iter(request_ids)) if len(request_ids) == 1 else None,
        "service_name": next(iter(service_names)) if len(service_names) == 1 else None,
        "failures": failures,
        "supported": not failures,
    }


def _distribution(values: Iterable[float]) -> dict[str, float | None]:
    ordered = sorted(float(value) for value in values)
    if not ordered:
        return {"median": None, "p95": None, "maximum": None}
    p95_index = max(0, math.ceil(0.95 * len(ordered)) - 1)
    return {
        "median": statistics.median(ordered),
        "p95": ordered[p95_index],
        "maximum": ordered[-1],
    }


def summarize_overhead(
    observations: Iterable[dict[str, Any]], expected_repetitions: int
) -> dict[str, Any]:
    rows = list(observations)
    failures: list[str] = []
    expected_scale_points = {
        (users, providers)
        for users in (1, 10, 100)
        for providers in (1, 4, 16)
    }
    if len(rows) != expected_repetitions:
        failures.append(
            f"observed {len(rows)} repetitions, expected {expected_repetitions}"
        )
    for row in rows:
        repetition = row.get("repetition", "unknown")
        if row.get("cold_latency_ms") is None:
            failures.append(f"repetition {repetition} lacks cold observation")
        if not row.get("warm_latencies_ms"):
            failures.append(f"repetition {repetition} lacks warm observations")
        if row.get("failures", 0) != 0:
            failures.append(f"repetition {repetition} has request failures")
        counters = row.get("crypto_counters", {})
        if counters.get("decrypt_failures", 0) != 0:
            failures.append(f"repetition {repetition} has crypto failures")
        if row.get("protected_content_bytes", 0) <= 0:
            failures.append(f"repetition {repetition} lacks wire-byte evidence")
        observed_scale_points = {
            (item.get("users"), item.get("providers"))
            for item in row.get("provisioning_scale", [])
        }
        if observed_scale_points != expected_scale_points:
            failures.append(
                f"repetition {repetition} lacks registered provisioning scale points"
            )
    cold = [row["cold_latency_ms"] for row in rows if row.get("cold_latency_ms") is not None]
    warm = [
        value for row in rows for value in row.get("warm_latencies_ms", [])
    ]
    per_repetition = [
        {
            "repetition": row.get("repetition"),
            "cold_latency_ms": row.get("cold_latency_ms"),
            "warm_requests": len(row.get("warm_latencies_ms", [])),
            "warm_latency_ms": _distribution(row.get("warm_latencies_ms", [])),
        }
        for row in rows
    ]
    scale_summary = []
    for users, providers in sorted(expected_scale_points):
        points = [
            item
            for row in rows
            for item in row.get("provisioning_scale", [])
            if item.get("users") == users and item.get("providers") == providers
        ]
        scale_summary.append(
            {
                "users": users,
                "providers": providers,
                "policy_terms": providers,
                "total_us": _distribution(item["total_us"] for item in points),
                "response_bytes": points[0]["response_bytes"] if points else None,
                "encrypted_bytes": points[0]["encrypted_bytes"] if points else None,
            }
        )
    return {
        "repetitions": len(rows),
        "cold_latency_ms": _distribution(cold),
        "cold_latency_range_ms": [min(cold), max(cold)] if cold else [None, None],
        "warm_latency_ms": _distribution(warm),
        "warm_run_median_ms": _distribution(
            item["warm_latency_ms"]["median"]
            for item in per_repetition
            if item["warm_latency_ms"]["median"] is not None
        ),
        "per_repetition": per_repetition,
        "provisioning_scale": scale_summary,
        "failures": failures,
        "supported": not failures,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, action="append", required=True)
    parser.add_argument("--cases", type=Path, default=DEFAULT_CASES)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    cases = yaml.safe_load(args.cases.read_text(encoding="utf-8"))["cases"]
    analyses = []
    for input_dir in args.input:
        manifest_failures = validate_manifest(input_dir)
        correctness_path = input_dir / "case-results.json"
        onboarding_path = input_dir / "onboarding-results.json"
        overhead_path = input_dir / "overhead-results.json"
        if correctness_path.is_file():
            payload = json.loads(correctness_path.read_text(encoding="utf-8"))
            analysis = {
                "kind": "correctness",
                **summarize_correctness(payload["cases"], cases),
            }
        elif onboarding_path.is_file():
            payload = json.loads(onboarding_path.read_text(encoding="utf-8"))
            manifest = json.loads(
                (input_dir / "manifest.json").read_text(encoding="utf-8")
            )
            expected_repetitions = int(
                manifest.get("configuration", {}).get("repetitions", 0)
            )
            analysis = {
                "kind": "onboarding",
                **summarize_onboarding(
                    payload["observations"], expected_repetitions
                ),
            }
        elif overhead_path.is_file():
            payload = json.loads(overhead_path.read_text(encoding="utf-8"))
            manifest = json.loads(
                (input_dir / "manifest.json").read_text(encoding="utf-8")
            )
            expected_repetitions = int(
                manifest.get("configuration", {}).get("repetitions", 0)
            )
            analysis = {
                "kind": "overhead",
                **summarize_overhead(
                    payload["observations"], expected_repetitions
                ),
            }
        elif (input_dir / "minindn-results.json").is_file():
            payload = json.loads(
                (input_dir / "minindn-results.json").read_text(encoding="utf-8")
            )
            minindn_summary = summarize_minindn(payload["observations"])
            post_observation = next(
                (
                    row
                    for row in payload["observations"]
                    if row.get("case_id") == "post_onboarding_authorized"
                ),
                {},
            )
            publication_audit = summarize_publication_audit(
                post_observation.get("publication_audit", [])
            )
            minindn_summary["publication_audit"] = publication_audit
            minindn_summary["failures"].extend(
                f"publication audit: {failure}"
                for failure in publication_audit["failures"]
            )
            minindn_summary["supported"] = not minindn_summary["failures"]
            analysis = {
                "kind": "minindn",
                **minindn_summary,
            }
        else:
            analysis = {
                "kind": "unknown",
                "failures": ["no recognized result payload"],
                "supported": False,
            }
        analysis["failures"] = manifest_failures + analysis["failures"]
        analysis["supported"] = not analysis["failures"]
        analyses.append({"input": str(input_dir), **analysis})
    report = {
        "schema_version": 1,
        "analyses": analyses,
        "supported": all(item["supported"] for item in analyses),
    }
    rendered = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0 if report["supported"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
