#!/usr/bin/env python3
"""Extract per-process measured spans without inferring cross-host clocks."""

from __future__ import annotations

import argparse
from collections import defaultdict, deque
from fractions import Fraction
import json
import math
from pathlib import Path
import random
import re


IDENTITY = (
    "requestId", "attempt", "component", "executionRole", "providerBootId",
    "sessionId", "conversationId",
    "inferenceEpoch", "contextEpoch", "providerName", "scope",
    "planDigest", "plannedName", "operationIndex", "round", "microbatch",
)


def qualify_local_work_pairs(pairs: list[dict], coverage: dict) -> dict:
    """SC002 worst-endpoint paired oracle; coverage is a separate source-review input.

    Each pair supplies baseline/candidate lowerNs and upperNs. This function
    validates scalar bounds and the frozen coverage prerequisites; it cannot
    establish source coverage from runtime measurements alone.
    """
    if not isinstance(coverage, dict) or not isinstance(pairs, list):
        return {"status": "INCONCLUSIVE", "reasons": ["INVALID_LOCAL_WORK_INPUT"]}
    prerequisites = ("sourceCoverageComplete", "exclusiveScopesDisjoint",
                     "ownedWorkBounded", "clocksAndThreadsValid",
                     "observationsComplete", "matchedConditions")
    missing = [name for name in prerequisites if coverage.get(name) is not True]
    digest = coverage.get("sourceCoverageDigest")
    if not isinstance(digest, str) or not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
        missing.append("sourceCoverageDigest")
    if missing:
        return {"status": "INCONCLUSIVE", "reasons": missing}
    if len(pairs) not in (5, 10):
        return {"status": "INCONCLUSIVE", "reasons": ["REQUEST_PAIR_COUNT"]}
    base, candidate = [], []
    for pair in pairs:
        if not isinstance(pair, dict):
            return {"status": "INCONCLUSIVE", "reasons": ["INVALID_LOCAL_WORK_INPUT"]}
        for label in ("baseline", "candidate"):
            bounds = pair.get(label, {})
            if not isinstance(bounds, dict):
                return {"status": "INCONCLUSIVE", "reasons": ["INVALID_LOCAL_WORK_BOUND"]}
            lower, upper = bounds.get("lowerNs"), bounds.get("upperNs")
            if (type(lower) is not int or type(upper) is not int
                    or lower < 0 or upper < lower or upper > 2**64 - 1
                    or (label == "baseline" and lower == 0)):
                return {"status": "INCONCLUSIVE", "reasons": ["INVALID_LOCAL_WORK_BOUND"]}
        base.append(pair["baseline"]["lowerNs"])
        candidate.append(pair["candidate"]["upperNs"])
    rng = random.Random(220)
    ratios = []

    def exact_median(values):
        ordered = sorted(values)
        middle = len(ordered) // 2
        if len(ordered) % 2:
            return Fraction(ordered[middle])
        return Fraction(ordered[middle - 1] + ordered[middle], 2)

    for _ in range(10000):
        indices = [rng.randrange(len(pairs)) for _ in pairs]
        ratios.append(exact_median(candidate[i] for i in indices) /
                      exact_median(base[i] for i in indices))
    ratios.sort()

    def percentile(fraction):
        position = (len(ratios) - 1) * fraction
        index = math.floor(position)
        return ratios[index] + (ratios[min(index + 1, len(ratios) - 1)] - ratios[index]) * (
            position - index)

    interval = [percentile(Fraction(1, 40)), percentile(Fraction(39, 40))]
    passed = interval[1] <= Fraction(17, 20)
    return {"status": "PASS" if passed else "INCONCLUSIVE",
            "reasons": [] if passed else ["WORST_ENDPOINT_UPPER_EXCEEDS_0_85"],
            "worstEndpointMedianRatio": float(exact_median(candidate) / exact_median(base)),
            "pairedPercentile95": [float(value) for value in interval],
            "pairedPercentile95Exact": [str(value) for value in interval],
            "bootstrapReplicates": 10000,
            "seed": 220, "pairs": len(pairs),
            "sourceCoverageDigest": coverage["sourceCoverageDigest"]}


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


def elapsed(start, finish):
    """Missing/invalid observations are unknown, including bool-valued clocks."""
    if (type(start) is int and type(finish) is int and start > 0 and finish >= start):
        return finish - start
    return None


def validate_statistics_record(record, actor):
    """Reject malformed nested records before using their identities as keys."""
    domain = record.get("clockDomain")
    if domain is not None and (not isinstance(domain, str) or not domain):
        raise ValueError("invalid clock domain")
    if actor == "provider":
        for key in ("provider", "providerBootId", "role"):
            if not isinstance(record.get(key), str) or not record[key]:
                raise ValueError("missing provider identity: " + key)
        if type(record.get("attemptEpoch")) is not int or record["attemptEpoch"] < 1:
            raise ValueError("invalid attempt identity")
    metrics = record.get("runtimeMetrics", [])
    if not isinstance(metrics, list):
        raise ValueError("runtimeMetrics must be an array")
    for metric in metrics:
        if not isinstance(metric, dict):
            raise ValueError("runtime metric must be an object")
        if type(metric.get("inferenceEpoch")) is not int or metric["inferenceEpoch"] < 0:
            raise ValueError("invalid metric epoch")
        timing = metric.get("lastBackendExecution")
        if timing is None:
            continue
        if not isinstance(timing, dict):
            raise ValueError("backend receipt must be an object")
        for key in ("requestId", "sessionId", "providerBootId", "role"):
            if not isinstance(timing.get(key), str) or not timing[key]:
                raise ValueError("missing backend identity: " + key)
        for key in ("attemptEpoch", "inferenceEpoch"):
            if type(timing.get(key)) is not int or timing[key] < 0:
                raise ValueError("invalid backend integer identity")
    stats = record.get("workStatistics")
    if stats is None:
        return
    if not isinstance(stats, dict) or not isinstance(stats.get("transfers", []), list):
        raise ValueError("invalid work statistics shape")
    for key in ("phases", "counts"):
        if not isinstance(stats.get(key, {}), dict):
            raise ValueError("invalid statistics inventory: " + key)
    for phase in stats.get("phases", {}).values():
        if not isinstance(phase, dict):
            raise ValueError("invalid phase record")
        for key in ("exclusiveWallNs", "exclusiveThreadCpuNs", "calls"):
            value = phase.get(key)
            if value is not None and (type(value) is not int or value < 0):
                raise ValueError("invalid phase duration or count")
    for transfer in stats.get("transfers", []):
        if (not isinstance(transfer, dict) or
                not isinstance(transfer.get("plannedDataName"), str) or
                not transfer["plannedDataName"].startswith("/") or
                type(transfer.get("inferenceEpoch")) is not int or
                transfer["inferenceEpoch"] < 0):
            raise ValueError("invalid transfer identity")


def analyze_statistics(paths: list[Path], expected_roles=()) -> dict:
    """Analyze structured observations without replacing the native result oracle."""
    requests = defaultdict(list)
    gaps = []
    schemas = {
        "ndnsf-di-native-requester-statistics-v1": "requester",
        "ndnsf-di-native-request-statistics-v1": "provider",
        "ndnsf-di-native-ack-statistics-v1": "ack",
    }
    for path in paths:
        with path.open(encoding="utf-8") as stream:
            for line, text in enumerate(stream, 1):
                if not text.strip():
                    continue
                source = {"file": str(path), "line": line}
                try:
                    record = json.loads(text)
                    if not isinstance(record, dict):
                        raise ValueError("record must be an object")
                    # Ready records carry no request timing and are not failures.
                    if record.get("type") == "serve-ready":
                        continue
                    actor = schemas[record["schema"]]
                    request = record["requestId"]
                    if not isinstance(request, str) or not request:
                        raise ValueError("missing request identity")
                    validate_statistics_record(record, actor)
                except (ValueError, KeyError, TypeError) as error:
                    gaps.append({**source, "reason": "INVALID_STATISTICS_RECORD",
                                 "detail": str(error)})
                    continue
                requests[request].append((actor, record, source))

    rounds = []
    for request, observations in requests.items():
        issues = []
        callers = [r for a, r, _ in observations if a == "requester"]
        caller = callers[0] if len(callers) == 1 else {}
        if len(callers) != 1:
            issues.append("MISSING_OR_AMBIGUOUS_REQUESTER")
        submitted = caller.get("requestSubmittedSteadyNs")
        finished = caller.get("finishedSteadyNs")
        wall = elapsed(submitted, finished)
        preprocessing = elapsed(caller.get("startedSteadyNs"), submitted)
        domain = caller.get("clockDomain")
        backend = {}
        backend_conflicts = set()
        work = []
        transfers = defaultdict(list)
        provider_success = []
        providers = set()
        roles_seen = set()
        for actor, record, source in observations:
            if actor == "provider":
                identity = tuple(record.get(k) for k in
                                 ("provider", "providerBootId", "role", "attemptEpoch"))
                if identity in providers:
                    issues.append("DUPLICATE_PROVIDER_RECORD")
                providers.add(identity)
                if record.get("role") in roles_seen:
                    issues.append("AMBIGUOUS_ROLE_ATTEMPT")
                roles_seen.add(record.get("role"))
                provider_success.append(record.get("completionAdmitted") is True)
                record_receipts = set()
                for metric in record.get("runtimeMetrics", []):
                    timing = metric.get("lastBackendExecution")
                    if not timing:
                        continue
                    key = tuple(timing.get(k) for k in
                                ("requestId", "sessionId", "providerBootId", "role",
                                 "attemptEpoch", "inferenceEpoch"))
                    duration = elapsed(timing.get("startedSteadyNs"),
                                       timing.get("finishedSteadyNs"))
                    if (any(v is None or v == "" for v in key) or key[0] != request or
                            timing.get("role") != record.get("role") or
                            timing.get("providerBootId") != record.get("providerBootId") or
                            timing.get("attemptEpoch") != record.get("attemptEpoch") or
                            timing.get("inferenceEpoch") != metric.get("inferenceEpoch") or
                            timing.get("completed") is not True or duration is None):
                        issues.append("INCOMPLETE_BACKEND_RECEIPT")
                        backend_conflicts.add(key)
                        continue
                    value = {"identity": key, "clockDomain": record.get("clockDomain"),
                             "startedSteadyNs": timing["startedSteadyNs"],
                             "finishedSteadyNs": timing["finishedSteadyNs"],
                             "durationNs": duration}
                    if key in backend and backend[key] != value:
                        backend_conflicts.add(key)
                        issues.append("CONFLICTING_BACKEND_RECEIPT")
                    backend[key] = value
                    record_receipts.add(key)
                if (record.get("hasEpochStatistics") is not True or
                        type(record.get("epochsExecuted")) is not int or
                        record.get("epochsExecuted") != len(record_receipts) or
                        {k[-1] for k in record_receipts} != set(range(len(record_receipts))) or
                        len({k[1] for k in record_receipts}) != 1):
                    issues.append("INCOMPLETE_EPOCH_BACKEND_COVERAGE")
            stats = record.get("workStatistics") or {}
            work.append({"actor": actor, "provider": record.get("provider"),
                         "role": record.get("role"), "source": source,
                         "complete": stats.get("complete", False),
                         "localWorkComplete": stats.get("localWorkComplete", False),
                         "phases": stats.get("phases", {}), "counts": stats.get("counts", {})})
            for transfer in stats.get("transfers", []):
                key = (transfer.get("plannedDataName"), transfer.get("inferenceEpoch"))
                transfers[key].append({**transfer, "clockDomain": record.get("clockDomain"),
                                       "provider": record.get("provider"), "source": source,
                                       "observationComplete": stats.get("complete") is True})
        receipts = [r for k, r in backend.items() if k not in backend_conflicts]
        # A rejected line may hide another role/attempt. Qualify no round from
        # a tainted input set; keep the valid observations for diagnosis only.
        backend_complete = (not gaps and bool(receipts) and not backend_conflicts and
                            bool(expected_roles) and roles_seen == set(expected_roles) and
                            bool(providers) and len(providers) == len(provider_success) and
                            all(provider_success) and caller.get("completed") is True and
                            not any(i in issues for i in
                                    ("DUPLICATE_PROVIDER_RECORD", "AMBIGUOUS_ROLE_ATTEMPT",
                                     "INCOMPLETE_EPOCH_BACKEND_COVERAGE",
                                     "INCOMPLETE_BACKEND_RECEIPT")))
        total_backend = sum(r["durationNs"] for r in receipts) if backend_complete else None
        if not backend_complete:
            issues.append("BACKEND_COVERAGE_NOT_QUALIFIED")
        comparable = (wall is not None and bool(domain) and backend_complete and
                      all(r["clockDomain"] == domain and
                          submitted <= r["startedSteadyNs"] <= r["finishedSteadyNs"] <= finished
                          for r in receipts))
        startup = min(r["startedSteadyNs"] for r in receipts) - submitted if comparable else None
        residual = (wall - total_backend if wall is not None and total_backend is not None and
                    wall >= total_backend else None)
        if not comparable:
            issues.append("STARTUP_CLOCK_OR_COVERAGE_UNKNOWN")
        deliveries = []
        for (name, epoch), values in transfers.items():
            points = {point: [v for v in values if v.get("point") == point] for point in
                      ("output-publication", "coordinator-fetch", "worker-input")}
            row = {"plannedDataName": name, "inferenceEpoch": epoch,
                   "observations": values, "outputReadyToFetchConsumedNs": None,
                   "outputReadyToWorkerAdmittedNs": None, "reasons": []}
            sends = points["output-publication"]
            for point, field, endpoint in (
                    ("coordinator-fetch", "outputReadyToFetchConsumedNs", "finishedSteadyNs"),
                    ("worker-input", "outputReadyToWorkerAdmittedNs", "admittedSteadyNs")):
                receives = points[point]
                if len(sends) != 1 or len(receives) != 1:
                    row["reasons"].append(point + ":MISSING_OR_AMBIGUOUS_ENDPOINT")
                    continue
                send, receive = sends[0], receives[0]
                if (not name or not send["clockDomain"] or
                        send["clockDomain"] != receive["clockDomain"] or
                        not send["observationComplete"] or not receive["observationComplete"]):
                    row["reasons"].append(point + ":CLOCK_OR_COVERAGE_UNKNOWN")
                    continue
                row[field] = elapsed(send.get("outputReadySteadyNs"), receive.get(endpoint))
                if row[field] is None:
                    row["reasons"].append(point + ":TIMESTAMP_UNKNOWN_OR_REVERSED")
            deliveries.append(row)
        rounds.append({
            "requestId": request, "turn": caller.get("turn"),
            "completed": caller.get("completed") is True,
            "requestWallNs": wall, "inputOptionsPreparationNs": preprocessing,
            "backendServiceNs": total_backend,
            "observedBackendServiceNs": sum(r["durationNs"] for r in receipts),
            "backendReceipts": receipts,
            "startupBeforeFirstBackendNs": startup, "wallMinusBackendServiceNs": residual,
            "backendServiceFraction": total_backend / wall
            if total_backend is not None and wall else None,
            "localWorkComplete": False, "workInventories": work,
            "transfers": deliveries, "gaps": sorted(set(issues)),
        })
    return {"scope": "observed service work; no inferred network time or SC verdict",
            "expectedRoles": list(expected_roles),
            "invalidRecordPolicy": "any rejected input row prevents B/S qualification",
            "fetchBoundary": "result consumption upper bound, including scheduling",
            "byteAccounting": "separate observation points; never sum send/fetch/admission",
            "requests": rounds, "gaps": gaps}


def statistics_markdown(statistics: dict) -> str:
    """Human-readable inventory; phase rows are not a request-wall partition."""
    def cell(value):
        return str(value).replace("|", "\\|").replace("\n", " ").replace("\r", " ")

    def ms(value):
        return "UNKNOWN" if value is None else f"{value / 1e6:.3f}"

    rows = ["| Turn | Request | Completed | W ms | S ms | B ms | W minus B ms | Input/options ms |",
            "| --- | --- | --- | --- | --- | --- | --- | --- |"]
    for request in statistics["requests"]:
        rows.append("| " + " | ".join(map(cell, (
            request["turn"], request["requestId"], request["completed"],
            ms(request["requestWallNs"]), ms(request["startupBeforeFirstBackendNs"]),
            ms(request["backendServiceNs"]), ms(request["wallMinusBackendServiceNs"]),
            ms(request["inputOptionsPreparationNs"])))) + " |")
    rows.extend(["", "W uses submission-to-requester-finish. S requires verified clocks and complete "
                 "backend coverage. B is backend service work, potentially overlapping across providers. "
                 "W minus B is not network time. UNKNOWN is not zero.", "",
                 "Phase rows below are exclusive within each thread, not an additive request-wall "
                 "partition. Complete local-work coverage is not asserted.", "",
                 "| Request | Actor/provider | Phase | Kind | Calls | Exclusive wall ms | Thread CPU ms |",
                 "| --- | --- | --- | --- | --- | --- | --- |"])
    for request in statistics["requests"]:
        for inventory in request["workInventories"]:
            for name, phase in sorted(inventory["phases"].items()):
                complete = inventory["complete"]
                rows.append("| " + " | ".join(map(cell, (
                    request["requestId"], inventory["provider"] or inventory["actor"], name,
                    phase.get("kind"), phase.get("calls"),
                    ms(phase.get("exclusiveWallNs") if complete else None),
                    ms(phase.get("exclusiveThreadCpuNs") if complete else None)))) + " |")
    rows.extend(["", "Transfer observations and exact-name latency joins remain in the JSON output. "
                 "Send, fetch and admission bytes must not be summed as distinct traffic."])
    return "\n".join(rows)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="*", type=Path)
    parser.add_argument("--statistics", nargs="+", type=Path,
                        help="requester/provider JSONL and optional ACK sidecar")
    parser.add_argument("--expected-role", action="append", default=[],
                        help="repeat for each role in the frozen deployment; needed for B coverage")
    parser.add_argument("--format", choices=("json", "markdown"), default="json")
    parser.add_argument("--local-work-pairs", type=Path,
                        help="frozen coverage and paired lower/upper ns bounds for SC002")
    args = parser.parse_args()
    try:
        if not args.logs and not args.statistics and not args.local_work_pairs:
            parser.error("provide logs, --statistics JSONL or --local-work-pairs JSON")
        if args.format == "markdown" and (args.logs or not args.statistics):
            parser.error("markdown format requires --statistics without diagnostic logs")
        if args.format == "markdown" and args.local_work_pairs:
            parser.error("local-work qualification requires JSON format")
        reports = [analyze(path) for path in args.logs]
        statistics = analyze_statistics(args.statistics, args.expected_role) if args.statistics else None
        local_work = None
        if args.local_work_pairs:
            with args.local_work_pairs.open(encoding="utf-8") as stream:
                bounds = json.load(stream)
            if not isinstance(bounds, dict):
                parser.error("local-work input must contain coverage and pairs")
            local_work = qualify_local_work_pairs(bounds.get("pairs"), bounds.get("coverage"))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if args.format == "markdown":
        print(statistics_markdown(statistics))
        return
    print(json.dumps({"schema": "ndnsf-invocation-timing-analysis-v1",
                      "scope": "within-log monotonic intervals; warm status not inferred",
                      "reports": reports, "statistics": statistics,
                      "localWorkQualification": local_work}, indent=2))


if __name__ == "__main__":
    main()
