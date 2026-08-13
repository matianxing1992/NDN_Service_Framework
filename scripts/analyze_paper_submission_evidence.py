#!/usr/bin/env python3
"""Normalize, aggregate, and audit the frozen Spec 173 evidence campaign."""

from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import statistics
import sys
from typing import Any

import jsonschema
import yaml


REPO = Path(__file__).resolve().parents[1]
DEFAULT_REGISTRATION = (
    REPO / "specs/173-paper-submission-evidence/contracts/experiment-registration.yaml"
)
DEFAULT_SCHEMA = (
    REPO / "specs/173-paper-submission-evidence/contracts/artifact-index.schema.json"
)
DEFAULT_INDEX = (
    REPO / "specs/173-paper-submission-evidence/evidence/artifact-index.json"
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_registration(path: Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text())
    if not isinstance(value, dict) or value.get("schemaVersion") != 1:
        raise ValueError("unsupported Spec 173 experiment registration")
    return value


def parse_numeric_fields(line: str) -> dict[str, float | int]:
    result: dict[str, float | int] = {}
    for key, value in re.findall(r"([A-Za-z0-9_]+)=(-?[0-9]+(?:\.[0-9]+)?)", line):
        result[key] = float(value) if "." in value else int(value)
    return result


def verify_retained_cell(cell_dir: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    manifest_path = cell_dir / "cell-manifest.json"
    result_path = cell_dir / "cell-result.json"
    if not manifest_path.is_file() or not result_path.is_file():
        raise ValueError(f"missing cell metadata in {cell_dir}")
    manifest = json.loads(manifest_path.read_text())
    result = json.loads(result_path.read_text())
    if result.get("status") != "valid" or result.get("exitCode") != 0:
        raise ValueError(f"cell is not terminal valid: {cell_dir}")
    hashes = result.get("artifactHashes")
    if not isinstance(hashes, dict) or not hashes:
        raise ValueError(f"cell has no retained hashes: {cell_dir}")
    for relative, expected in hashes.items():
        path = cell_dir / relative
        if not path.is_file() or sha256_file(path) != expected:
            raise ValueError(f"retained hash mismatch: {cell_dir}/{relative}")
    for relative in result.get("requiredSummaries", []):
        path = cell_dir / relative
        if not path.is_file():
            raise ValueError(f"required summary missing: {cell_dir}/{relative}")
        json.loads(path.read_text())
    return manifest, result


def normalized_common(
    manifest: dict[str, Any], result: dict[str, Any], scheduled: int,
    issued: int, admitted: Any, successful: int, timed_out: int,
    other_failed: int, pending: int, duration: float,
) -> dict[str, Any]:
    return {
        "runId": manifest["cellId"],
        "comparison": manifest["comparison"],
        "system": manifest["system"],
        "repetition": manifest["repetition"],
        "seed": manifest["seed"],
        "rateRps": manifest["rateRps"],
        "scheduledRequests": scheduled,
        "issuedRequests": issued,
        "admittedRequests": admitted,
        "successfulRequests": successful,
        "timedOutRequests": timed_out,
        "otherFailedRequests": other_failed,
        "pendingRequests": pending,
        "throughputSuccessfulRps": successful / duration if duration else 0.0,
        "successRateIssued": successful / issued if issued else 0.0,
        "completionRateScheduled": successful / scheduled if scheduled else 0.0,
        "sourceRevisions": manifest.get("sourceRevisions", {}),
        "registrationSha256": manifest.get("registrationSha256", ""),
        "toolchainManifestSha256": manifest.get("toolchainManifestSha256", ""),
        "exactCommand": manifest.get("exactCommand", []),
        "rawArtifactHashes": result.get("artifactHashes", {}),
    }


def normalize_cell(cell_dir: Path, registration: dict[str, Any]) -> dict[str, Any]:
    manifest, result = verify_retained_cell(cell_dir)
    raw = json.loads((cell_dir / "summary.json").read_text())
    duration = float(registration["commonConditions"]["measuredSeconds"])
    scheduled = int(round(float(manifest["rateRps"] or 0) * duration))
    system = manifest["system"]

    if system.startswith("ndnsf"):
        issued = int(raw.get("sent_count", raw.get("total_requests_sent", 0)))
        successful = int(raw.get("total_successful_responses", 0))
        timed_out = int(raw.get("timed_out_count", raw.get("timeout_count", 0)))
        pending = int(raw.get("pending_at_shutdown", 0))
        other_failed = issued - successful - timed_out - pending
        admission_enabled = system == "ndnsf-admission-enabled"
        admitted = issued if admission_enabled else None
        normalized = normalized_common(
            manifest, result, scheduled, issued, admitted, successful,
            timed_out, other_failed, pending, duration,
        )
        normalized.update({
            "latencySampleCount": successful,
            "latencyMeanMs": float(raw.get("average_latency_ms", 0.0)),
            "latencyP50Ms": float(raw.get("p50_latency_ms", 0.0)),
            "latencyP95Ms": float(raw.get("p95_latency_ms", 0.0)),
            "providerSelectionCounts": raw.get("selected_provider_distribution", {}),
            "providerExecutionCounts": raw.get("provider_final_response_count", {}),
            "admissionCountersPresent": all(key in raw for key in (
                "pause_count",
                "admission_queue_pause_skips",
                "admission_recommended_rate_skips",
            )) if admission_enabled else False,
            "admissionPauseEvents": int(raw.get("pause_count", 0)),
            "maximumQueueDepth": int(raw.get("user_queued_task_max", 0)),
        })
    elif system == "grpc":
        client_log = (cell_dir / "client.log").read_text(errors="replace") \
            if (cell_dir / "client.log").is_file() else ""
        rate_lines = [
            parse_numeric_fields(line) for line in client_log.splitlines()
            if line.startswith("GRPC_CLIENT_RATE")
        ]
        rate_summary = rate_lines[-1] if rate_lines else {}
        latency = parse_numeric_fields(str(raw.get("summary_line", "")))
        issued = int(rate_summary.get("sent", raw.get("count", scheduled)))
        successful = int(rate_summary.get("success", latency.get("count", 0)))
        terminal_failures = int(rate_summary.get("failures", issued - successful))
        timed_out = len(re.findall(r"StatusCode\.DEADLINE_EXCEEDED", client_log))
        timed_out = min(timed_out, terminal_failures)
        pending = 0
        other_failed = terminal_failures - timed_out
        normalized = normalized_common(
            manifest, result, scheduled, issued, None, successful,
            timed_out, other_failed, pending, duration,
        )
        normalized.update({
            "latencySampleCount": successful,
            "latencyMeanMs": float(latency.get("avg_ms", 0.0)),
            "latencyP50Ms": float(latency.get("p50_ms", 0.0)),
            "latencyP95Ms": float(latency.get("p95_ms", 0.0)),
            "providerSelectionCounts": {},
            "providerExecutionCounts": {},
            "admissionCountersPresent": False,
        })
    elif system == "nsc":
        summaries = raw.get("summaries", [])
        if len(summaries) != 1:
            raise ValueError(f"expected one NSC rate summary in {cell_dir}")
        summary = summaries[0]
        issued = int(summary.get("count", 0))
        successful = int(summary.get("success", 0))
        timed_out = int(summary.get("timeout", 0))
        pending = 0
        other_failed = issued - successful - timed_out
        normalized = normalized_common(
            manifest, result, scheduled, issued, None, successful,
            timed_out, other_failed, pending, duration,
        )
        normalized.update({
            "latencySampleCount": successful,
            "latencyMeanMs": float(summary.get("avg_ms", 0.0)),
            "latencyP50Ms": float(summary.get("p50_ms", 0.0)),
            "latencyP95Ms": float(summary.get("p95_ms", 0.0)),
            "providerSelectionCounts": {},
            "providerExecutionCounts": {},
            "admissionCountersPresent": False,
        })
    elif system == "regression":
        normalized = normalized_common(
            manifest, result, 0, 0, None, 0, 0, 0, 0, duration,
        )
        normalized.update({
            "regressionPass": bool(raw.get("regressionPass")),
            "latencySampleCount": 0,
            "latencyMeanMs": None,
            "latencyP50Ms": None,
            "latencyP95Ms": None,
            "providerSelectionCounts": {},
            "providerExecutionCounts": {},
            "admissionCountersPresent": False,
        })
    else:
        raise ValueError(f"unsupported system: {system}")

    valid, reasons = validate_normalized_run(normalized, registration)
    normalized["valid"] = valid
    normalized["validityReasons"] = reasons
    return normalized


def validate_normalized_run(
    run: dict[str, Any], registration: dict[str, Any],
) -> tuple[bool, list[str]]:
    reasons = []
    count_keys = (
        "scheduledRequests", "issuedRequests", "successfulRequests",
        "timedOutRequests", "otherFailedRequests", "pendingRequests",
    )
    for key in count_keys:
        if int(run.get(key, -1)) < 0:
            reasons.append(f"{key} is negative or missing")
    issued = int(run.get("issuedRequests", 0))
    terminal = sum(int(run.get(key, 0)) for key in (
        "successfulRequests", "timedOutRequests", "otherFailedRequests", "pendingRequests"
    ))
    if issued != terminal:
        reasons.append(f"outcome counts do not reconcile: issued={issued}, outcomes={terminal}")

    comparison = run.get("comparison")
    system = run.get("system")
    offered_rule = (
        comparison in ("one-provider-baseline", "custom-selection") or
        (comparison == "admission-control" and system == "ndnsf-admission-disabled")
    )
    scheduled = int(run.get("scheduledRequests", 0))
    if offered_rule and scheduled > 0 and issued < 0.80 * scheduled:
        reasons.append("issued requests are below the registered 80% offered-load threshold")
    if comparison == "admission-control" and system == "ndnsf-admission-enabled":
        if scheduled <= 0:
            reasons.append("intended scheduler tick count is missing")
        if not run.get("admissionCountersPresent"):
            reasons.append("admission counters are missing")
    if comparison == "selective-ack-correctness" and not run.get("regressionPass", False):
        reasons.append("selective ACK correctness regression failed")
    return not reasons, reasons


AGGREGATE_METRICS = (
    "throughputSuccessfulRps",
    "successRateIssued",
    "completionRateScheduled",
    "latencyMeanMs",
    "latencyP50Ms",
    "latencyP95Ms",
)


def descriptive(values: list[float]) -> dict[str, Any]:
    return {
        "n": len(values),
        "mean": statistics.mean(values),
        "sampleSd": statistics.stdev(values) if len(values) > 1 else None,
        "min": min(values),
        "max": max(values),
    }


def aggregate_repetitions(runs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    groups: dict[tuple[Any, Any, Any], list[dict[str, Any]]] = defaultdict(list)
    for run in runs:
        if run.get("valid"):
            groups[(run["comparison"], run["rateRps"], run["system"])].append(run)
    output = []
    for key in sorted(groups, key=lambda item: (str(item[0]), float(item[1] or 0), str(item[2]))):
        members = sorted(groups[key], key=lambda item: item["repetition"])
        metrics = {}
        for metric in AGGREGATE_METRICS:
            values = [float(item[metric]) for item in members if item.get(metric) is not None]
            if values:
                metrics[metric] = descriptive(values)
        output.append({
            "comparison": key[0],
            "rateRps": key[1],
            "system": key[2],
            "independentUnit": "process-repetition",
            "repetitions": [item["repetition"] for item in members],
            "runValues": [
                {"repetition": item["repetition"], **{
                    metric: item.get(metric) for metric in AGGREGATE_METRICS
                }} for item in members
            ],
            "metrics": metrics,
            "significanceClaimAllowed": False,
            "significanceNote": "The registered n=3 analysis is descriptive only.",
        })
    return output


def validate_artifact_index(index: dict[str, Any], schema_path: Path = DEFAULT_SCHEMA) -> None:
    schema = json.loads(schema_path.read_text())
    jsonschema.validate(index, schema, format_checker=jsonschema.FormatChecker())


def tex_corpus(tex_path: Path) -> str:
    texts = [tex_path.read_text(errors="replace")]
    for path in sorted(tex_path.parent.rglob("*.tex")):
        if path != tex_path:
            texts.append(path.read_text(errors="replace"))
    return "\n".join(texts)


def audit_manuscript_precision(
    tex_path: Path, artifact_index: dict[str, Any],
) -> list[dict[str, Any]]:
    corpus = tex_corpus(tex_path)
    findings = []
    for entry in artifact_index["entries"]:
        identifier = entry["manuscriptId"]
        if f"\\label{{{identifier}}}" not in corpus:
            continue
        if entry["status"] not in ("supported", "qualified"):
            findings.append({
                "manuscriptId": identifier,
                "status": entry["status"],
                "location": entry["location"],
                "reason": entry["reason"],
            })
    return findings


def write_canonical(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def analyze_campaign(
    campaign_root: Path, output_root: Path, registration_path: Path,
) -> dict[str, Any]:
    registration = load_registration(registration_path)
    normalized = []
    exclusions = []
    runs_root = campaign_root / "runs"
    for block_root in sorted(runs_root.glob("*")) if runs_root.is_dir() else []:
        attempts = sorted(block_root.glob("attempt-[0-9][0-9][0-9][0-9]"))
        if not attempts:
            exclusions.append({"blockId": block_root.name, "reason": "no attempts"})
            continue
        for superseded in attempts[:-1]:
            exclusions.append({
                "blockId": block_root.name,
                "attempt": superseded.name,
                "reason": "superseded-attempt",
            })
        attempt = attempts[-1]
        try:
            block_result = json.loads((attempt / "block-result.json").read_text())
        except Exception as error:
            exclusions.append({
                "blockId": block_root.name,
                "attempt": attempt.name,
                "reason": f"missing-or-invalid-block-result: {error}",
            })
            continue
        if block_result.get("status") != "valid":
            exclusions.append({
                "blockId": block_root.name,
                "attempt": attempt.name,
                "reason": "infrastructure-invalid",
                "detail": block_result.get("reason", ""),
            })
            continue
        for cell_dir in sorted(path for path in attempt.iterdir() if path.is_dir()):
            try:
                run = normalize_cell(cell_dir, registration)
                normalized.append(run)
                if not run["valid"]:
                    exclusions.append({
                        "runId": run["runId"],
                        "reason": "analysis-invalid",
                        "detail": run["validityReasons"],
                    })
            except Exception as error:
                exclusions.append({
                    "blockId": block_root.name,
                    "attempt": attempt.name,
                    "cell": cell_dir.name,
                    "reason": f"normalization-error: {error}",
                })

    normalized.sort(key=lambda item: item["runId"])
    aggregates = aggregate_repetitions(normalized)
    output_root.mkdir(parents=True, exist_ok=True)
    normalized_path = output_root / "normalized-runs.json"
    aggregate_path = output_root / "aggregate-statistics.json"
    exclusions_path = output_root / "exclusions.json"
    write_canonical(normalized_path, {"schemaVersion": 1, "runs": normalized})
    write_canonical(aggregate_path, {"schemaVersion": 1, "aggregates": aggregates})
    write_canonical(exclusions_path, {"schemaVersion": 1, "exclusions": exclusions})

    index_path = output_root / "artifact-index.json"
    if not index_path.is_file():
        index_path = DEFAULT_INDEX
    validate_artifact_index(json.loads(index_path.read_text()), DEFAULT_SCHEMA)
    manifest = {
        "schemaVersion": 1,
        "generatedAt": registration["frozenAt"],
        "generationSemantics": "deterministic-from-frozen-registration",
        "campaignRoot": str(campaign_root.resolve()),
        "registrationPath": str(registration_path.resolve()),
        "registrationSha256": sha256_file(registration_path),
        "analyzerSha256": sha256_file(Path(__file__).resolve()),
        "independentUnit": "process-repetition",
        "packetPseudoreplication": False,
        "significanceClaims": False,
        "outputs": {
            "normalized-runs.json": sha256_file(normalized_path),
            "aggregate-statistics.json": sha256_file(aggregate_path),
            "exclusions.json": sha256_file(exclusions_path),
        },
        "artifactIndexPath": str(index_path.resolve()),
        "artifactIndexSha256": sha256_file(index_path),
    }
    manifest_path = output_root / "analysis-manifest.json"
    write_canonical(manifest_path, manifest)
    return manifest


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registration", type=Path, default=DEFAULT_REGISTRATION)
    parser.add_argument("--input", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--audit-manuscript", type=Path)
    parser.add_argument("--artifact-index", type=Path, default=DEFAULT_INDEX)
    parser.add_argument("--artifact-schema", type=Path, default=DEFAULT_SCHEMA)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.audit_manuscript:
        index = json.loads(args.artifact_index.read_text())
        validate_artifact_index(index, args.artifact_schema)
        findings = audit_manuscript_precision(args.audit_manuscript, index)
        print(json.dumps({
            "status": "pass" if not findings else "fail",
            "unsupportedPrecision": findings,
        }, indent=2, sort_keys=True))
        return 0 if not findings else 2
    if args.input is None or args.output is None:
        raise SystemExit("--input and --output are required for campaign analysis")
    manifest = analyze_campaign(
        args.input.resolve(), args.output.resolve(), args.registration.resolve()
    )
    print(json.dumps({
        "status": "pass",
        "analysisManifest": str((args.output / "analysis-manifest.json").resolve()),
        "outputHashes": manifest["outputs"],
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
