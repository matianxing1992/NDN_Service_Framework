#!/usr/bin/env python3
"""Analyze sampled NDNSF Stream latency without inventing correlations.

The analyzer accepts only same-role, same-requestId, same-clock intervals.
Decoder output additionally needs an explicit exact cardinality binding.  A
FIFO position or a coincident numeric cursor is never sufficient because one
H.264 input group can yield many decoded frames.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
from typing import Iterable


FIELD_RE = re.compile(r"([A-Za-z_]+)=([^\s]+)")
ROLE_STAGES = {
    "provider": (
        "encoded-output-ready", "group-ready", "protection-complete",
        "signed-and-materialized", "data-put",
    ),
    "consumer": (
        "data-received", "signature-validated", "decrypted",
        "reorder-ready", "decoder-input", "decoder-output", "gui-delivered",
    ),
}


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1,
                max(0, int(round((len(ordered) - 1) * fraction))))
    return float(ordered[index])


def distribution(values: list[float]) -> dict[str, float | int | None]:
    return {
        "samples": len(values),
        "mean": (sum(values) / len(values)) if values else None,
        "p50": percentile(values, 0.50),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
    }


def analyze_frame_observations(
        observations: Iterable[dict[str, int | str]]) -> dict[str, object]:
    """Admit only source-to-widget samples independently confirmed in pixels."""
    rejected: Counter[str] = Counter()
    accepted: list[float] = []
    identities: set[tuple[str, int, int]] = set()
    for value in observations:
        stream_id = str(value.get("streamId", ""))
        session_epoch = value.get("sessionEpoch")
        source_frame_id = value.get("sourceFrameId")
        capture_origin_ns = value.get("captureOriginNs")
        oracle_frame_id = value.get("oracleFrameId")
        oracle_capture_ns = value.get("oracleCaptureOriginNs")
        widget_ns = value.get("widgetSubmittedNs")
        if not stream_id or not all(isinstance(item, int) for item in (
                session_epoch, source_frame_id, capture_origin_ns,
                oracle_frame_id, oracle_capture_ns, widget_ns)):
            rejected["missing-identity-or-endpoint"] += 1
            continue
        active_epoch = value.get("activeSessionEpoch", session_epoch)
        if active_epoch != session_epoch:
            rejected["stale-session"] += 1
            continue
        identity = (stream_id, int(session_epoch), int(source_frame_id))
        if identity in identities:
            rejected["duplicate-source-identity"] += 1
            continue
        identities.add(identity)
        if (source_frame_id != oracle_frame_id or
                capture_origin_ns != oracle_capture_ns):
            rejected["oracle-runtime-mismatch"] += 1
            continue
        if int(widget_ns) < int(capture_origin_ns):
            rejected["non-monotonic-capture-to-widget"] += 1
            continue
        accepted.append((int(widget_ns) - int(capture_origin_ns)) / 1_000_000.0)
    return {
        "acceptedFrames": len(accepted),
        "rejectedFrames": dict(sorted(rejected.items())),
        "captureToWidgetMs": distribution(accepted),
        "identityRule": "runtime-must-match-independent-in-image-oracle",
    }


def _fields(line: str) -> dict[str, str]:
    return dict(FIELD_RE.findall(line))


def _phase(steady_us: int, first_us: int, warmup_us: int) -> str:
    if steady_us == first_us:
        return "startup"
    if steady_us < first_us + warmup_us:
        return "warmup"
    return "steady"


def analyze_texts(texts: Iterable[str], warmup_ms: int = 5000,
                  shared_monotonic_clock: bool = False) -> dict[str, object]:
    """Return honest local-stage distributions and rejection counters."""
    records: dict[tuple[str, str], dict[str, list[dict[str, str]]]] = defaultdict(
        lambda: defaultdict(list))
    event_counts: Counter[str] = Counter()
    first_by_role: dict[str, int] = {}
    frame_records: dict[str, dict[str, dict[str, str]]] = defaultdict(dict)
    rejected: Counter[str] = Counter()

    for text in texts:
        for line in text.splitlines():
            if ("NDNSF_TIMELINE" not in line or
                    ("/NDNSF/STREAM/TIMELINE/" not in line and
                     "/NDNSF/UAV/VIDEO/FRAME/" not in line)):
                continue
            value = _fields(line)
            role = value.get("role", "")
            event = value.get("event", "")
            request_id = value.get("requestId", "")
            steady = value.get("steady_us", "")
            if ("/NDNSF/UAV/VIDEO/FRAME/" in request_id and
                    value.get("frame_correlation") == "exact" and
                    event in ("source-acquired", "encoded-output-ready",
                              "decoder-output", "gui-delivered") and
                    steady.isdigit()):
                if event in frame_records[request_id]:
                    rejected["duplicate-frame-stage"] += 1
                else:
                    frame_records[request_id][event] = value
            if role not in ROLE_STAGES or event not in ROLE_STAGES[role]:
                continue
            if not request_id or not steady.isdigit():
                continue
            steady_us = int(steady)
            first_by_role[role] = min(first_by_role.get(role, steady_us), steady_us)
            records[(role, request_id)][event].append(value)
            event_counts[event] += 1

    intervals: dict[str, dict[str, list[float]]] = defaultdict(
        lambda: defaultdict(list))
    shared_clock_intervals: dict[str, dict[str, list[float]]] = defaultdict(
        lambda: defaultdict(list))
    missing = {
        role: {event: 0 for event in stages}
        for role, stages in ROLE_STAGES.items()
    }

    for (role, _request_id), observed in records.items():
        stages = ROLE_STAGES[role]
        for event in stages:
            if event not in observed:
                missing[role][event] += 1
        for first, second in zip(stages, stages[1:]):
            if first not in observed or second not in observed:
                continue
            if len(observed[first]) != 1 or len(observed[second]) != 1:
                rejected["duplicate-or-ambiguous-stage"] += 1
                continue
            left, right = observed[first][0], observed[second][0]
            left_clock = left.get("clock_domain", f"{role}-steady")
            right_clock = right.get("clock_domain", f"{role}-steady")
            if left_clock != right_clock:
                rejected["cross-clock-unavailable"] += 1
                continue
            # An output cannot inherit a source identity from FIFO order.  It
            # needs an explicit output ordinal and an exact binding supplied by
            # the decoder or codec-aware application.
            if second in ("decoder-output", "gui-delivered") and not (
                right.get("frame_correlation") == "exact" and
                "output_ordinal" in right and
                right.get("source_id") == left.get("source_id") and
                right.get("source_id")
            ):
                rejected["ambiguous-one-to-many"] += 1
                continue
            left_us = int(left["steady_us"])
            right_us = int(right["steady_us"])
            if right_us < left_us:
                rejected["non-monotonic-stage"] += 1
                continue
            phase = _phase(left_us, first_by_role[role], warmup_ms * 1000)
            intervals[f"{first}->{second}"][phase].append(
                (right_us - left_us) / 1000.0)

    if shared_monotonic_clock:
        request_ids = {request_id for _role, request_id in records}
        first_consumer_us = first_by_role.get("consumer", 0)
        for request_id in request_ids:
            provider = records.get(("provider", request_id), {})
            consumer = records.get(("consumer", request_id), {})
            puts = provider.get("data-put", [])
            receives = consumer.get("data-received", [])
            if not puts or not receives:
                continue
            if len(puts) != 1 or len(receives) != 1:
                rejected["duplicate-or-ambiguous-cross-role-stage"] += 1
                continue
            put_us = int(puts[0]["steady_us"])
            receive_us = int(receives[0]["steady_us"])
            if receive_us < put_us:
                rejected["non-monotonic-cross-role-stage"] += 1
                continue
            phase = _phase(receive_us, first_consumer_us, warmup_ms * 1000)
            shared_clock_intervals["data-put->data-received"][phase].append(
                (receive_us - put_us) / 1000.0)

    capture_to_widget: list[float] = []
    capture_to_decode: list[float] = []
    decode_to_widget: list[float] = []
    decoder_output_us: list[int] = []
    for request_id, observed in frame_records.items():
        source = observed.get("source-acquired")
        decoded = observed.get("decoder-output")
        widget = observed.get("gui-delivered")
        if source is None or decoded is None or widget is None:
            rejected["incomplete-exact-frame-timeline"] += 1
            continue
        source_id = source.get("source_id")
        if (not source_id or decoded.get("source_id") != source_id or
                widget.get("source_id") != source_id):
            rejected["frame-source-id-conflict"] += 1
            continue
        if any(value.get("clock_domain") != "host-steady"
               for value in (source, decoded, widget)):
            rejected["frame-cross-clock-unavailable"] += 1
            continue
        capture_ns = int(source.get("capture_origin_ns", "0"))
        decode_ns = int(decoded["steady_us"]) * 1000
        widget_ns = int(widget["steady_us"]) * 1000
        if capture_ns <= 0 or not capture_ns <= decode_ns <= widget_ns:
            rejected["non-monotonic-exact-frame"] += 1
            continue
        capture_to_decode.append((decode_ns - capture_ns) / 1_000_000.0)
        decode_to_widget.append((widget_ns - decode_ns) / 1_000_000.0)
        capture_to_widget.append((widget_ns - capture_ns) / 1_000_000.0)
        decoder_output_us.append(int(decoded["steady_us"]))
    decoder_output_us.sort()
    output_gaps = [
        (right - left) / 1000.0
        for left, right in zip(decoder_output_us, decoder_output_us[1:])
    ]
    # Provider capture can start before the consumer is ready and continue
    # while a lossy stop command is completing. Those frames are outside the
    # consumer's measured presentation interval and must not dilute identity
    # coverage. Keep every provider identity whose capture origin lies between
    # the first and last exactly presented frame; an incomplete identity inside
    # that shared interval remains in the denominator.
    accepted_capture_ns = [
        int(observed["source-acquired"].get("capture_origin_ns", "0"))
        for observed in frame_records.values()
        if all(stage in observed for stage in
               ("source-acquired", "decoder-output", "gui-delivered")) and
           observed["source-acquired"].get("source_id") ==
             observed["decoder-output"].get("source_id") ==
             observed["gui-delivered"].get("source_id")
    ]
    if accepted_capture_ns:
        measurement_start_ns = min(accepted_capture_ns)
        measurement_end_ns = max(accepted_capture_ns)
        observed_frame_identities = sum(
            1 for observed in frame_records.values()
            if (source := observed.get("source-acquired")) is not None and
               source.get("capture_origin_ns", "").isdigit() and
               measurement_start_ns <= int(source["capture_origin_ns"]) <=
                 measurement_end_ns)
    else:
        observed_frame_identities = len(frame_records)

    return {
        "sampledIdentities": len(records),
        "eventCounts": dict(sorted(event_counts.items())),
        "missingStageCounts": missing,
        "rejectedCorrelations": dict(sorted(rejected.items())),
        "localClockStageMs": {
            name: {
                phase: distribution(values)
                for phase, values in sorted(by_phase.items())
            }
            for name, by_phase in sorted(intervals.items())
        },
        "sharedHostClockStageMs": {
            name: {
                phase: distribution(values)
                for phase, values in sorted(by_phase.items())
            }
            for name, by_phase in sorted(shared_clock_intervals.items())
        },
        "crossClockOneWay": (
            "measured-with-shared-host-monotonic-clock"
            if shared_monotonic_clock else
            "unavailable-without-offset-uncertainty"
        ),
        "decoderOutputRule": "requires-explicit-source-id-and-output-ordinal",
        "exactFrameTimeline": {
            "observedIdentities": observed_frame_identities,
            "acceptedFrames": len(capture_to_widget),
            "identityCoverage": (
                len(capture_to_widget) / observed_frame_identities
                if observed_frame_identities else 0.0),
            "measurementWindowRule":
                "provider-capture-within-first-to-last-exact-presentation",
            "captureToDecodeMs": distribution(capture_to_decode),
            "decodeToWidgetMs": distribution(decode_to_widget),
            "captureToWidgetMs": distribution(capture_to_widget),
            "decoderOutputGapMs": distribution(output_gaps),
            "presentationEndpoint": "gtk-widget-submit",
            "physicalPresentation": "unavailable",
        },
        "warmupMs": warmup_ms,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--warmup-ms", type=int, default=5000)
    parser.add_argument("--shared-monotonic-clock", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze_texts(
        (path.read_text(encoding="utf-8", errors="replace") for path in args.logs),
        warmup_ms=args.warmup_ms,
        shared_monotonic_clock=args.shared_monotonic_clock,
    )
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
