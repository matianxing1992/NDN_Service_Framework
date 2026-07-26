#!/usr/bin/env python3
"""Validate and summarize Spec 131 pure NDN-SVS PubSub evidence."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics
from typing import Any, Iterable


RATES = (200, 400, 600, 800, 1000)
SUBJECTS = ("baseline-sync-serial", "latest-async-parallel")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip()]


def nearest_rank(values: Iterable[float], percentile: float) -> float | None:
    ordered = sorted(values)
    if not ordered:
        return None
    rank = max(1, math.ceil(percentile * len(ordered)))
    return ordered[rank - 1]


def percentile_triplet(values: list[float]) -> dict[str, float | None]:
    return {"p50": nearest_rank(values, 0.50), "p95": nearest_rank(values, 0.95),
            "p99": nearest_rank(values, 0.99), "max": max(values) if values else None}


def parse_network(path: Path) -> dict[str, int]:
    if not path.is_file():
        return {}
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            result[key] = int(value.strip())
    return result


def resource_metrics(cell: Path) -> dict[str, Any]:
    samples = read_jsonl(cell / "resource-samples.jsonl")
    processes = {}
    for role in ("publisher", "subscriber"):
        entries = [sample.get("processes", {}).get(role, {}) for sample in samples]
        resolved = [entry for entry in entries if entry.get("peerResolved")]
        rss = [int(entry["VmRSS"].split()[0]) for entry in resolved if "VmRSS" in entry]
        ticks = [int(entry["cpuTicks"]) for entry in resolved if "cpuTicks" in entry]
        processes[role] = {"samples": len(resolved), "peerResolved": bool(resolved),
                           "peakRssKb": max(rss) if rss else None,
                           "cpuTicksDelta": max(ticks) - min(ticks) if ticks else None}
    network = {}
    for role in ("publisher", "subscriber"):
        before = parse_network(cell / f"{role}-network-before.txt")
        after = parse_network(cell / f"{role}-network-final.txt")
        network[role] = {key: after[key] - before.get(key, 0) for key in after}
    return {"processes": processes, "network": network}


def summarize_cell(cell: Path, config: dict[str, Any]) -> dict[str, Any]:
    pub = read_jsonl(cell / "publisher.jsonl")
    sub = read_jsonl(cell / "subscriber.jsonl")
    attempted = {int(e["logicalId"]): e for e in pub
                 if e.get("event") == "attempted" and e.get("phase") == "measured"}
    completed = {int(e["logicalId"]): e for e in pub
                 if e.get("event") == "api-return" and e.get("phase") == "measured"}
    enters = {int(e["logicalId"]): e for e in pub
              if e.get("event") == "api-enter" and e.get("phase") == "measured"}
    scheduled = {int(e["logicalId"]): e for e in pub
                 if e.get("event") == "scheduled" and e.get("phase") == "measured"}
    first: dict[int, dict[str, Any]] = {}
    duplicate = 0
    invalid = 0
    order: list[int] = []
    for event in sub:
        if event.get("phase") != "measured":
            continue
        if event.get("event") == "delivery":
            logical = int(event["logicalId"])
            if logical in first:
                duplicate += 1
            else:
                first[logical] = event
                order.append(logical)
            if not event.get("details", {}).get("payloadValid", False):
                invalid += 1
        elif event.get("event") == "duplicate":
            duplicate += 1
    unexpected = sorted(set(first) - set(attempted))
    delivered = {logical: event for logical, event in first.items() if logical in attempted}
    missing = sorted(set(attempted) - set(delivered))
    delays_ms = [(event["monotonicRawNs"] - scheduled[logical]["monotonicRawNs"]) / 1e6
                 for logical, event in delivered.items() if logical in scheduled]
    if any(value < 0 for value in delays_ms):
        raise ValueError(f"negative delay in {cell}")
    api_ms = [(completed[i]["monotonicRawNs"] - enters[i]["monotonicRawNs"]) / 1e6
              for i in completed if i in enters]
    drain_ns = int(config["drainSeconds"] * 1e9)
    capped = list(delays_ms) + [drain_ns / 1e6 for _ in missing]
    update_by_seq: dict[int, int] = {}
    for event in sub:
        if event.get("event") == "state-update":
            seq = int(event.get("svsSeqNo", 0))
            update_by_seq.setdefault(seq, int(event["monotonicRawNs"]))
    update_ms = []
    for logical, event in delivered.items():
        seq = int(event.get("svsSeqNo", 0))
        if seq in update_by_seq and logical in scheduled:
            update_ms.append((update_by_seq[seq] - scheduled[logical]["monotonicRawNs"]) / 1e6)
    reordered = sum(1 for a, b in zip(order, order[1:]) if b < a)
    worker_events = [e for e in sub + pub if e.get("event") == "worker-stats"]
    worker = None if config["subject"] == SUBJECTS[0] else [e.get("details") for e in worker_events]
    adapter_events = [e for e in pub if e.get("event") == "adapter-stats" and
                      isinstance(e.get("details"), dict)]
    adapter = None
    if adapter_events:
        adapter = dict(adapter_events[-1]["details"])
        attempted_all = [e for e in pub if e.get("event") == "attempted"]
        entered_all = [e for e in pub if e.get("event") == "api-enter"]
        enqueue_ns = [int(e.get("details", {}).get("enqueueLatencyNs"))
                      for e in attempted_all
                      if isinstance(e.get("details"), dict) and
                      e["details"].get("enqueueLatencyNs") is not None]
        queue_depths = [int(e.get("details", {}).get("queueDepth"))
                        for e in attempted_all
                        if isinstance(e.get("details"), dict) and
                        e["details"].get("queueDepth") is not None]
        admitted = int(adapter.get("admitted", 0))
        remaining = int(adapter.get("remainingAtShutdown", 0))
        adapter.update({
            "attemptedEvents": len(attempted_all),
            "apiEntered": len(entered_all),
            "enqueueLatencyNs": percentile_triplet(enqueue_ns),
            "observedQueueDepth": percentile_triplet(queue_depths),
            "accountingReconciled": (admitted == len(attempted_all) and
                                      admitted == len(entered_all) + remaining),
        })
    elapsed = float(config["measureSeconds"])
    summary = {
        "schemaVersion": "spec131-cell-summary-v1", "cellId": config["cellId"],
        "subject": config["subject"], "ratePps": config["ratePps"],
        "repetition": config["repetition"], "status": "COMPLETE",
        "scheduledMeasured": len(scheduled), "emittedMeasured": len(attempted),
        "deliveredMeasured": len(delivered), "missingMeasured": len(missing),
        "duplicates": duplicate, "reorderTransitions": reordered,
        "invalidPayloads": invalid, "unexpectedLogicalIds": unexpected,
        "attemptedMeasured": len(attempted), "apiCompletedMeasured": len(completed),
        "offerFulfillmentRatio": len(attempted) / len(scheduled) if scheduled else 0.0,
        "deliveryAttemptedRatio": len(delivered) / len(attempted) if attempted else 0.0,
        "deliveryRatio": len(delivered) / len(attempted) if attempted else 0.0,
        "attemptedRatePps": len(attempted) / elapsed,
        "achievedRatePps": len(attempted) / elapsed,
        "senderLimited": abs(len(attempted) / elapsed - config["ratePps"]) / config["ratePps"] > 0.02,
        "deliveredDelayMs": percentile_triplet(delays_ms),
        "deadlineCappedDelayMs": percentile_triplet(capped),
        "stateUpdateDelayMs": percentile_triplet(update_ms),
        "publishApiDurationMs": percentile_triplet(api_ms),
        "resourceMetrics": resource_metrics(cell),
        "sourceIdentity": {"baseCommit": config.get("baseCommit"),
                           "binarySha256": config.get("binarySha256"),
                           "librarySha256": config.get("librarySha256")},
        "terminalClassification": "sender-limited" if abs(len(attempted) / elapsed - config["ratePps"]) / config["ratePps"] > 0.02 else "valid-rate",
        "workerStats": worker,
        "adapterMetrics": adapter,
        "rawHashes": {name: sha256_file(cell / name) for name in
                      ("publisher.jsonl", "subscriber.jsonl") if (cell / name).is_file()},
    }
    if unexpected or invalid:
        summary["status"] = "INVALID"
    (cell / "cell-summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n",
                                             encoding="utf-8")
    return summary


def validate_manifest(manifest: dict[str, Any]) -> None:
    cells = manifest.get("cells", [])
    if len(cells) != 10 or len({cell["cellId"] for cell in cells}) != 10:
        raise ValueError("formal manifest must contain 10 unique cells")
    if any(cell["attempt"] != 1 for cell in cells):
        raise ValueError("formal cells must have exactly one attempt")
    if [cell["subject"] for cell in cells[:5]] != [SUBJECTS[0]] * 5 or \
       [cell["subject"] for cell in cells[5:]] != [SUBJECTS[1]] * 5:
        raise ValueError("formal order must be 5 baseline then 5 treatment")
    for subject in SUBJECTS:
        subset = [cell for cell in cells if cell["subject"] == subject]
        if sorted((cell["ratePps"], cell["repetition"]) for cell in subset) != \
           sorted((rate, 1) for rate in RATES):
            raise ValueError(f"incomplete rate/repetition grid for {subject}")
    baseline = [(c["ratePps"], c["repetition"]) for c in cells[:5]]
    treatment = [(c["ratePps"], c["repetition"]) for c in cells[5:]]
    if baseline != treatment:
        raise ValueError("subject schedules are not matched")


def aggregate(campaign: Path) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    manifest_path = campaign / "campaign-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    validate_manifest(manifest)
    summaries = []
    for cell_config in manifest["cells"]:
        cell = campaign / "cells" / cell_config["cellId"]
        receipt_path = cell / "attempt-receipt.json"
        if not receipt_path.is_file():
            raise ValueError(f"missing once-only receipt: {cell_config['cellId']}")
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        if receipt.get("attempt") != 1:
            raise ValueError(f"invalid attempt receipt: {cell_config['cellId']}")
        summaries.append(summarize_cell(cell, cell_config))
    rows = []
    by_key = {(s["subject"], s["ratePps"], s["repetition"]): s for s in summaries}
    for rate in RATES:
        old = [by_key[(SUBJECTS[0], rate, 1)]]
        new = [by_key[(SUBJECTS[1], rate, 1)]]
        def mean(key: str, field: str | None = None, group=old) -> float | None:
            values = [x[key] if field is None else x[key][field] for x in group]
            present = [value for value in values if value is not None]
            return statistics.mean(present) if present else None
        effects = [new[0]["deliveredDelayMs"]["p95"] - old[0]["deliveredDelayMs"]["p95"]]
        if new[0]["deliveredDelayMs"]["p95"] is None or old[0]["deliveredDelayMs"]["p95"] is None:
            effects = []
        old_delivery, new_delivery = mean("deliveryRatio"), mean("deliveryRatio", group=new)
        old_p95 = mean("deliveredDelayMs", "p95")
        new_p95 = mean("deliveredDelayMs", "p95", new)
        sender_limited = any(item["senderLimited"] for item in old + new)
        if sender_limited:
            classification = "inconclusive"
        elif new_delivery > old_delivery + 0.001:
            classification = "improved"
        elif new_delivery < old_delivery - 0.001:
            classification = "regressed"
        elif old_p95 is not None and new_p95 is not None and new_p95 < old_p95 * 0.95:
            classification = "improved"
        elif old_p95 is not None and new_p95 is not None and new_p95 > old_p95 * 1.05:
            classification = "regressed"
        else:
            classification = "inconclusive"
        rows.append({
            "ratePps": rate,
            "replicationCount": 1,
            "baselineScheduled": old[0]["scheduledMeasured"],
            "latestScheduled": new[0]["scheduledMeasured"],
            "baselineAttempted": old[0]["attemptedMeasured"],
            "latestAttempted": new[0]["attemptedMeasured"],
            "baselineApiCompleted": old[0]["apiCompletedMeasured"],
            "latestApiCompleted": new[0]["apiCompletedMeasured"],
            "baselineDelivered": old[0]["deliveredMeasured"],
            "latestDelivered": new[0]["deliveredMeasured"],
            "baselineOfferFulfillmentPct": old[0]["offerFulfillmentRatio"] * 100,
            "latestOfferFulfillmentPct": new[0]["offerFulfillmentRatio"] * 100,
            "baselineDeliveryPct": old_delivery * 100, "latestDeliveryPct": new_delivery * 100,
            "deliveryDeltaPp": (new_delivery - old_delivery) * 100,
            "baselineAchievedPps": mean("achievedRatePps"),
            "latestAchievedPps": mean("achievedRatePps", group=new),
            "baselineP50Ms": mean("deliveredDelayMs", "p50"),
            "latestP50Ms": mean("deliveredDelayMs", "p50", new),
            "baselineP95Ms": old_p95,
            "latestP95Ms": new_p95,
            "baselineP99Ms": mean("deliveredDelayMs", "p99"),
            "latestP99Ms": mean("deliveredDelayMs", "p99", new),
            "directP95DeltaMs": effects[0] if effects else None,
            "classification": classification,
            "senderLimitedCells": sum(item["senderLimited"] for item in old + new),
        })
    return summaries, rows


def write_outputs(campaign: Path, summaries: list[dict[str, Any]], rows: list[dict[str, Any]]) -> None:
    with (campaign / "cell-comparison.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(summaries[0].keys()))
        writer.writeheader()
        for item in summaries:
            writer.writerow({key: json.dumps(value, sort_keys=True) if isinstance(value, (dict, list)) else value
                             for key, value in item.items()})
    with (campaign / "rate-comparison.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        for item in rows:
            writer.writerow({key: json.dumps(value) if isinstance(value, list) else value
                             for key, value in item.items()})
    lines = ["# Spec 131 Rate Comparison", "",
             "Each row is one direct old/new observation. It is descriptive, has no replication-based uncertainty interval, and does not isolate async or threading causality.", "",
             "| Rate | Baseline scheduled/attempted/API-completed/delivered | Latest scheduled/attempted/API-completed/delivered | Baseline attempted/scheduled | Latest attempted/scheduled | Baseline delivered/attempted | Latest delivered/attempted | Baseline attempted pps | Latest attempted pps | Baseline p50/p95/p99 ms | Latest p50/p95/p99 ms | Direct p95 delta ms | Sender-limited | Classification |",
             "|---:|:---|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:---|"]
    f = lambda v: "n/a" if v is None else f"{v:.3f}"
    for r in rows:
        lines.append(f"| {r['ratePps']} | {r['baselineScheduled']}/{r['baselineAttempted']}/{r['baselineApiCompleted']}/{r['baselineDelivered']} | {r['latestScheduled']}/{r['latestAttempted']}/{r['latestApiCompleted']}/{r['latestDelivered']} | {r['baselineOfferFulfillmentPct']:.3f}% | {r['latestOfferFulfillmentPct']:.3f}% | {r['baselineDeliveryPct']:.3f}% | {r['latestDeliveryPct']:.3f}% | {r['baselineAchievedPps']:.3f} | {r['latestAchievedPps']:.3f} | {f(r['baselineP50Ms'])}/{f(r['baselineP95Ms'])}/{f(r['baselineP99Ms'])} | {f(r['latestP50Ms'])}/{f(r['latestP95Ms'])}/{f(r['latestP99Ms'])} | {f(r['directP95DeltaMs'])} | {r['senderLimitedCells']}/2 | {r['classification']} |")
    highest_sustained = {}
    for subject in SUBJECTS:
        valid = [s["ratePps"] for s in summaries if s["subject"] == subject and
                 not s["senderLimited"] and s["status"] == "COMPLETE" and
                 s["deliveryAttemptedRatio"] >= 0.99 and s["missingMeasured"] == 0]
        highest_sustained[subject] = max(valid) if valid else None
    (campaign / "rate-comparison.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (campaign / "campaign-summary.json").write_text(json.dumps(
        {"schemaVersion": "spec131-campaign-summary-v1", "cellCount": len(summaries),
         "rateRows": rows, "highestSustainedRatePps": highest_sustained,
         "status": "COMPLETE"}, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("campaign", type=Path)
    parser.add_argument("--cell", type=Path)
    parser.add_argument("--config", type=Path)
    args = parser.parse_args()
    if args.cell:
        config = json.loads(args.config.read_text(encoding="utf-8"))
        print(json.dumps(summarize_cell(args.cell, config), sort_keys=True))
        return 0
    summaries, rows = aggregate(args.campaign)
    write_outputs(args.campaign, summaries, rows)
    print(args.campaign / "rate-comparison.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
