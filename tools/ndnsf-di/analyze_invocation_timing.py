#!/usr/bin/env python3
"""Extract per-process measured spans without inferring cross-host clocks."""

from __future__ import annotations

import argparse
from collections import defaultdict, deque
import json
from pathlib import Path
import re


IDENTITY = (
    "requestId", "attempt", "component", "executionRole", "providerBootId",
    "sessionId", "conversationId",
    "inferenceEpoch", "contextEpoch", "providerName", "scope",
    "planDigest", "plannedName", "operationIndex", "round", "microbatch",
)


def parse_fields(record: str) -> dict:
    fields = {}
    for token in record.split():
        key, separator, value = token.partition("=")
        if not separator or not key or not value or key in fields:
            raise ValueError("malformed or duplicate field")
        fields[key] = value
    return fields


def analyze(path: Path) -> dict:
    groups = defaultdict(list)
    gaps = []
    plan_phases = []
    work_phases = []
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line_number, line in enumerate(stream, 1):
            if re.search(r"(?:^|\s)NDNSF_DI_PROVIDER_STAGE ", line):
                try:
                    fields = parse_fields(line.split("NDNSF_DI_PROVIDER_STAGE ", 1)[1])
                    if fields.get("stage") != "DEPENDENCY_FETCH":
                        continue
                    for name in ("requestId", "provider", "role", "sessionId",
                                 "scope", "plannedName", "planDigest"):
                        if not fields.get(name):
                            raise ValueError("missing acquisition identity")
                    for name in ("steady_us", "attemptEpoch", "operationIndex",
                                 "round", "microbatch"):
                        if not re.fullmatch(r"[0-9]+", fields[name]):
                            raise ValueError("invalid acquisition number")
                    time_us = int(fields["steady_us"])
                    if time_us == 0:
                        raise ValueError("missing acquisition clock")
                    suffix = {"begin": "Begin", "complete": "End",
                              "failed": "Failed"}[fields["status"]]
                except (KeyError, ValueError):
                    # Historical records have no exact edge or monotonic clock.
                    gaps.append({"line": line_number,
                                 "reason": "INCOMPLETE_DEPENDENCY_ACQUISITION_RECORD"})
                    continue
                fields.update(component="di-dependency", executionRole=fields["role"],
                              providerName=fields["provider"], attempt=fields["attemptEpoch"])
                key = tuple(fields.get(name, "none") for name in IDENTITY)
                groups[key].append((time_us, "dependencyAcquisition" + suffix, line_number))
                continue
            if re.search(r"(?:^|\s)NDNSF_DI_NATIVE_WORK_PHASE ", line):
                try:
                    fields = parse_fields(line.split("NDNSF_DI_NATIVE_WORK_PHASE ", 1)[1])
                    if not fields.get("phase") or not fields.get("requestId"):
                        raise ValueError("missing work phase identity")
                    for name in ("duration_us", "thread_cpu_us", "attempt"):
                        if name not in fields and name != "duration_us":
                            continue
                        if not re.fullmatch(r"[0-9]+", fields[name]):
                            raise ValueError("invalid work phase number")
                    if "attempt" in fields and int(fields["attempt"]) == 0:
                        raise ValueError("invalid supplied attempt")
                    duration_us = int(fields["duration_us"])
                    cpu_us = int(fields["thread_cpu_us"]) if "thread_cpu_us" in fields else None
                    attempt = int(fields["attempt"]) if "attempt" in fields else None
                except (KeyError, ValueError):
                    gaps.append({"line": line_number, "reason": "INVALID_WORK_PHASE_RECORD"})
                    continue
                # Missing CPU is unknown, including spans crossing workers.
                # CPU may exceed wall slightly due to independent clock resolution.
                work_phases.append({"requestId": fields["requestId"], "phase": fields["phase"],
                                    "role": fields.get("role"), "attempt": attempt,
                                    "durationMs": duration_us / 1000,
                                    "threadCpuMs": cpu_us / 1000 if cpu_us is not None else None,
                                    "line": line_number})
                continue
            if re.search(r"(?:^|\s)NDNSF_DI_NATIVE_PLAN_PHASE ", line):
                try:
                    fields = parse_fields(line.split("NDNSF_DI_NATIVE_PLAN_PHASE ", 1)[1])
                    duration_us = int(fields["duration_us"])
                    if duration_us < 0 or not fields.get("phase") or not fields.get("requestId"):
                        raise ValueError("invalid plan phase")
                except (KeyError, ValueError):
                    gaps.append({"line": line_number, "reason": "INVALID_PLAN_PHASE_RECORD"})
                    continue
                # Producer has no attempt field: retain request scope explicitly.
                plan_phases.append({"requestId": fields["requestId"], "phase": fields["phase"],
                                    "durationMs": duration_us / 1000, "line": line_number,
                                    "attempt": None})
                continue
            if not re.search(r"(?:^|\s)NDNSF_PHASE_TIMING ", line):
                continue
            try:
                fields = parse_fields(line.split("NDNSF_PHASE_TIMING ", 1)[1])
                for name in ("component", "executionRole", "phase", "requestId", "attempt"):
                    if not fields.get(name):
                        raise ValueError("missing identity")
                for name in ("steady_us", "timestamp_us"):
                    if not re.fullmatch(r"[0-9]+", fields[name]) or int(fields[name]) == 0:
                        raise ValueError("invalid timestamp")
                time_us = int(fields["steady_us"])
                if time_us < 0 or not fields.get("phase") or not fields.get("requestId"):
                    raise ValueError("missing identity or negative time")
            except (KeyError, ValueError):
                gaps.append({"line": line_number, "reason": "INVALID_PHASE_RECORD"})
                continue
            key = tuple(fields.get(name, "none") for name in IDENTITY)
            groups[key].append((time_us, fields["phase"], line_number))

    rows = []
    for key, events in groups.items():
        # Preserve log order: sorting can conceal a clock reset or bad pairing.
        pending = defaultdict(deque)
        ambiguous = set()
        spans = []
        previous = None
        for time_us, phase, line_number in events:
            if previous is not None and time_us < previous:
                gaps.append({"line": line_number, "reason": "CLOCK_REGRESSION"})
                # Do not produce spans crossing a clock reset.
                pending.clear()
                ambiguous.clear()
            previous = time_us
            if phase.endswith("Begin"):
                name = phase[:-5]
                if pending[name]:
                    ambiguous.add(name)
                    gaps.append({"line": line_number, "reason": "AMBIGUOUS_PAIR", "phase": name})
                pending[name].append((time_us, line_number))
            elif phase.endswith("End") or phase == "dependencyAcquisitionFailed":
                failed = phase == "dependencyAcquisitionFailed"
                name = "dependencyAcquisition" if failed else (
                    "close" if phase == "drainEnd" else phase[:-3])
                if not pending[name]:
                    gaps.append({"line": line_number, "reason": "UNMATCHED_END", "phase": phase})
                    continue
                begin, begin_line = pending[name].popleft()
                if name not in ambiguous:
                    spans.append({"phase": name, "durationMs": (time_us - begin) / 1000,
                                  "status": "failed" if failed else "complete",
                                  "beginLine": begin_line, "endLine": line_number})
                if not pending[name]:
                    ambiguous.discard(name)
        for name, starts in pending.items():
            for _, line_number in starts:
                gaps.append({"line": line_number, "reason": "UNMATCHED_BEGIN", "phase": name})
        origin = events[0][0]
        rows.append({
            "identity": dict(zip(IDENTITY, key)),
            "originSteadyUs": origin,
            "events": [{"phase": phase, "offsetMs": (time_us - origin) / 1000,
                        "line": line_number} for time_us, phase, line_number in events],
            "spans": spans,
        })
    if not rows:
        gaps.append({"reason": "NO_TIMELINE_RECORDS"})
    return {"log": str(path), "groups": rows, "planPhases": plan_phases,
            "workPhases": work_phases, "gaps": gaps}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        reports = [analyze(path) for path in args.logs]
    except OSError as error:
        parser.error(str(error))
    print(json.dumps({"schema": "ndnsf-invocation-timing-analysis-v1",
                      "scope": "within-log monotonic intervals; warm status not inferred",
                      "reports": reports}, indent=2))


if __name__ == "__main__":
    main()
