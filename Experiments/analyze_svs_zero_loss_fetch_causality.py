#!/usr/bin/env python3
"""Correlate Spec 143 consumer and producer Fetch events by Name and Nonce."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
from typing import Any, Iterable


SCHEMA = "spec143.causality-analysis.v1"
SUMMARY_SCHEMA = "spec143.classification-summary.v1"
RESOURCE_SCHEMA = "spec143.resource-summary.v1"
EVENT_RE = re.compile(r"\bevent=([A-Za-z0-9_]+)")
FIELD_RE = re.compile(r"\b([A-Za-z][A-Za-z0-9_]*)=([^\s]+)")
TIMEOUT_CLASSES = {
    "PRODUCER_STORE_MISS",
    "PRODUCER_PUT_WITHOUT_CONSUMER_DATA",
    "NO_PRODUCER_OBSERVATION",
    "PRODUCER_STORE_HIT_WITHOUT_PUT",
    "UNCLASSIFIED",
}
CLASSIFIED_CLASSES = TIMEOUT_CLASSES - {"UNCLASSIFIED"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: Any) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def parse_trace(path: Path, peer: str) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line_number, line in enumerate(stream, 1):
            match = EVENT_RE.search(line)
            if not match:
                continue
            fields = {key: value.rstrip(",") for key, value in FIELD_RE.findall(line)}
            event = {
                "peer": peer,
                "event": match.group(1),
                "source": str(path),
                "sourceLine": line_number,
                **fields,
            }
            for key in (
                "attempt_id",
                "queued_mono_ns",
                "dispatch_mono_ns",
                "terminal_mono_ns",
                "mono_ns",
                "queue_us",
                "pending_us",
                "lookup_us",
                "lifetime_ms",
                "retries_left",
            ):
                if key in event:
                    try:
                        event[key] = int(str(event[key]))
                    except ValueError:
                        event[f"{key}ParseError"] = True
            events.append(event)
    return events


def validate_events(events_by_peer: dict[str, list[dict[str, Any]]]) -> None:
    for peer, events in events_by_peer.items():
        for event in events:
            parse_errors = sorted(
                key for key in event if key.endswith("ParseError")
            )
            if parse_errors:
                raise ValueError(
                    f"{peer}:{event['sourceLine']}: malformed numeric fields "
                    f"{parse_errors}"
                )
            kind = event["event"]
            if kind.startswith(
                ("fetcher_", "producer_", "mapping_producer_")
            ) and (not event.get("name") or not event.get("nonce")):
                raise ValueError(
                    f"{peer}:{event['sourceLine']}: {kind} lacks name/nonce"
                )
            if kind == "fetcher_timeout" and (
                event.get("attempt_id") is None
                or event.get("terminal_mono_ns") is None
            ):
                raise ValueError(
                    f"{peer}:{event['sourceLine']}: timeout lacks attempt/time"
                )


def semantic_kind(name: str) -> str:
    return "mapping" if "/MAPPING/" in name or name.endswith("/MAPPING") else "publication"


def event_ref(event: dict[str, Any]) -> dict[str, Any]:
    return {
        "peer": event["peer"],
        "event": event["event"],
        "source": event["source"],
        "sourceLine": event["sourceLine"],
    }


def in_measurement(event: dict[str, Any], summary: dict[str, Any]) -> bool:
    timestamp = event.get("terminal_mono_ns")
    start = summary.get("resourceMeasureStartSteadyNs")
    end = summary.get("resourceMeasureEndSteadyNs")
    return (
        isinstance(timestamp, int)
        and isinstance(start, int)
        and isinstance(end, int)
        and start <= timestamp <= end
    )


def classify_timeouts(
    events_by_peer: dict[str, list[dict[str, Any]]],
    summaries: dict[str, dict[str, Any]],
) -> list[dict[str, Any]]:
    peers = tuple(sorted(events_by_peer))
    if len(peers) != 2:
        raise ValueError("Spec 143 requires exactly two peer traces")
    producer_indexes: dict[str, dict[tuple[str, str], list[dict[str, Any]]]] = {}
    producer_by_name: dict[str, dict[str, list[dict[str, Any]]]] = {}
    consumer_data: dict[str, dict[tuple[str, str], list[dict[str, Any]]]] = {}
    consumer_data_by_name: dict[str, dict[str, list[dict[str, Any]]]] = {}
    consumer_attempts: dict[
        str, dict[tuple[str, str, Any], list[dict[str, Any]]]
    ] = {}
    for peer, events in events_by_peer.items():
        p_index: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
        p_name: dict[str, list[dict[str, Any]]] = defaultdict(list)
        c_index: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
        c_name: dict[str, list[dict[str, Any]]] = defaultdict(list)
        c_attempt: dict[
            tuple[str, str, Any], list[dict[str, Any]]
        ] = defaultdict(list)
        for event in events:
            name = str(event.get("name", ""))
            nonce = str(event.get("nonce", ""))
            if not name or not nonce:
                continue
            if event["event"].startswith(("producer_", "mapping_producer_")):
                p_index[(name, nonce)].append(event)
                p_name[name].append(event)
            if event["event"].startswith("fetcher_"):
                c_attempt[(name, nonce, event.get("attempt_id"))].append(event)
            if event["event"] == "fetcher_data":
                c_index[(name, nonce)].append(event)
                c_name[name].append(event)
        producer_indexes[peer] = p_index
        producer_by_name[peer] = p_name
        consumer_data[peer] = c_index
        consumer_data_by_name[peer] = c_name
        consumer_attempts[peer] = c_attempt

    timelines: list[dict[str, Any]] = []
    for consumer in peers:
        producer = peers[1] if consumer == peers[0] else peers[0]
        for timeout in events_by_peer[consumer]:
            if timeout["event"] != "fetcher_timeout":
                continue
            if not in_measurement(timeout, summaries[consumer]):
                continue
            name = str(timeout.get("name", ""))
            nonce = str(timeout.get("nonce", ""))
            attempt_id = timeout.get("attempt_id")
            consumer_attempt = consumer_attempts[consumer].get(
                (name, nonce, attempt_id), []
            )
            matching_producer = producer_indexes[producer].get((name, nonce), [])
            matching_data = consumer_data[consumer].get((name, nonce), [])
            kind = semantic_kind(name)
            # Mapping Interests overlap the generic SVSync DataStore prefix.
            # A generic producer_store_miss is therefore expected and is not
            # causal when MappingProvider handles the same exact Interest.
            if kind == "mapping":
                causal_producer = [
                    event
                    for event in matching_producer
                    if event["event"].startswith("mapping_producer_")
                ]
            else:
                causal_producer = [
                    event
                    for event in matching_producer
                    if event["event"].startswith("producer_")
                ]
            same_name_other_nonce = [
                event
                for event in producer_by_name[producer].get(name, [])
                if event.get("nonce") != nonce
                and (
                    event["event"].startswith("mapping_producer_")
                    if kind == "mapping"
                    else event["event"].startswith("producer_")
                )
            ]
            producer_event_names = {event["event"] for event in causal_producer}
            if {
                "producer_store_miss",
                "mapping_producer_empty",
            } & producer_event_names:
                classification = "PRODUCER_STORE_MISS"
            elif "producer_store_hit" in producer_event_names and (
                "producer_data_put" not in producer_event_names
            ):
                classification = "PRODUCER_STORE_HIT_WITHOUT_PUT"
            elif {
                "producer_data_put",
                "mapping_producer_data_put",
            } & producer_event_names and not matching_data:
                classification = "PRODUCER_PUT_WITHOUT_CONSUMER_DATA"
            elif not causal_producer:
                classification = "NO_PRODUCER_OBSERVATION"
            else:
                classification = "UNCLASSIFIED"

            later_same_name = [
                event
                for event in consumer_data_by_name[consumer].get(name, [])
                if event.get("nonce") != nonce
                and isinstance(event.get("terminal_mono_ns"), int)
                and event["terminal_mono_ns"] > timeout["terminal_mono_ns"]
            ]
            validation = [
                event
                for event in consumer_attempt
                if event["event"].startswith("fetcher_validation_")
            ]
            timeline = {
                "consumerPeer": consumer,
                "producerPeer": producer,
                "interestName": name,
                "interestNonce": nonce,
                "consumerAttemptId": attempt_id,
                "semanticKind": kind,
                "phase": "measure",
                "classification": classification,
                "queueToDispatchUs": next(
                    (
                        event.get("queue_us")
                        for event in consumer_attempt
                        if event.get("queue_us") is not None
                    ),
                    None,
                ),
                "dispatchToTerminalUs": timeout.get("pending_us"),
                "producerLookupUs": next(
                    (
                        event.get("lookup_us")
                        for event in matching_producer
                        if event.get("lookup_us") is not None
                    ),
                    None,
                ),
                "laterSameNameData": bool(later_same_name),
                "producerSameNameOtherNonce": bool(same_name_other_nonce),
                "producerSameNameOtherNonceCount": len(same_name_other_nonce),
                "evidence": [
                    event_ref(event)
                    for event in [
                        *consumer_attempt,
                        *causal_producer,
                        *matching_data,
                    ]
                ],
                "secondaryEvidence": [
                    event_ref(event)
                    for event in [
                        *later_same_name,
                        *validation,
                        *same_name_other_nonce[:8],
                    ]
                ],
            }
            if timeline["classification"] not in TIMEOUT_CLASSES:
                raise AssertionError("unknown classification")
            timelines.append(timeline)
    return timelines


def validate_resource(summary: dict[str, Any], peer: str) -> dict[str, Any]:
    wall_ns = int(summary.get("resourceMeasureWallNs", 0))
    user_us = int(summary.get("resourceUserCpuUs", -1))
    system_us = int(summary.get("resourceSystemCpuUs", -1))
    total_us = int(summary.get("resourceTotalCpuUs", -1))
    one_core = float(summary.get("resourceCpuPctOneCore", -1.0))
    four_core = float(summary.get("resourceCpuPctFourCore", -1.0))
    errors: list[str] = []
    if wall_ns <= 0:
        errors.append("resourceMeasureWallNs must be positive")
    if min(user_us, system_us, total_us) < 0 or total_us != user_us + system_us:
        errors.append("CPU deltas are inconsistent")
    expected_one = (
        100.0 * total_us * 1000.0 / wall_ns if wall_ns > 0 else 0.0
    )
    if abs(one_core - expected_one) > 0.02:
        errors.append("one-core CPU utilization does not recompute")
    if abs(four_core - one_core / 4.0) > 0.02:
        errors.append("four-core CPU utilization does not recompute")
    for key in ("resourceThreadsAtMeasureStart", "resourceThreadsAtMeasureEnd"):
        if int(summary.get(key, 0)) <= 0:
            errors.append(f"{key} must be positive")
    return {
        "peer": peer,
        "valid": not errors,
        "errors": errors,
        "measurementWallNs": wall_ns,
        "userCpuUs": user_us,
        "systemCpuUs": system_us,
        "totalCpuUs": total_us,
        "cpuPctOneCore": one_core,
        "cpuPctFourCore": four_core,
        "threadsAtStart": int(summary.get("resourceThreadsAtMeasureStart", 0)),
        "threadsAtEnd": int(summary.get("resourceThreadsAtMeasureEnd", 0)),
        "maxRssKiB": int(summary.get("resourceMaxRssKiBAtMeasureEnd", 0)),
    }


def write_timelines(path: Path, timelines: Iterable[dict[str, Any]]) -> None:
    with path.open("w", encoding="utf-8") as stream:
        for timeline in timelines:
            stream.write(json.dumps(timeline, sort_keys=True) + "\n")


def analyze_cell(cell: Path) -> dict[str, Any]:
    peers = ("peer-a", "peer-b")
    summaries = {
        peer: json.loads(
            (cell / f"{peer}-summary.json").read_text(encoding="utf-8")
        )
        for peer in peers
    }
    trace_paths = {
        peer: (
            cell / f"{peer}.trace.log"
            if (cell / f"{peer}.trace.log").is_file()
            else cell / f"{peer}.stderr"
        )
        for peer in peers
    }
    events_by_peer = {
        peer: parse_trace(trace_paths[peer], peer) for peer in peers
    }
    validate_events(events_by_peer)
    timelines = classify_timeouts(events_by_peer, summaries)
    counts = Counter(timeline["classification"] for timeline in timelines)
    semantic_counts = Counter(timeline["semanticKind"] for timeline in timelines)
    no_exact_producer = [
        timeline
        for timeline in timelines
        if timeline["classification"] == "NO_PRODUCER_OBSERVATION"
    ]
    timeout_count = len(timelines)
    classified_count = sum(counts[name] for name in CLASSIFIED_CLASSES)
    coverage = classified_count / timeout_count if timeout_count else 0.0
    status = (
        "DIAGNOSED"
        if timeout_count > 0 and coverage >= 0.95
        else "INCONCLUSIVE"
    )
    timeline_path = cell / "causal-timelines.jsonl"
    write_timelines(timeline_path, timelines)
    trace_records = {
        peer: {
            "path": str(path),
            "sha256": sha256(path),
            "bytes": path.stat().st_size,
            "eventCount": len(events_by_peer[peer]),
        }
        for peer, path in trace_paths.items()
    }
    resource_peers = {
        peer: {
            **validate_resource(summaries[peer], peer),
            "traceBytes": trace_records[peer]["bytes"],
        }
        for peer in peers
    }
    resource = {
        "schema": RESOURCE_SCHEMA,
        "valid": all(record["valid"] for record in resource_peers.values()),
        "peers": resource_peers,
    }
    write_json(cell / "resource-summary.json", resource)
    analyzer_path = Path(__file__).resolve()
    analyzer_sha256 = sha256(analyzer_path)
    frozen_analyzer_sha256 = ""
    profile_manifest = cell.parent / "runtime-profile-manifest.json"
    if profile_manifest.is_file():
        profile = json.loads(profile_manifest.read_text(encoding="utf-8"))
        frozen_analyzer_sha256 = (
            profile.get("build", {})
            .get("sources", {})
            .get("analyzer", {})
            .get("sha256", "")
        )
    summary = {
        "schema": SUMMARY_SCHEMA,
        "status": status,
        "timeoutCount": timeout_count,
        "classifiedTimeoutCount": classified_count,
        "classificationCoverage": coverage,
        "classificationCounts": dict(sorted(counts.items())),
        "semanticTimeoutCounts": dict(sorted(semantic_counts.items())),
        "secondaryObservations": {
            "noExactProducerObservation": len(no_exact_producer),
            "sameNameOtherNonceObservedAtProducer": sum(
                bool(timeline["producerSameNameOtherNonce"])
                for timeline in no_exact_producer
            ),
            "noSameNameProducerObservation": sum(
                not timeline["producerSameNameOtherNonce"]
                for timeline in no_exact_producer
            ),
        },
        "traceRecords": trace_records,
        "timelinePath": str(timeline_path),
        "timelineSha256": sha256(timeline_path),
        "resourceSummary": str(cell / "resource-summary.json"),
        "resourceValid": resource["valid"],
        "analysisTool": {
            "path": str(analyzer_path),
            "sha256": analyzer_sha256,
            "frozenBuildSha256": frozen_analyzer_sha256,
            "postCellRevision": bool(
                frozen_analyzer_sha256
                and frozen_analyzer_sha256 != analyzer_sha256
            ),
            "revisionReason": (
                "mapping-provider precedence over overlapping generic store miss"
            ),
        },
    }
    write_json(cell / "classification-summary.json", summary)
    terminal_path = cell / "terminal.json"
    superseded_summary_sha256 = ""
    if terminal_path.is_file():
        terminal = json.loads(terminal_path.read_text(encoding="utf-8"))
        superseded_summary_sha256 = terminal.get(
            "classificationSummarySha256", ""
        )
    revision = {
        "schema": "spec143.analysis-revision.v1",
        "networkCellRerun": False,
        "reason": summary["analysisTool"]["revisionReason"],
        "frozenAnalyzerSha256": frozen_analyzer_sha256,
        "revisedAnalyzerSha256": analyzer_sha256,
        "supersededClassificationSummarySha256": superseded_summary_sha256,
        "revisedClassificationSummarySha256": sha256(
            cell / "classification-summary.json"
        ),
        "rawTraceSha256": {
            peer: record["sha256"] for peer, record in trace_records.items()
        },
    }
    write_json(cell / "analysis-revision.json", revision)
    return {
        "schema": SCHEMA,
        "cell": str(cell),
        "classification": summary,
        "resource": resource,
        "analysisRevision": revision,
    }


def render_report(analysis: dict[str, Any]) -> str:
    summary = analysis["classification"]
    resource = analysis["resource"]
    lines = [
        "# Spec 143 Zero-Loss Fetch Causality Report",
        "",
        "## Material Passport",
        "",
        "- Origin Skill: experiment-agent",
        "- Origin Mode: validate",
        "- Verification Status: MEASURED",
        "- Version Label: code_result_v1",
        "",
        f"**Verdict: {summary['status']}**",
        "",
        f"- Measurement-window timeouts: {summary['timeoutCount']}",
        f"- Classified: {summary['classifiedTimeoutCount']}",
        f"- Coverage: {summary['classificationCoverage']:.2%}",
        f"- Analyzer SHA-256: `{summary['analysisTool']['sha256']}`",
        f"- Post-cell analyzer revision: {summary['analysisTool']['postCellRevision']}",
        "- Network cell rerun for this revision: False",
        "",
        "## Timeout classifications",
        "",
        "| Class | Count |",
        "|---|---:|",
    ]
    for name, count in summary["classificationCounts"].items():
        lines.append(f"| {name} | {count} |")
    secondary = summary["secondaryObservations"]
    lines += [
        "",
        "## Secondary observations",
        "",
        f"- No exact-Nonce producer observation: {secondary['noExactProducerObservation']}",
        "- Same name with another nonce observed at producer: "
        f"{secondary['sameNameOtherNonceObservedAtProducer']}",
        "- No same-name producer observation: "
        f"{secondary['noSameNameProducerObservation']}",
        "",
        "The different-Nonce observation is consistent with retry overlap or PIT "
        "aggregation, but does not by itself prove either mechanism.",
    ]
    lines += [
        "",
        "## Process resources",
        "",
        "| Peer | CPU one-core | CPU four-core | Threads start/end | Max RSS KiB | Trace bytes |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for peer, record in resource["peers"].items():
        lines.append(
            f"| {peer} | {record['cpuPctOneCore']:.2f}% | "
            f"{record['cpuPctFourCore']:.2f}% | "
            f"{record['threadsAtStart']}/{record['threadsAtEnd']} | "
            f"{record['maxRssKiB']} | {record['traceBytes']} |"
        )
    lines += [
        "",
        "## Claim boundary",
        "",
        "This once-only, two-node diagnostic classifies where Fetch attempts stop. "
        "It does not estimate run-to-run variance, prove physical packet loss, "
        "or validate a recovery fix.",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("cell", type=Path)
    args = parser.parse_args()
    analysis = analyze_cell(args.cell.resolve())
    write_json(args.cell / "analysis.json", analysis)
    (args.cell / "report.md").write_text(
        render_report(analysis), encoding="utf-8"
    )
    print(args.cell / "classification-summary.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
