#!/usr/bin/env python3
"""Join NDNSF lifecycle TRACE events with the enforced mobility trace.

The analyzer is intentionally post-processing only: it does not change the
experiment or protocol.  REQUEST_PUBLISHED timestamps are aligned to the
campaign epoch recorded in summary.json, then joined to the last enforced
coverage epoch in mobility_trace.csv.
"""

import argparse
import csv
import hashlib
import json
import re
from collections import defaultdict
from pathlib import Path


PROVIDER_NODE_TO_LABEL = {
    "ucla": "A",
    "wustl": "B",
    "uiuc": "C",
    "arizona": "D",
}
PROVIDER_LABELS = tuple(PROVIDER_NODE_TO_LABEL.values())
FIELD_RE = re.compile(r"([A-Za-z][A-Za-z0-9_]*)=([^\s]+)")


def parse_fields(line):
    return dict(FIELD_RE.findall(line))


def provider_label(fields, path):
    name = fields.get("providerName", "")
    if name:
        return name.rstrip("/").rsplit("/", 1)[-1]
    match = re.search(r"provider-([A-D])\.log$", path.name)
    return match.group(1) if match else "?"


def add_event(events, request_id, stage, timestamp_us, provider=None):
    if not request_id or timestamp_us is None:
        return
    record = events[request_id][stage]
    record["timestamps"].append(timestamp_us)
    if provider and provider != "?":
        record["providers"].add(provider)


def parse_logs(run_dir):
    events = defaultdict(
        lambda: defaultdict(lambda: {"timestamps": [], "providers": set()}))
    for path in sorted(run_dir.glob("ndnsf-*.log")):
        with path.open(encoding="utf-8", errors="replace") as stream:
            for line in stream:
                if "[NDNSF_TRACE]" not in line:
                    continue
                fields = parse_fields(line)
                event = fields.get("event")
                try:
                    timestamp_us = int(fields["timestamp_us"])
                except (KeyError, ValueError):
                    continue
                request_id = fields.get("requestId")
                provider = provider_label(fields, path)

                if event == "REQUEST_PUBLISHED":
                    add_event(events, request_id, "request_published", timestamp_us)
                elif event == "REQUEST_RECEIVED":
                    add_event(events, request_id, "provider_request_fetched",
                              timestamp_us, provider)
                elif event == "ACK_PUBLISHED":
                    # This marker is emitted when the ACK publication API is
                    # entered, before encryption and the SVS publish call.
                    add_event(events, request_id, "ack_publish_called",
                              timestamp_us, provider)
                elif event == "SVS_PUBLISH_BEGIN" and "/NDNSF/ACK/" in fields.get(
                        "messageName", ""):
                    component = fields["messageName"].rstrip("/").rsplit("/", 1)[-1]
                    add_event(events, "/" + component, "ack_publish_started",
                              timestamp_us, provider)
                elif event == "SVS_PUBLISH_DONE" and "/NDNSF/ACK/" in fields.get(
                        "messageName", ""):
                    component = fields["messageName"].rstrip("/").rsplit("/", 1)[-1]
                    add_event(events, "/" + component, "ack_publish_done",
                              timestamp_us, provider)
                elif event == "ACK_MATCH_ATTEMPT" and fields.get("phase") == "pre_decrypt":
                    add_event(events, request_id, "user_ack_fetched",
                              timestamp_us, provider)
                elif event == "ACK_MATCHED_PENDING_CALL":
                    add_event(events, request_id, "ack_matched", timestamp_us, provider)
                elif event == "PROVIDER_SELECTED":
                    selected = fields.get("selectedProvider", "").rstrip("/").rsplit("/", 1)[-1]
                    add_event(events, request_id, "provider_selected",
                              timestamp_us, selected)
                elif event == "RESPONSE_RETRY_CANDIDATE_STORED":
                    add_event(events, request_id, "response_retry_candidate",
                              timestamp_us, provider)
                elif event == "RESPONSE_ATTEMPT_STARTED":
                    add_event(events, request_id, "response_attempt_started",
                              timestamp_us, provider)
                elif event == "RESPONSE_ATTEMPT_TIMEOUT":
                    add_event(events, request_id, "response_attempt_timeout",
                              timestamp_us, provider)
                elif event == "RESPONSE_RESELECTION":
                    add_event(events, request_id, "response_reselection",
                              timestamp_us, provider)
                elif event == "RESPONSE_RETRY_EXHAUSTED":
                    add_event(events, request_id, "response_retry_exhausted",
                              timestamp_us)
                elif event == "RESPONSE_OBSERVED":
                    add_event(events, request_id, "response_observed",
                              timestamp_us, provider)
                elif event == "CALLBACK_FIRED":
                    add_event(events, request_id, "response_callback", timestamp_us)
                elif event == "TIMEOUT_FIRED":
                    add_event(events, request_id, "timeout", timestamp_us)
    # Older runs did not emit SVS_PUBLISH_DONE on the hybrid encryption path.
    # Keep them analyzable, but preserve whether ACK publication is confirmed
    # or inferred from entry into the synchronous publish call.
    for stages in events.values():
        source = (stages["ack_publish_done"] if
                  stages["ack_publish_done"]["timestamps"] else
                  stages["ack_publish_started"])
        stages["ack_published"]["timestamps"].extend(source["timestamps"])
        stages["ack_published"]["providers"].update(source["providers"])
    return events


def load_summary(run_dir):
    payload = json.loads((run_dir / "summary.json").read_text(encoding="utf-8"))
    summaries = payload.get("summaries", [])
    if len(summaries) != 1:
        raise ValueError("expected exactly one summary entry")
    return summaries[0]


def load_coverage(run_dir):
    by_time = defaultdict(dict)
    actual_updates = defaultdict(list)
    with (run_dir / "mobility_trace.csv").open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            label = PROVIDER_NODE_TO_LABEL.get(row["provider"])
            if label:
                by_time[float(row["time_s"])][label] = int(row["in_range"])
                if row.get("applied_unix_s"):
                    actual_updates[label].append((
                        int(round(float(row["applied_unix_s"]) * 1_000_000.0)),
                        int(row["in_range"]),
                        float(row["time_s"]),
                    ))
    epochs = sorted(by_time)
    if not epochs:
        raise ValueError("mobility_trace.csv contains no coverage epochs")
    if actual_updates and set(actual_updates) != set(PROVIDER_LABELS):
        raise ValueError("actual coverage timestamps are incomplete")
    return epochs, by_time, actual_updates


def last_epoch_at_or_before(epochs, timestamp_s):
    lo, hi = 0, len(epochs)
    while lo < hi:
        mid = (lo + hi) // 2
        if epochs[mid] <= timestamp_s:
            lo = mid + 1
        else:
            hi = mid
    return epochs[max(0, lo - 1)]


def last_update_at_or_before(updates, timestamp_us):
    lo, hi = 0, len(updates)
    while lo < hi:
        mid = (lo + hi) // 2
        if updates[mid][0] <= timestamp_us:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0:
        raise ValueError("request predates the first applied coverage state")
    return updates[lo - 1]


def first_timestamp(stage):
    values = stage.get("timestamps", [])
    return min(values) if values else None


def delta_ms(timestamp_us, request_us):
    if timestamp_us is None:
        return ""
    return f"{(timestamp_us - request_us) / 1000.0:.3f}"


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def timing_summary(rows, outcome, stage):
    values = sorted(
        float(row[f"{stage}_delta_ms"])
        for row in rows
        if row["outcome"] == outcome and row[f"{stage}_delta_ms"])
    if not values:
        return {"count": 0}

    def percentile(fraction):
        return values[min(len(values) - 1, int(fraction * (len(values) - 1)))]

    return {
        "count": len(values),
        "p50_ms": round(percentile(0.50), 3),
        "p95_ms": round(percentile(0.95), 3),
        "max_ms": round(values[-1], 3),
        "within_500_ms": sum(value <= 500.0 for value in values),
        "within_5000_ms": sum(value <= 5000.0 for value in values),
    }


def analyze(run_dir):
    summary = load_summary(run_dir)
    epochs, coverage, actual_updates = load_coverage(run_dir)
    events = parse_logs(run_dir)
    request_ids = [
        request_id for request_id, stages in events.items()
        if stages["request_published"]["timestamps"]
    ]
    request_ids.sort(key=lambda request_id: first_timestamp(
        events[request_id]["request_published"]))
    if not request_ids:
        raise ValueError("no REQUEST_PUBLISHED TRACE events found")

    first_publish_us = first_timestamp(events[request_ids[0]]["request_published"])
    launch_offset_s = float(summary["traffic_launch_offset_s"])
    rows = []
    for index, request_id in enumerate(request_ids):
        stages = events[request_id]
        published_us = first_timestamp(stages["request_published"])
        trace_time_s = launch_offset_s + (published_us - first_publish_us) / 1_000_000.0
        epoch_s = last_epoch_at_or_before(epochs, trace_time_s)
        if actual_updates:
            applied = {
                label: last_update_at_or_before(actual_updates[label], published_us)
                for label in PROVIDER_LABELS
            }
            state = {label: applied[label][1] for label in PROVIDER_LABELS}
            applied_us = {label: applied[label][0] for label in PROVIDER_LABELS}
            coverage_basis = "actual_gate_application"
        else:
            state = {label: coverage[epoch_s].get(label, 0) for label in PROVIDER_LABELS}
            applied_us = {}
            coverage_basis = "scheduled_trace_epoch"
        reachable = [label for label in PROVIDER_LABELS if state[label]]
        row = {
            "request_index": index,
            "request_id": request_id,
            "request_published_us": published_us,
            "trace_time_s": f"{trace_time_s:.6f}",
            "coverage_epoch_s": f"{epoch_s:.3f}",
            "coverage_basis": coverage_basis,
            "coverage_state": "|".join(f"{label}={state[label]}" for label in PROVIDER_LABELS),
            "coverage_last_applied_us": "|".join(
                f"{label}={applied_us[label]}" for label in PROVIDER_LABELS)
                if applied_us else "",
            "reachable_count": len(reachable),
            "reachable_providers": "|".join(reachable),
        }
        for stage in (
                "provider_request_fetched", "ack_publish_called",
                "ack_publish_started", "ack_published",
                "user_ack_fetched", "ack_matched", "provider_selected",
                "response_retry_candidate", "response_attempt_started",
                "response_attempt_timeout", "response_reselection",
                "response_retry_exhausted", "response_observed"):
            stage_record = stages[stage]
            timestamp_us = first_timestamp(stage_record)
            row[f"{stage}_us"] = timestamp_us if timestamp_us is not None else ""
            row[f"{stage}_delta_ms"] = delta_ms(timestamp_us, published_us)
            row[f"{stage}_count"] = len(stage_record["timestamps"])
            row[f"{stage}_providers"] = "|".join(sorted(stage_record["providers"]))
        row["outcome"] = (
            "success" if stages["response_callback"]["timestamps"] else
            "timeout" if stages["timeout"]["timestamps"] else "unknown")
        rows.append(row)

    csv_path = run_dir / "request-lifecycle-coverage.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    stage_names = (
        "provider_request_fetched", "ack_publish_called",
        "ack_publish_started", "ack_published",
        "user_ack_fetched", "ack_matched", "provider_selected",
        "response_retry_candidate", "response_attempt_started",
        "response_attempt_timeout", "response_reselection",
        "response_retry_exhausted", "response_observed")
    stage_unique = {
        stage: sum(1 for row in rows if row[f"{stage}_count"] > 0)
        for stage in stage_names
    }
    by_coverage = {}
    for reachable_count in sorted({row["reachable_count"] for row in rows}):
        subset = [row for row in rows if row["reachable_count"] == reachable_count]
        by_coverage[str(reachable_count)] = {
            "requests": len(subset),
            "success": sum(row["outcome"] == "success" for row in subset),
            "selected": sum(row["provider_selected_count"] > 0 for row in subset),
            "ack_fetched": sum(row["user_ack_fetched_count"] > 0 for row in subset),
        }
    by_coverage_state = {}
    for state in sorted({row["coverage_state"] for row in rows}):
        subset = [row for row in rows if row["coverage_state"] == state]
        by_coverage_state[state] = {
            "requests": len(subset),
            "success": sum(row["outcome"] == "success" for row in subset),
            "ack_fetched": sum(row["user_ack_fetched_count"] > 0 for row in subset),
        }
    success_indices = [
        int(row["request_index"]) for row in rows if row["outcome"] == "success"]
    timeout_indices = [
        int(row["request_index"]) for row in rows if row["outcome"] == "timeout"]
    timing = {
        outcome: {
            stage: timing_summary(rows, outcome, stage)
            for stage in stage_names
        }
        for outcome in ("success", "timeout")
    }
    report = {
        "schema": "ndnsf-mobility-request-lifecycle-v1",
        "run_dir": str(run_dir.resolve()),
        "requests": len(rows),
        "success": sum(row["outcome"] == "success" for row in rows),
        "timeout": sum(row["outcome"] == "timeout" for row in rows),
        "stage_unique_request_counts": stage_unique,
        "by_reachable_provider_count_at_publish": by_coverage,
        "by_coverage_state_at_publish": by_coverage_state,
        "outcome_order": {
            "last_success_request_index": max(success_indices) if success_indices else None,
            "first_timeout_request_index": min(timeout_indices) if timeout_indices else None,
            "successes_are_contiguous_prefix": success_indices == list(range(len(success_indices))),
        },
        "stage_timing_by_outcome": timing,
        "response_retry": {
            "requests_with_candidates": sum(
                row["response_retry_candidate_count"] > 0 for row in rows),
            "requests_with_attempt_timeout": sum(
                row["response_attempt_timeout_count"] > 0 for row in rows),
            "requests_with_reselection": sum(
                row["response_reselection_count"] > 0 for row in rows),
            "total_reselections": sum(
                row["response_reselection_count"] for row in rows),
            "successful_after_reselection": sum(
                row["response_reselection_count"] > 0 and
                row["outcome"] == "success" for row in rows),
            "timed_out_after_reselection": sum(
                row["response_reselection_count"] > 0 and
                row["outcome"] == "timeout" for row in rows),
            "requests_with_exhaustion_marker": sum(
                row["response_retry_exhausted_count"] > 0 for row in rows),
        },
        "coverage_alignment": {
            "method": (
                "last actual per-provider gate application at REQUEST_PUBLISHED time"
                if actual_updates else
                "last scheduled replay epoch at REQUEST_PUBLISHED time"
            ),
            "uses_actual_gate_timestamps": bool(actual_updates),
            "traffic_launch_offset_s": launch_offset_s,
            "first_request_published_us": first_publish_us,
            "trace_epoch_resolution_s": min(
                (b - a for a, b in zip(epochs, epochs[1:])), default=None),
        },
        "ack_published_definition": (
            "SVS_PUBLISH_DONE for an ACK name when available; legacy runs fall "
            "back to SVS_PUBLISH_BEGIN. ACK_PUBLISHED is retained separately "
            "as ack_publish_called because it precedes encryption"
        ),
        "ack_published_evidence": {
            "done": sum(
                bool(events[request_id]["ack_publish_done"]["timestamps"])
                for request_id in request_ids),
            "begin_fallback": sum(
                not events[request_id]["ack_publish_done"]["timestamps"] and
                bool(events[request_id]["ack_publish_started"]["timestamps"])
                for request_id in request_ids),
        },
        "input_hashes": {
            "mobility_trace_sha256": sha256_file(run_dir / "mobility_trace.csv"),
            "runtime_commands_sha256": sha256_file(run_dir / "runtime-commands.json"),
        },
        "csv": str(csv_path.resolve()),
    }
    json_path = run_dir / "request-lifecycle-coverage-summary.json"
    json_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dir", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.run_dir), indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
