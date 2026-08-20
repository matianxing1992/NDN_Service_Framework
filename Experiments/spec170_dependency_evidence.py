"""Pure log-evidence analysis for Spec170 runtime gates.

This module intentionally has no NDNSF native-extension, MiniNDN, or ORT
imports. Exact-SIF gates must work when the host has no compatible ``_ndnsf``.
"""

from __future__ import annotations

import json
from pathlib import Path


DEFAULT_SERVICE = "/Inference/NativeTracer"


def _read_log_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def _parse_trace_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for part in line.split():
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        fields[key] = value.strip()
    return fields


def _int_field(fields: dict[str, str], key: str, fallback: int = 0) -> int:
    try:
        return int(float(fields.get(key, fallback)))
    except (TypeError, ValueError):
        return fallback


def _trace_data_name(fields: dict[str, str]) -> str:
    """Return the actual object name represented by one timing trace.

    The native trace writes ``planned_name=<URI>``.  The Python ONNX adapter
    writes ``planned_name=true|false`` as a boolean indicating whether the
    planned fast path was used, and puts the actual URI in ``data_name``.
    Evidence matching must compare the URI, not that boolean marker.
    """
    data_name = str(fields.get("data_name", "")).strip()
    if data_name and data_name.lower() not in {"false", "true", "none", "-"}:
        return data_name
    planned_name = str(fields.get("planned_name", "")).strip()
    if planned_name.lower() in {"false", "true", "none", "-"}:
        return ""
    return planned_name


def collect_dependency_execution_evidence(
        log_paths: list[Path], plan_path: Path,
        service_name: str = DEFAULT_SERVICE) -> dict[str, object]:
    """Prove that every planned producer-to-consumer edge was transferred."""
    plan = json.loads(plan_path.read_text(encoding="utf-8"))
    service = next(
        item for item in plan["services"] if item["service"] == service_name)
    expected: list[dict[str, str]] = []
    for dependency in service.get("dependencies", []):
        producers = dependency.get("producers", [])
        is_multi_producer_redistribution = (
            bool(dependency.get("redistributions")) and len(producers) > 1)
        for producer in producers:
            logical_scope = str(dependency["keyScope"])
            runtime_scope = (
                f"{logical_scope}/from/{str(producer).strip('/')}"
                if is_multi_producer_redistribution else logical_scope)
            for consumer in dependency.get("consumers", []):
                expected.append({
                    "scope": runtime_scope,
                    "transportScope": logical_scope,
                    "producer": str(producer),
                    "consumer": str(consumer),
                })

    published: dict[tuple[str, str], dict[str, object]] = {}
    fetched: dict[tuple[str, str, str], dict[str, object]] = {}
    for path in log_paths:
        for line_no, line in enumerate(_read_log_text(path).splitlines(), 1):
            if "NDNSF_DI_DEPENDENCY_OUTPUT_TIMING" in line:
                fields = _parse_trace_fields(line)
                key = (fields.get("producer", ""), fields.get("scope", ""))
                published[key] = {
                    "plannedDataName": _trace_data_name(fields),
                    "declaredDataName": fields.get("data_name", ""),
                    "plannedNameMarker": fields.get("planned_name", ""),
                    "bytes": _int_field(fields, "bytes"),
                    "log": str(path),
                    "line": line_no,
                }
            elif "NDNSF_DI_DEPENDENCY_INPUT_TIMING" in line:
                fields = _parse_trace_fields(line)
                key = (
                    fields.get("role", ""),
                    fields.get("producer", ""),
                    fields.get("scope", ""),
                )
                fetched[key] = {
                    "plannedDataName": _trace_data_name(fields),
                    "declaredDataName": fields.get("data_name", ""),
                    "plannedNameMarker": fields.get("planned_name", ""),
                    "bytes": _int_field(fields, "bytes"),
                    "log": str(path),
                    "line": line_no,
                }

    edge_records: list[dict[str, object]] = []
    missing_publications: list[str] = []
    missing_fetches: list[str] = []
    name_mismatches: list[str] = []
    empty_payloads: list[str] = []
    for edge in expected:
        label = f"{edge['producer']}->{edge['consumer']}:{edge['scope']}"
        output = published.get((edge["producer"], edge["scope"]))
        # A producer publishes the shared object under the logical transport
        # scope.  Consumers of a multi-producer redistribution use the
        # producer-qualified runtime scope to keep their local endpoint
        # authorities distinct.  Match both representations while comparing
        # the actual Data name below.
        if output is None:
            output = published.get((edge["producer"], edge["transportScope"]))
        input_record = fetched.get(
            (edge["consumer"], edge["producer"], edge["scope"]))
        if output is None:
            missing_publications.append(label)
        if input_record is None:
            missing_fetches.append(label)
        if output is not None and input_record is not None:
            output_name = str(output["plannedDataName"])
            input_name = str(input_record["plannedDataName"])
            if not output_name or output_name != input_name:
                name_mismatches.append(label)
            if int(output["bytes"]) <= 0 or int(input_record["bytes"]) <= 0:
                empty_payloads.append(label)
        edge_records.append({
            **edge,
            "published": output is not None,
            "fetched": input_record is not None,
            "publish": output,
            "fetch": input_record,
        })

    complete = bool(expected) and not (
        missing_publications or missing_fetches or name_mismatches or empty_payloads)
    return {
        "status": "executed" if complete else "incomplete",
        "reason": (
            "every planned dependency edge has matching non-empty production "
            "publish and consumer fetch evidence"
            if complete else
            "one or more planned dependency edges lacks matching lifecycle evidence"
        ),
        "expectedEdgeCount": len(expected),
        "completeEdgeCount": sum(
            1 for item in edge_records if item["published"] and item["fetched"]),
        "missingPublications": missing_publications,
        "missingFetches": missing_fetches,
        "nameMismatches": name_mismatches,
        "emptyPayloads": empty_payloads,
        "edges": edge_records,
    }
