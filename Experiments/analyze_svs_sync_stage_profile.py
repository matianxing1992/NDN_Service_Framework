#!/usr/bin/env python3
"""Strict Spec 133 campaign analyzer.

The analyzer treats the sealed manifest, terminal receipts, exact process-end
summaries, sampled spans, and application events as separate authorities.  It
never adds aggregate spans to leaf CPU demand and never treats queue/external
wait as CPU service.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from collections import defaultdict
from pathlib import Path
import re
from typing import Any, Iterable


RATES = (200, 400, 600, 800, 1000)
PEERS = ("peer-a", "peer-b")
OUTPUTS = (
    "campaign-summary.json", "cell-summary.csv", "rate-stage-summary.csv",
    "critical-path-groups.csv", "path-frequency.csv", "bottleneck-ranking.csv",
    "bottleneck-report.md", "limitations.md",
)
INTEGER_FIELDS = {
    "startRawNs", "durationNs", "bytes", "items", "sampleModulus", "calls",
    "successes", "failures", "sampledCalls", "droppedRecords", "totalDurationNs",
    "minDurationNs", "maxDurationNs", "totalBytes", "totalItems",
}
VALID_KINDS = {"leaf-cpu", "aggregate", "lock-wait", "queue-wait", "external-wait", "milestone"}
VALID_THREADS = {"app-main", "face-io", "callback", "external", "mixed"}
VALID_CORRELATION = {"exact", "ambiguous", "censored"}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"invalid JSON authority {path}: {error}") from error
    if not isinstance(value, dict):
        raise RuntimeError(f"JSON authority is not an object: {path}")
    return value


def nearest_rank(values: Iterable[int | float], percentile: float) -> float | None:
    ordered = sorted(float(value) for value in values)
    if not ordered:
        return None
    if percentile < 0 or percentile > 1:
        raise ValueError("percentile must be within [0, 1]")
    return ordered[max(0, math.ceil(percentile * len(ordered)) - 1)]


def interval_union_ns(intervals: Iterable[tuple[int, int]]) -> int:
    normalized = sorted((int(start), int(end)) for start, end in intervals)
    if any(start < 0 or end < start for start, end in normalized):
        raise RuntimeError("invalid interval")
    total = 0
    cursor_start = cursor_end = None
    for start, end in normalized:
        if cursor_start is None:
            cursor_start, cursor_end = start, end
        elif start <= cursor_end:
            cursor_end = max(cursor_end, end)
        else:
            total += cursor_end - cursor_start
            cursor_start, cursor_end = start, end
    if cursor_start is not None:
        total += cursor_end - cursor_start
    return total


def parse_registry(header: Path) -> dict[str, dict[str, str]]:
    text = header.read_text(encoding="utf-8")
    pattern = re.compile(
        r'X\([A-Z0-9_]+, "([A-Z]+\.[A-Z0-9_.]+)", '
        r'(LeafCpu|Aggregate|LockWait|QueueWait|ExternalWait|Milestone), '
        r'(AppMain|FaceIo|Callback|External|Mixed), "([^"]*)"\)')
    kind_map = {"LeafCpu": "leaf-cpu", "Aggregate": "aggregate", "LockWait": "lock-wait",
                "QueueWait": "queue-wait", "ExternalWait": "external-wait", "Milestone": "milestone"}
    thread_map = {"AppMain": "app-main", "FaceIo": "face-io", "Callback": "callback",
                  "External": "external", "Mixed": "mixed"}
    registry = {stage: {"kind": kind_map[kind], "thread": thread_map[thread], "parent": parent}
                for stage, kind, thread, parent in pattern.findall(text)}
    if len(registry) != 81:
        raise RuntimeError(f"stage registry count mismatch: expected 81, got {len(registry)}")
    return registry


def parse_profile_record(line: str) -> dict[str, Any] | None:
    position = line.find("schema=spec133-")
    if position < 0:
        return None
    record: dict[str, Any] = {}
    for token in line[position:].strip().split():
        if "=" not in token:
            raise RuntimeError(f"malformed profile token: {token}")
        key, value = token.split("=", 1)
        if key in record:
            raise RuntimeError(f"duplicate profile field: {key}")
        record[key] = value
    for key in INTEGER_FIELDS & record.keys():
        if not re.fullmatch(r"[0-9]+", str(record[key])):
            raise RuntimeError(f"invalid nonnegative integer {key}={record[key]}")
        record[key] = int(record[key])
    return record


def read_profile(path: Path, cell: str, peer: str, registry: dict[str, dict[str, str]],
                 sample_modulus: int) -> tuple[list[dict[str, Any]], dict[str, dict[str, Any]]]:
    records = [record for line in path.read_text(encoding="utf-8", errors="replace").splitlines()
               if (record := parse_profile_record(line)) is not None]
    starts = [r for r in records if r.get("event") == "profile-start"]
    stops = [r for r in records if r.get("event") == "profile-stop"]
    spans = [r for r in records if r.get("event") == "stage-span"]
    summaries_list = [r for r in records if r.get("event") == "stage-summary"]
    if len(starts) != 1 or len(stops) != 1:
        raise RuntimeError(f"{cell}/{peer}: lifecycle count mismatch")
    if len(summaries_list) != len(registry):
        raise RuntimeError(f"{cell}/{peer}: expected {len(registry)} summaries, got {len(summaries_list)}")
    summaries: dict[str, dict[str, Any]] = {}
    span_counts: dict[str, int] = defaultdict(int)
    for record in records:
        if record.get("cell") != cell or record.get("peer") != peer:
            raise RuntimeError(f"{cell}/{peer}: cross-identity profile record")
        if record.get("sampleModulus") != sample_modulus:
            raise RuntimeError(f"{cell}/{peer}: sample modulus mismatch")
        stage = record.get("stage")
        if stage is None:
            continue
        definition = registry.get(stage)
        if definition is None or record.get("kind") != definition["kind"] or record.get("thread") != definition["thread"]:
            raise RuntimeError(f"{cell}/{peer}: stage schema mismatch for {stage}")
        if record.get("event") == "stage-span":
            if record.get("correlationMode") not in VALID_CORRELATION:
                raise RuntimeError(f"{cell}/{peer}: invalid correlation mode")
            if stage.endswith("NETWORK_WAIT") and record["correlationMode"] != "exact" and record["durationNs"] != 0:
                raise RuntimeError(f"{cell}/{peer}: ambiguous network duration")
            span_counts[stage] += 1
        elif record.get("event") == "stage-summary":
            if stage in summaries:
                raise RuntimeError(f"{cell}/{peer}: duplicate summary {stage}")
            if record["successes"] + record["failures"] > record["calls"]:
                raise RuntimeError(f"{cell}/{peer}: impossible outcome counts {stage}")
            if record["droppedRecords"] != 0:
                raise RuntimeError(f"{cell}/{peer}: dropped profile records {stage}")
            summaries[stage] = record
    if set(summaries) != set(registry):
        raise RuntimeError(f"{cell}/{peer}: incomplete stage registry")
    for stage, summary in summaries.items():
        if span_counts[stage] != summary["sampledCalls"]:
            raise RuntimeError(f"{cell}/{peer}: sampled-call mismatch {stage}")
    validate_containment(spans, registry, cell, peer)
    return spans, summaries


def validate_containment(spans: list[dict[str, Any]], registry: dict[str, dict[str, str]],
                         cell: str, peer: str) -> None:
    parents: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for span in spans:
        if span["kind"] == "aggregate":
            parents[(span["trace"], span["stage"])].append(span)
    for child in spans:
        parent_stage = registry[child["stage"]]["parent"]
        if not parent_stage:
            continue
        candidates = parents.get((child["trace"], parent_stage), [])
        if not candidates:
            continue  # deterministic sampling may observe a child keyed at another operation boundary
        child_end = child["startRawNs"] + child["durationNs"]
        contained = [parent for parent in candidates
                     if parent["startRawNs"] <= child["startRawNs"] and
                     child_end <= parent["startRawNs"] + parent["durationNs"]]
        child["containmentValid"] = bool(contained)
        if not contained:
            child["containmentIssue"] = (
                f"{cell}/{peer}: sampled child outside same-trace {parent_stage}"
            )


def aggregate_residuals(spans: list[dict[str, Any]], registry: dict[str, dict[str, str]]) -> dict[str, list[int]]:
    by_trace: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for span in spans:
        by_trace[span["trace"]].append(span)
    result: dict[str, list[int]] = defaultdict(list)
    for trace_spans in by_trace.values():
        for parent in (s for s in trace_spans if s["kind"] == "aggregate"):
            start = parent["startRawNs"]
            end = start + parent["durationNs"]
            children = [(s["startRawNs"], s["startRawNs"] + s["durationNs"])
                        for s in trace_spans
                        if registry[s["stage"]]["parent"] == parent["stage"] and
                        s["kind"] != "aggregate" and start <= s["startRawNs"] and
                        s["startRawNs"] + s["durationNs"] <= end]
            residual = parent["durationNs"] - interval_union_ns(children)
            if residual < 0:
                raise RuntimeError(f"negative residual for {parent['stage']}")
            result[parent["stage"]].append(residual)
    return result


def read_events(path: Path, cell: str, peer: str) -> list[dict[str, Any]]:
    events = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        try:
            event = json.loads(line)
        except json.JSONDecodeError as error:
            raise RuntimeError(f"{path}:{number}: invalid event JSON: {error}") from error
        if event.get("schemaVersion") != "spec133-app-event-v1" or event.get("cellId") != cell or event.get("peerId") != peer:
            raise RuntimeError(f"{path}:{number}: event identity/schema mismatch")
        if not isinstance(event.get("monotonicRawNs"), int) or event["monotonicRawNs"] < 0:
            raise RuntimeError(f"{path}:{number}: invalid monotonic timestamp")
        events.append(event)
    return events


def distribution(values: Iterable[int | float]) -> dict[str, float | int | None]:
    data = [float(value) for value in values]
    return {"samples": len(data), "meanNs": sum(data) / len(data) if data else None,
            "p50Ns": nearest_rank(data, .50), "p95Ns": nearest_rank(data, .95),
            "p99Ns": nearest_rank(data, .99), "maxNs": max(data) if data else None}


def write_csv(path: Path, rows: list[dict[str, Any]], fields: list[str]) -> None:
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def path_name(stage: str) -> str:
    if stage.startswith("PUB."): return "publisher"
    if stage.startswith("SYNC."): return "sync"
    if stage.startswith("MAP."): return "mapping"
    if stage.startswith("PAYLOAD."): return "payload"
    return "app-boundary"


def rate_boundary(cell_rows: list[dict[str, Any]]) -> int | None:
    rows = sorted((r for r in cell_rows if r["valid"]), key=lambda r: r["ratePpsPerPeer"])
    for row in rows:
        target = row["ratePpsPerPeer"]
        if row["attemptedPpsPerPeer"] < target * .98 or row["deliveryRatio"] < .98:
            return target
    return None


def rank_bottlenecks(stage_rows: list[dict[str, Any]], cell_rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    boundary = rate_boundary(cell_rows)
    high_rate = max((r["ratePpsPerPeer"] for r in cell_rows if r["valid"]), default=None)
    findings = []
    if high_rate is None:
        return findings
    candidates = [row for row in stage_rows if row["ratePpsPerPeer"] == high_rate and row["calls"] > 0]
    for group, kinds in (("publisher-main", {("app-main", "leaf-cpu")}),
                         ("face-callback-cpu", {("face-io", "leaf-cpu"), ("callback", "leaf-cpu"), ("mixed", "leaf-cpu")}),
                         ("queue-lock-wait", {(t, k) for t in VALID_THREADS for k in ("queue-wait", "lock-wait")}),
                         ("external-wait", {("external", "external-wait")})):
        group_rows = [r for r in candidates if (r["threadRole"], r["kind"]) in kinds]
        group_rows.sort(key=lambda r: (r["totalDurationNs"], r["p95Ns"] or 0), reverse=True)
        for rank, row in enumerate(group_rows[:5], 1):
            historical = sorted([r for r in stage_rows if r["stageId"] == row["stageId"] and
                                 r["peerId"] == row["peerId"]], key=lambda r: r["ratePpsPerPeer"])
            growth = False
            if len(historical) >= 2 and historical[0]["p95Ns"] is not None and row["p95Ns"] is not None:
                growth = row["p95Ns"] > historical[0]["p95Ns"] * 1.25
            demand_signal = (row.get("threadCpuShare") or 0) >= .20
            boundary_signal = boundary is not None
            signals = int(demand_signal) + int(growth) + int(boundary_signal)
            findings.append({"group": group, "rank": rank, "stageId": row["stageId"],
                             "peerId": row["peerId"], "highRate": high_rate,
                             "threadCpuShare": row.get("threadCpuShare"), "p95Ns": row["p95Ns"],
                             "p95Growth25Percent": growth, "throughputBoundaryRate": boundary,
                             "signalCount": signals,
                             "verdict": "supported" if signals >= 2 else "candidate" if signals == 1 else "inconclusive"})
    return findings


def validate_authorities(campaign: Path) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    manifest_path = campaign / "campaign-manifest.json"
    manifest = load_json(manifest_path)
    if manifest.get("schemaVersion") != "spec133-campaign-manifest-v1":
        raise RuntimeError("campaign manifest schema mismatch")
    cells = manifest.get("cells", [])
    if [c.get("ratePpsPerPeer") for c in cells] != list(RATES) or len(cells) != 5 or any(c.get("attempt") != 1 for c in cells):
        raise RuntimeError("campaign is not the frozen five-cell matrix")
    seal = campaign / ".sealed"
    if not seal.is_file() or seal.read_text().strip() != sha256_file(manifest_path):
        raise RuntimeError("campaign seal mismatch")
    subject_path = Path(manifest["subjectManifest"])
    overhead_path = Path(manifest["overheadReceipt"])
    if sha256_file(subject_path) != manifest["subjectManifestSha256"] or sha256_file(overhead_path) != manifest["overheadReceiptSha256"]:
        raise RuntimeError("sealed subject or overhead authority drift")
    subject, overhead = load_json(subject_path), load_json(overhead_path)
    if subject.get("schemaVersion") not in {
            "spec133-subject-manifest-v1", "spec133-subject-manifest-io-v2"} or \
       overhead.get("verdict") != "ADMITTED":
        raise RuntimeError("subject identity or overhead admission invalid")
    for key in ("profiledBinary", "profiledLibrary"):
        artifact = Path(subject[key])
        if not artifact.is_file() or sha256_file(artifact) != subject[f"{key}Sha256"]:
            raise RuntimeError(f"subject artifact drift: {key}")
    receipt_files = sorted((campaign / "receipts").glob("*.json"))
    if len(receipt_files) != 5:
        raise RuntimeError(f"expected exactly five terminal receipts, got {len(receipt_files)}")
    return manifest, subject, overhead


def analyze(campaign: Path) -> dict[str, Any]:
    campaign = campaign.resolve()
    manifest, subject, overhead = validate_authorities(campaign)
    registry = parse_registry(Path(subject["profileWorktree"]) / "ndn-svs/profile.hpp")
    modulus = int(subject["profileConfig"]["sampleModulus"])
    cell_rows: list[dict[str, Any]] = []
    stage_rows: list[dict[str, Any]] = []
    group_rows: list[dict[str, Any]] = []
    path_rows: list[dict[str, Any]] = []
    invalid_cells: list[str] = []

    for config in manifest["cells"]:
        cell_id, rate = config["cellId"], config["ratePpsPerPeer"]
        receipt_path = campaign / "receipts" / f"{cell_id}.json"
        receipt = load_json(receipt_path)
        if receipt.get("schemaVersion") != "spec133-terminal-receipt-v1" or receipt.get("cellId") != cell_id or receipt.get("attempt") != 1:
            raise RuntimeError(f"terminal receipt mismatch: {cell_id}")
        cell_dir = campaign / "cells" / cell_id
        valid = receipt.get("status") == "COMPLETE"
        events_by_peer: dict[str, list[dict[str, Any]]] = {}
        profile_by_peer: dict[str, tuple[list[dict[str, Any]], dict[str, dict[str, Any]]]] = {}
        if valid:
            try:
                for peer in PEERS:
                    events_by_peer[peer] = read_events(cell_dir / f"{peer}-events.jsonl", cell_id, peer)
                    profile_by_peer[peer] = read_profile(cell_dir / f"{peer}.stderr", cell_id, peer, registry, modulus)
            except Exception:
                invalid_cells.append(cell_id)
                raise
        else:
            invalid_cells.append(cell_id)

        attempted = sum(sum(e["event"] == "api-return" and e["phase"] == "measured" for e in events_by_peer.get(p, [])) for p in PEERS)
        delivered = sum(sum(e["event"] == "delivery" and e["phase"] == "measured" for e in events_by_peer.get(p, [])) for p in PEERS)
        invalid = sum(sum(e["event"] == "invalid" for e in events_by_peer.get(p, [])) for p in PEERS)
        api_durations = []
        deadline_lateness = []
        delivery_delays = []
        for peer in PEERS:
            events = events_by_peer.get(peer, [])
            enters = {e["logicalId"]: e["monotonicRawNs"] for e in events if e["event"] == "api-enter" and e["phase"] == "measured"}
            for event in events:
                if event["event"] == "api-return" and event["phase"] == "measured" and event["logicalId"] in enters:
                    api_durations.append(event["monotonicRawNs"] - enters[event["logicalId"]])
                elif event["event"] == "deadline" and event["phase"] == "measured":
                    deadline_lateness.append(max(0, int(event.get("details", {}).get("actualWakeNs", event["monotonicRawNs"])) - event["monotonicRawNs"]))
                elif event["event"] == "delivery" and event["phase"] == "measured":
                    scheduled = int(event.get("details", {}).get("scheduledNs", event["monotonicRawNs"]))
                    delivery_delays.append(max(0, event["monotonicRawNs"] - scheduled))
        containment_violations = sum(
            sum(span.get("containmentValid") is False
                for span in profile_by_peer.get(peer, ([], {}))[0])
            for peer in PEERS
        )
        row = {"cellId": cell_id, "ratePpsPerPeer": rate, "status": receipt.get("status"), "valid": valid,
               "attempted": attempted, "delivered": delivered, "invalid": invalid,
               "containmentViolations": containment_violations,
               "attemptedPpsPerPeer": attempted / 120 if valid else 0,
               "deliveredPpsPerPeer": delivered / 120 if valid else 0,
               "deliveryRatio": delivered / attempted if attempted else 0,
               "apiP50Ns": nearest_rank(api_durations, .5), "apiP95Ns": nearest_rank(api_durations, .95),
               "deadlineLatenessP95Ns": nearest_rank(deadline_lateness, .95),
               "applicationDeliveryP95Ns": nearest_rank(delivery_delays, .95)}
        cell_rows.append(row)
        if not valid:
            continue

        for peer in PEERS:
            spans, summaries = profile_by_peer[peer]
            residuals = aggregate_residuals(spans, registry)
            span_by_stage: dict[str, list[dict[str, Any]]] = defaultdict(list)
            for span in spans:
                span_by_stage[span["stage"]].append(span)
            thread_totals: dict[str, int] = defaultdict(int)
            for stage, summary in summaries.items():
                definition = registry[stage]
                if definition["kind"] == "leaf-cpu":
                    thread_totals[definition["thread"]] += summary["totalDurationNs"]
            for stage, summary in summaries.items():
                definition = registry[stage]
                durations = [s["durationNs"] for s in span_by_stage[stage]
                             if not (definition["kind"] == "external-wait" and s["correlationMode"] != "exact")]
                dist = distribution(durations)
                denom_thread = thread_totals[definition["thread"]]
                stage_rows.append({"cellId": cell_id, "ratePpsPerPeer": rate, "peerId": peer,
                    "stageId": stage, "path": path_name(stage), "kind": definition["kind"],
                    "threadRole": definition["thread"], "calls": summary["calls"],
                    "sampledCalls": summary["sampledCalls"], "missingSamples": summary["sampledCalls"] - len(durations),
                    "containmentViolations": sum(
                        span.get("containmentValid") is False
                        for span in span_by_stage[stage]
                    ),
                    **dist, "totalDurationNs": summary["totalDurationNs"],
                    "callsPerAttempt": summary["calls"] / max(1, sum(e["event"] == "api-return" and e["phase"] == "measured" for e in events_by_peer[peer])),
                    "callsPerDelivery": summary["calls"] / max(1, sum(e["event"] == "delivery" and e["phase"] == "measured" for e in events_by_peer[peer])),
                    "serviceDemandNsPerAttempt": summary["totalDurationNs"] / max(1, sum(e["event"] == "api-return" and e["phase"] == "measured" for e in events_by_peer[peer])) if definition["kind"] == "leaf-cpu" else None,
                    "threadCpuShare": summary["totalDurationNs"] / denom_thread if definition["kind"] == "leaf-cpu" and denom_thread else None,
                    "residualMeanNs": (sum(residuals[stage]) / len(residuals[stage])) if residuals.get(stage) else None})
            counts = {stage: summaries[stage]["calls"] for stage in summaries}
            map_opportunities = counts["MAP.PROCESS_TOTAL"]
            map_fallback = counts["MAP.INTEREST_BUILD"]
            payload_opportunities = counts["MAP.FETCH_QUEUE_INSERT"] + counts["MAP.PIGGY_CALLBACK"]
            payload_fallback = counts["PAYLOAD.INTEREST_BUILD"]
            path_rows.append({"cellId": cell_id, "ratePpsPerPeer": rate, "peerId": peer,
                "direction": f"{('peer-b' if peer == 'peer-a' else 'peer-a')}->{peer}",
                "attempted": sum(e["event"] == "api-return" and e["phase"] == "measured" for e in events_by_peer[peer]),
                "delivered": sum(e["event"] == "delivery" and e["phase"] == "measured" for e in events_by_peer[peer]),
                "mappingOpportunities": map_opportunities, "mappingPiggyCallbacks": counts["MAP.PIGGY_CALLBACK"],
                "mappingFallbacks": map_fallback, "mappingFallbackRatio": map_fallback / map_opportunities if map_opportunities else 0,
                "payloadOpportunities": payload_opportunities, "payloadFallbacks": payload_fallback,
                "payloadFallbackRatio": payload_fallback / payload_opportunities if payload_opportunities else 0,
                "mappingNacks": summaries["MAP.NETWORK_WAIT"]["failures"],
                "payloadNacksOrTimeouts": summaries["PAYLOAD.NETWORK_WAIT"]["failures"]})
            for label, selected in (("publisher-main-cpu", lambda r: r["kind"] == "leaf-cpu" and r["threadRole"] == "app-main"),
                                    ("face-callback-cpu", lambda r: r["kind"] == "leaf-cpu" and r["threadRole"] in {"face-io", "callback", "mixed"}),
                                    ("lock-wait", lambda r: r["kind"] == "lock-wait"),
                                    ("queue-wait", lambda r: r["kind"] == "queue-wait"),
                                    ("external-wait", lambda r: r["kind"] == "external-wait")):
                selected_rows = [r for r in stage_rows if r["cellId"] == cell_id and r["peerId"] == peer and selected(r)]
                group_rows.append({"cellId": cell_id, "ratePpsPerPeer": rate, "peerId": peer,
                                   "group": label, "stageCount": len(selected_rows),
                                   "calls": sum(r["calls"] for r in selected_rows),
                                   "totalDurationNs": sum(r["totalDurationNs"] for r in selected_rows),
                                   "dominantStage": max(selected_rows, key=lambda r: r["totalDurationNs"])["stageId"] if selected_rows else "none"})

    findings = rank_bottlenecks(stage_rows, cell_rows)
    write_csv(campaign / "cell-summary.csv", cell_rows, list(cell_rows[0]))
    write_csv(campaign / "rate-stage-summary.csv", stage_rows, list(stage_rows[0]) if stage_rows else ["cellId"])
    write_csv(campaign / "critical-path-groups.csv", group_rows, list(group_rows[0]) if group_rows else ["cellId"])
    write_csv(campaign / "path-frequency.csv", path_rows, list(path_rows[0]) if path_rows else ["cellId"])
    write_csv(campaign / "bottleneck-ranking.csv", findings, list(findings[0]) if findings else ["group", "rank", "stageId", "verdict"])
    summary = {"schemaVersion": "spec133-campaign-summary-v1", "campaignId": manifest["campaignId"],
               "subjectManifestSha256": manifest["subjectManifestSha256"],
               "overheadVerdict": overhead["verdict"], "receiptCount": 5,
               "rates": list(RATES), "validCells": sum(r["valid"] for r in cell_rows),
               "invalidCells": invalid_cells, "throughputBoundaryRate": rate_boundary(cell_rows),
               "stageRowCount": len(stage_rows), "findingCount": len(findings),
               "containmentViolations": sum(
                   row["containmentViolations"] for row in cell_rows
               )}
    (campaign / "campaign-summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    supported = [f for f in findings if f["verdict"] == "supported"]
    report = ["# Spec 133 Synchronous NDN-SVS Bottleneck Report", "",
              f"Campaign: `{manifest['campaignId']}`. All five immutable receipts were analyzed; {summary['validCells']} cells were schema-valid.", "",
              "## Result", ""]
    if supported:
        for finding in supported:
            report.append(f"- {finding['group']}: `{finding['stageId']}` on {finding['peerId']} (signals={finding['signalCount']}, high rate={finding['highRate']} pps/peer).")
    else:
        report.append("INCONCLUSIVE: no stage met the frozen two-signal rule.")
    report += ["", "CPU demand uses leaf-only all-call totals. Aggregate spans are used only for residual checks; lock, queue, and external waits remain separate.",
               "",
               f"Sampled child/parent containment mismatches: {summary['containmentViolations']}; these spans remain in per-stage demand but are excluded from aggregate residual attribution.",
               "", "All results are descriptive: there is one run per rate and no run-level confidence interval."]
    (campaign / "bottleneck-report.md").write_text("\n".join(report) + "\n", encoding="utf-8")
    (campaign / "limitations.md").write_text(
        "# Limitations\n\n- One once-only observation per rate; no inferential population claim.\n"
        "- Rates ran in ascending order, so temporal drift is not randomized.\n"
        "- Profiling passed a bounded short-run gate but is not zero-overhead.\n"
        "- Results cover two peers, 256-byte non-segmented payloads, compression off, and the frozen security/topology only.\n"
        "- Ambiguous/censored correlations are excluded from external-wait and critical-path claims.\n"
        "- Sampled children outside a same-trace aggregate parent are excluded from aggregate residual attribution and reported as containment violations.\n",
        encoding="utf-8")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("campaign", type=Path)
    args = parser.parse_args()
    summary = analyze(args.campaign)
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
