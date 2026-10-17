#!/usr/bin/env python3
"""Recompute Spec206 outcomes from actor records and relay packet captures."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_jsonl(path):
    return [json.loads(line) for line in Path(path).read_text().splitlines()]


def task_input(row):
    code = "M" if row["class"] == "memory" else "C"
    return f"T205|M|{code}|{row['task']:010d}|private-input".ljust(64, "_")


def capture_bytes(directory, manifest, run, window_ns):
    name = run["id"] + "-relay-any"
    record = manifest["capture"][name]
    require(record["interface"] == "any" and record["exit"] == 0 and
            record["drops"] == 0 and record["started_ns"] < run["origin_ns"] and
            record["stopped_ns"] >= run["origin_ns"] + window_ns,
            "incomplete relay capture")
    path = directory / record["pcap"]
    require(path.name == name + ".pcap" and path.stat().st_size > 24,
            "wrong or empty relay capture")
    result = subprocess.run(
        ["tshark", "-r", str(path), "-Y", "ip", "-T", "fields",
         "-e", "frame.time_epoch", "-e", "ip.len",
         "-E", "separator=,", "-E", "occurrence=f"],
        capture_output=True, text=True, check=True, timeout=60)
    anchor = manifest["clock_anchor"]
    first = run["origin_ns"]
    last = first + window_ns
    packets = 0
    total = 0
    for line in result.stdout.splitlines():
        fields = line.split(",")
        require(len(fields) == 2 and fields[0] and fields[1],
                "unreadable IP packet in relay capture")
        wall_ns = round(float(fields[0]) * 1e9)
        monotonic_ns = anchor["mono_ns"] + wall_ns - anchor["wall_ns"]
        if first <= monotonic_ns < last:
            ip_bytes = int(fields[1])
            require(20 <= ip_bytes <= 65535, "invalid IP packet length")
            packets += 1
            total += ip_bytes
    require(packets > 0, "no IP packets during measurement")
    return {"ip_bytes": total, "ip_packets": packets, "pcap_sha256": digest(path),
            "scope": "all relay interface IP transmissions in planned window"}


def grpc_outcomes(directory, manifest, run, tasks, window_ms):
    log = (directory / run["user_log"]).read_text().splitlines()
    published = {}
    terminals = {}
    attempts = 0
    for line in log:
        if line.startswith("GRPC_REQUEST_PUBLISHED "):
            match = re.fullmatch(
                r"GRPC_REQUEST_PUBLISHED request_id=(\d+) "
                r"monotonic_s=([\d.]+) planned_monotonic_s=([\d.]+)", line)
            require(match is not None, "malformed gRPC publication")
            task = int(match.group(1))
            require(task not in published, "duplicate gRPC publication")
            published[task] = (float(match.group(2)), float(match.group(3)))
        elif line.startswith("GRPC_REQUEST_TERMINAL_JSON "):
            row = json.loads(line[len("GRPC_REQUEST_TERMINAL_JSON "):])
            task = row["task"]
            require(task not in terminals, "duplicate gRPC terminal")
            terminals[task] = row
        elif line.startswith("GRPC_FAILOVER_ATTEMPT "):
            attempts += 1
    expected = {row["task"] for row in tasks}
    require(set(published) == expected and set(terminals) == expected,
            "incomplete gRPC task cohort")
    services = {}
    health_checks = 0
    origin_s = run["origin_ns"] / 1e9
    task_busy = manifest["condition"] == "task-busy"
    for node in ("drone1", "drone2"):
        trace = None
        if task_busy:
            index = 0 if node == "drone1" else 1
            reference = manifest["availability_trace"]["files"][index]
            trace_path = directory / reference["file"]
            require(digest(trace_path) == reference["sha256"],
                    "gRPC task-busy trace digest")
            trace = json.loads(trace_path.read_text())

        def constrained(at_s):
            if trace is None or at_s < origin_s:
                return False
            elapsed_ms = (at_s - origin_s) * 1000
            state = True
            for change in trace["changes"]:
                if change["offset_ms"] > elapsed_ms:
                    break
                state = change["available"]
            return not state

        path = directory / (run["id"] + "-" + node + ".jsonl")
        for row in read_jsonl(path):
            require(row["provider"] == node, "gRPC provider record identity")
            if row["event"] == "health":
                if task_busy:
                    require(row["status"] == "SERVING",
                            "task-specific pressure leaked into generic health")
                if origin_s <= row["started_monotonic_s"] < origin_s + window_ms / 1000.0:
                    health_checks += 1
                continue
            require(row["event"] == "service", "unknown gRPC provider event")
            task = int(row["request_id"])
            require(task in expected and row["request_name"] == task_input(tasks[task - 1])
                    and row["status"] in ("OK", "UNAVAILABLE", "RESOURCE_EXHAUSTED",
                                          "DEADLINE_EXCEEDED", "INVALID_ARGUMENT")
                    and row["completed_monotonic_s"] >= row["started_monotonic_s"],
                    "unbound gRPC service execution")
            if task_busy and row["status"] in ("OK", "RESOURCE_EXHAUSTED"):
                specialty = "compute" if node == "drone1" else "memory"
                require(type(row.get("admission_monotonic_ns")) is int,
                        "gRPC task admission timestamp missing")
                should_reject = (constrained(row["admission_monotonic_ns"] / 1e9) and
                                 tasks[task - 1]["class"] == specialty)
                require((row["status"] == "RESOURCE_EXHAUSTED") == should_reject,
                        "gRPC task-specific admission contradicts trace")
            services.setdefault(task, []).append(row)
    outcomes = []
    for task in tasks:
        task_id = task["task"]
        planned_s = origin_s + task["arrival_ms"] / 1000.0
        published_s, logged_planned_s = published[task_id]
        terminal = terminals[task_id]
        require(abs(logged_planned_s - planned_s) <= 0.001 and
                published_s >= planned_s - 0.001 and
                terminal["task"] == task_id and
                terminal["status"] in ("OK", "GLOBAL_DEADLINE_EXCEEDED",
                                        "UNAVAILABLE", "DEADLINE_EXCEEDED",
                                        "RESOURCE_EXHAUSTED", "BAD_ECHO",
                                        "WRONG_PROVIDER", "WRONG_REQUEST",
                                        "LATE_REPLY", "NO_RESPONSE"),
                "gRPC planned arrival/terminal mismatch")
        events = services.get(task_id, [])
        completed = [row for row in events if row["status"] == "OK"]
        if terminal["success"]:
            matching = [row for row in completed
                        if row["provider"] == terminal["provider"] and
                        row["completed_monotonic_s"] <=
                        planned_s + terminal["latency_ms_from_planned"] / 1000.0 + 0.001]
            require(terminal["status"] == "OK" and matching and
                    terminal["response"] == "Hello, " + task_input(task) and
                    terminal["latency_ms_from_planned"] < 2000.0,
                    "gRPC client success lacks bound handler")
        else:
            require(terminal["status"] != "OK", "gRPC false success state")
        late = sum(row["completed_monotonic_s"] >= planned_s + 2.0
                   for row in completed)
        correct = terminal["success"] and len(completed) == 1 and late == 0
        outcomes.append({"task": task_id, "success": correct,
                         "client_success": terminal["success"],
                         "latency_ms": terminal["latency_ms_from_planned"],
                         "provider": terminal["provider"],
                         "executions": len(completed),
                         "duplicate_executions": max(0, len(completed) - 1),
                         "late_executions": late})
    require(len(tasks) <= attempts <= len(tasks) * 3,
            "gRPC attempt budget mismatch")
    return outcomes, health_checks, attempts


def analyze(directory, oracle):
    directory = Path(directory)
    manifest_path = directory / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    coarse = manifest["schema"] == "ndnsf-spec207-coarse-v1"
    feedback = manifest["schema"] == "ndnsf-feedback-ablation-v1"
    require(manifest["schema"] in ("ndnsf-spec206-comparison-v1", "ndnsf-spec207-coarse-v1",
                                   "ndnsf-feedback-ablation-v1") and
            manifest["status"] == ("RUN_COMPLETE" if feedback else "ACTORS_COMPLETE_UNQUALIFIED") and
            (manifest["condition"] in ("task-busy", "offline") if coarse else
             manifest["condition"] in ("offline", "busy", "stable", "task-busy")),
            "incomplete comparison archive")
    plan_path = directory / "task-plan.json"
    require(digest(plan_path) == manifest["task_plan"]["sha256"],
            "task plan hash mismatch")
    plan = json.loads(plan_path.read_text())
    tasks = plan["tasks"]
    require(plan["window_ms"] == manifest["window_ms"] and
            plan["period_ms"] == manifest["period_ms"] and
            len(tasks) == (plan["window_ms"] + plan["period_ms"] - 1) // plan["period_ms"],
            "task plan mismatch")
    for index, row in enumerate(tasks):
        require(set(row) == {"task", "arrival_ms", "class"} and
                row["task"] == index + 1 and
                row["arrival_ms"] == index * plan["period_ms"] and
                row["class"] in ("compute", "memory"),
                "task plan row mismatch")
    runs = []
    for run in manifest["runs"]:
        if run["arm"] == "GRPC":
            outcomes, health_checks, attempts = grpc_outcomes(
                directory, manifest, run, tasks, plan["window_ms"])
            native = None
        else:
            require(digest(oracle) == manifest["binaries"][str(oracle)],
                    "native oracle differs from recorded binary")
            result = subprocess.run(
                [str(oracle), "--manifest", str(manifest_path), "--run-id", run["id"]],
                capture_output=True, text=True, timeout=30)
            require(result.returncode == 0,
                    "native oracle rejected " + run["id"] + ": " + result.stderr.strip())
            native = json.loads(result.stdout)
            require(native["qualified"] is True and native["tasks"] == len(tasks),
                    "native oracle incomplete")
            incorrect = set(native["incorrect_tasks"])
            require(all(1 <= task <= len(tasks) for task in incorrect),
                    "native incorrect task identity")
            records = read_jsonl(directory / (run["id"] + "-user.jsonl"))
            outcomes = []
            for row in records:
                if row.get("event") == "terminal":
                    task = (row["sequence"] if run["arm"] in ("SOLICIT", "FEEDBACK", "ACK_ALL") else
                            int(row["command"][9:19]))
                    client_success = row["terminal"] == "success"
                    outcomes.append({"task": task,
                                     "success": client_success and task not in incorrect,
                                     "client_success": client_success,
                                     "latency_ms": (row["terminal_ns"] -
                                                    run["origin_ns"] -
                                                    tasks[task - 1]["arrival_ms"] * 1000000) / 1e6,
                                     "provider": row.get("success_provider", row.get("target"))})
            require(len(outcomes) == len(tasks), "native outcome count mismatch")
            require(sum(row["success"] for row in outcomes) == native["successes"] and
                    sum(row["client_success"] for row in outcomes) == native["client_successes"],
                    "native corrected outcome mismatch")
            health_checks = None
            if run["arm"] in ("SOLICIT", "ADS_COARSE", "FEEDBACK", "ACK_ALL"):
                attempts = sum(row.get("event") == "attempt_submitted" for row in records)
                terminal_attempts = [row["attempts"] for row in records
                                     if row.get("event") == "terminal"]
                require(len(terminal_attempts) == len(tasks) and
                        all(type(count) is int and 0 <= count <= 3
                            for count in terminal_attempts),
                        "native per-task attempt budget mismatch")
                require(attempts == sum(terminal_attempts),
                        "native terminal/submission attempt count mismatch")
            else:
                lines = (directory / run["user_log"]).read_text().splitlines()
                counted = [re.fullmatch(
                    r"NSC_REQUEST_RESULT request_id=(\d+) status=(?:SUCCESS|FAILURE) "
                    r"latency_ms=\d+ attempts=(\d+) provider=\S+ "
                    r"published_monotonic_ms=[\d.]+", line)
                    for line in lines if line.startswith("NSC_REQUEST_RESULT ")]
                require(len(counted) == len(tasks) and all(counted),
                        "NSC attempt accounting incomplete")
                attempts = sum(int(row.group(2)) for row in counted)
                require(len(tasks) <= attempts <= len(tasks) * 3,
                        "native attempt budget mismatch")
        diagnostics = None
        if coarse:
            selected = [row for row in records
                        if row.get("event") == "advertisement_selection"]
            ages = [(row["mono_ns"] - row["snapshot"]["published_ns"]) / 1e6
                    for row in selected]
            require(all(age >= 0 for age in ages), "future selected snapshot")
            diagnostics = {
                "selected_advertisement_age_ms": ages,
                "selected_long_advertisements": sum(
                    row["snapshot"]["expires_ns"] - row["snapshot"]["published_ns"]
                    == 120000000000 for row in selected),
                "selected_short_statuses": sum(
                    row["snapshot"]["expires_ns"] - row["snapshot"]["published_ns"]
                    == 2500000000 for row in selected),
                "failed_invocations": attempts - sum(row["success"] for row in outcomes),
                "status_queries": sum(row.get("event") == "refresh_interest" for row in records),
                "accepted_initial_ads": sum(
                    row.get("event") == "snapshot_accepted"
                    and row.get("source") == "SVS-advertisement"
                    and row["mono_ns"] < run["origin_ns"] for row in records),
            }
        if feedback:
            starts = {(row["sequence"], row["attempt"]): row["begin_ns"] for row in records
                      if row.get("event") == "attempt_submitted"}
            selections = [row for row in records if row.get("event") == "ack_selection"]
            require(all((row["sequence"], row["attempt"]) in starts for row in selections),
                    "unbound selection timing")
            diagnostics = {
                "coordination_ms": [(row["mono_ns"] - starts[(row["sequence"], row["attempt"])]) / 1e6
                                    for row in selections],
                "failed_attempts": attempts - sum(row["success"] for row in outcomes),
                "ineligible_executions": sum(
                    row.get("event") == "execution" and row.get("eligible") is False
                    and row.get("begin_ns", 0) >= run["origin_ns"]
                    for node in ("drone1", "drone2")
                    for row in read_jsonl(directory / (run["id"] + "-" + node + ".jsonl"))),
            }
        traffic = capture_bytes(directory, manifest, run, plan["window_ms"] * 1000000)
        runs.append({"arm": run["arm"], "condition": manifest["condition"],
                     "run_id": run["id"], "tasks": len(tasks),
                     "successes": sum(row["success"] for row in outcomes),
                     "client_successes": sum(row.get("client_success", row["success"])
                                             for row in outcomes),
                     "outcomes": outcomes, "health_checks": health_checks,
                     "duplicate_executions": native["duplicate_executions"] if native else
                                             sum(row["duplicate_executions"] for row in outcomes),
                     "late_executions": native["late_executions"] if native else
                                        sum(row["late_executions"] for row in outcomes),
                     "attempts": attempts,
                     "diagnostics": diagnostics,
                     "native_receipt": native, "traffic": traffic})
    require({run["arm"] for run in runs} ==
            ({"FEEDBACK", "ACK_ALL"} if feedback else
             {"SOLICIT", "ADS_COARSE"} if coarse else {"SOLICIT", "NSC", "GRPC"}),
            "wrong comparison arms")
    return {"schema": "ndnsf-feedback-reanalysis-v1" if feedback else
            "ndnsf-spec207-reanalysis-v1" if coarse else "ndnsf-spec206-reanalysis-v1",
            "scope": "record/capture consistency, not standalone signature re-verification",
            "purpose": "qualification" if plan["window_ms"] == 60000 else "wiring",
            "manifest_sha256": digest(manifest_path), "plan_sha256": digest(plan_path),
            "analyzer_sha256": digest(__file__), "runs": runs}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive")
    parser.add_argument("--oracle", default="/usr/local/bin/ndnsf-dynamic-oracle")
    args = parser.parse_args()
    print(json.dumps(analyze(args.archive, Path(args.oracle)), sort_keys=True))
