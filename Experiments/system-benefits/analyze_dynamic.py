#!/usr/bin/env python3
"""Qualify complete dynamic cells and account all IPv4 traffic per physical link."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import statistics
import struct


def require(value, reason):
    if not value:
        raise ValueError(reason)


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines()]


def failure_penalized_ms(terminal):
    """Charge unsuccessful tasks their deadline budget, not their early exit time."""
    begin, deadline = terminal["begin_ns"], terminal["deadline_ns"]
    require(deadline > begin, "invalid task deadline budget")
    if terminal["terminal"] == "success":
        end = terminal["terminal_ns"]
        require(begin <= end < deadline, "success outside logical deadline")
    else:
        end = deadline
    return (end - begin) / 1000000


def recovery_cohort(terminals):
    """Use planned tasks 1 and 2, including failures, independently of bootstrap."""
    selected = [row for row in terminals if row["sequence"] in (1, 2)]
    require(len(selected) == 2 and {row["sequence"] for row in selected} == {1, 2},
            "missing/duplicate recovery-sensitive task")
    return {"planned_sequences": [1, 2], "tasks": 2,
            "successes": sum(row["terminal"] == "success" for row in selected),
            "terminal_counts": dict(Counter(row["terminal"] for row in selected)),
            "failure_penalized_mean_ms": statistics.mean(failure_penalized_ms(row) for row in selected)}


def percentile(values, p):
    if not values:
        return None
    ordered = sorted(values)
    i = (len(ordered)-1)*p
    low = int(i)
    return ordered[low]+(ordered[min(low+1, len(ordered)-1)]-ordered[low])*(i-low)


def ip_counts(path, anchor, t0, t1):
    totals = {phase: {"ip_bytes": 0, "packets": 0} for phase in ("before_window", "window", "drain")}
    with path.open("rb") as stream:
        header = stream.read(24)
        variants = {b"\xd4\xc3\xb2\xa1": ("<", 1000), b"\xa1\xb2\xc3\xd4": (">", 1000),
                    b"\x4d\x3c\xb2\xa1": ("<", 1), b"\xa1\xb2\x3c\x4d": (">", 1)}
        require(len(header) == 24 and header[:4] in variants, "invalid pcap header")
        endian, fraction = variants[header[:4]]
        require(struct.unpack(endian+"I", header[20:24])[0] == 1, "expected Ethernet capture")
        while True:
            h = stream.read(16)
            if not h:
                break
            require(len(h) == 16, "truncated pcap record")
            sec, sub, captured, wire = struct.unpack(endian+"IIII", h)
            frame = stream.read(captured)
            require(len(frame) == captured == wire, "truncated frame")
            require(len(frame) >= 34 and frame[12:14] == b"\x08\x00", "capture is not IPv4 Ethernet")
            ip = frame[14:]
            size, total = (ip[0]&15)*4, int.from_bytes(ip[2:4], "big")
            require(ip[0]>>4 == 4 and 20 <= size <= total <= len(ip), "invalid IPv4 extent")
            mono = sec*1000000000+sub*fraction-anchor["wall_ns"]+anchor["mono_ns"]
            phase = "before_window" if mono < t0 else "window" if mono < t1 else "drain"
            totals[phase]["ip_bytes"] += total
            totals[phase]["packets"] += 1
    return totals


def verify_record_receipt(directory, manifest, run):
    """Bind the native result to its stopped archive; no protocol simulation."""
    label = run["id"]
    receipt = run["record_oracle"]
    expected_files = {"task-plan.json"} | {label + suffix for suffix in
                     ("-user.jsonl", "-drone1.jsonl", "-drone2.jsonl")}
    if manifest.get("schema") == "ndnsf-dynamic205-e3-v1":
        reference = manifest["availability_trace"]
        require(reference["profile"] in ("stable", "slow", "fast")
                and len(reference["files"]) == 2, "availability trace references")
        for index, entry in enumerate(reference["files"]):
            name = f"availability-{index}.json"
            require(entry["file"] == name and entry["producer"] ==
                    f"/example/hello/provider/{'AB'[index]}", "availability trace identity")
            require(receipt["inputs"].get(name) == entry["sha256"], "availability trace receipt hash")
            expected_files.add(name)
    require(set(receipt["inputs"]) == expected_files, "incomplete native oracle inputs")
    for name, expected in receipt["inputs"].items():
        require(hashlib.sha256((directory/name).read_bytes()).hexdigest() == expected,
                "native oracle input changed: " + name)
    require(receipt["exit"] == 0 and receipt["output"] == label + "-record-oracle.log",
            "native oracle execution failed")
    output = (directory/receipt["output"]).read_bytes()
    require(hashlib.sha256(output).hexdigest() == receipt["output_sha256"],
            "native oracle output changed")
    checked = json.loads(output)
    require(checked == receipt["result"] and checked["qualified"] is True
            and checked["event"] == "dynamic_record_oracle_complete"
            and checked["run_id"] == label, "invalid native oracle result")
    require(receipt["binary_sha256"] == manifest["binaries"]["/usr/local/bin/ndnsf-dynamic-oracle"],
            "native oracle binary identity")
    expected_context = {
        "run": {key: value for key, value in run.items() if key != "record_oracle"},
        "manifest": {key: manifest[key] for key in
                     ("schema", "window_ms", "period_ms", "dwell_ms",
                      "force_unavailable", "task_plan")}}
    if manifest.get("schema") in ("ndnsf-dynamic205-e2-v1", "ndnsf-dynamic205-e3-v1"):
        expected_context["manifest"]["recovery_condition"] = manifest["recovery_condition"]
        expected_context["manifest"]["clock_anchor"] = manifest["clock_anchor"]
    if manifest.get("schema") == "ndnsf-dynamic205-e3-v1":
        expected_context["manifest"]["availability_trace"] = manifest["availability_trace"]
    require(receipt["context"] == expected_context, "native oracle context changed")
    plan = json.loads((directory/"task-plan.json").read_text())
    require(checked["tasks"] == len(plan["tasks"]), "native oracle planned denominator")
    return plan


def analyze(directory):
    manifest = json.loads((directory/"manifest.json").read_text())
    fixed_arrivals = manifest.get("schema") in (
        "ndnsf-dynamic205-v2", "ndnsf-dynamic205-e2-v1", "ndnsf-dynamic205-e3-v1")
    if fixed_arrivals:
        require(manifest.get("record_oracle", {}).get("qualified"),
                "task-dependent native record oracle has not qualified this cell")
    require(manifest["status"] == "RUN_COMPLETE" and not manifest.get("cleanup_errors"), "incomplete cell/cleanup")
    for child in manifest["children"]:
        # Maintained Controller app terminates by SIGINT; other actors exit normally.
        require(child["exit"] in ((0,-2) if child["label"] == "controller" else (0,)), "child exit: "+child["label"])
    results = []
    for run in manifest["runs"]:
        label = run["id"]
        if fixed_arrivals:
            plan = verify_record_receipt(directory, manifest, run)
        data = rows(directory/(label+"-user.jsonl"))
        windows = [r for r in data if r["event"] == "window"]
        closed = [r for r in data if r["event"] == "cohort_closed"]
        shutdown = [r for r in data if r["event"] == "shutdown"]
        require(len(windows) == len(closed) == len(shutdown) == 1, "window/terminal markers")
        window, final = windows[0], shutdown[0]
        t0, t1 = window["t0_ns"], window["t1_ns"]
        require(window["period_ms"]==manifest["period_ms"],"pacing profile")
        require(t0 == run["origin_ns"] and t1-t0 == manifest["window_ms"]*1000000, "window profile")
        require(final["started"] and final["invalid"] == 0, "native provenance oracle")
        terminals = [r for r in data if r["event"] == "terminal"]
        require(terminals, "empty measured cohort")
        submitted = [r for r in data if r["event"] == "attempt_submitted"]
        require(len(terminals) == final["tasks"] == final["terminals"] == closed[0]["tasks"], "task cardinality")
        require(set(r["sequence"] for r in terminals) == set(range(1, len(terminals)+1)), "duplicate/missing task")
        ordered=sorted(terminals,key=lambda r:r["sequence"])
        if fixed_arrivals:
            require(len(ordered) == len(plan["tasks"]), "missing planned task outcome")
            for terminal, planned in zip(ordered, plan["tasks"]):
                require(terminal["sequence"] == planned["task"]
                        and terminal["begin_ns"] == t0 + planned["arrival_ms"] * 1000000,
                        "fixed arrival changed")
        else:
            for previous,current in zip(ordered,ordered[1:]):
                require(current["begin_ns"]>=previous["terminal_ns"],"overlapping logical tasks")
                require(current["begin_ns"]-previous["begin_ns"]>=manifest["period_ms"]*1000000,"pacing interval")
        require(len(set((r["sequence"],r["attempt"]) for r in submitted)) == len(submitted), "duplicate submission")
        for terminal in terminals:
            require(t0 <= terminal["begin_ns"] < t1 and terminal["deadline_ns"]-terminal["begin_ns"] == 2000000000,
                    "logical deadline/cohort")
            require(0 <= terminal["attempts"] <= 3, "attempt cap")
            require(terminal["terminal_ns"] >= terminal["begin_ns"], "negative duration")
            require(sum(r["sequence"] == terminal["sequence"] for r in submitted) == terminal["attempts"], "submission count")
            require(terminal["terminal"] != "success" or terminal["terminal_ns"] < terminal["deadline_ns"], "late success")
        success = [r for r in terminals if r["terminal"] == "success"]
        responses = [r for r in data if r["event"] == "attempt_response"]
        submissions={(r["sequence"],r["attempt"]):r for r in submitted}
        for submission in submitted:
            require(0 < submission["deadline_ns"]-submission["begin_ns"] <= 600000000, "attempt deadline cap")
        for response in responses:
            require(not response["status"] or response["identity_verified"] and response["echo_verified"], "response oracle")
        provider_rows=[]
        for name in ("drone1", "drone2"):
            own=rows(directory/(label+"-"+name+".jsonl"))
            require(sum(r["event"]=="ready" for r in own)==1
                    and own[-1]["event"]=="shutdown" and own[-1]["ready"], "Provider lifecycle")
            provider_rows.extend(own)
        executions = [r for r in provider_rows if r["event"] == "execution"
                      and (r["task"]["phase"] == "measure" if fixed_arrivals
                           else not r["command"].startswith("w"*64))]
        require(all(r["payload_valid"] for r in executions), "provider payload oracle")
        for execution in executions:
            index=0 if execution["provider"]=="/example/hello/provider/A" else 1
            require(execution["provider"] in ("/example/hello/provider/A", "/example/hello/provider/B"), "unknown executor")
            if manifest.get("schema") != "ndnsf-dynamic205-e3-v1":
                dwell=manifest["dwell_ms"]
                expected=not manifest["force_unavailable"] and (not dwell or ((execution["begin_ns"]-t0)//(dwell*1000000))%2==index)
                require(execution["eligible"] == expected, "independent admission trace oracle")
            # E3 admission is checked by the native trace replay whose input/output
            # hashes verify_record_receipt bound above; do not apply legacy dwell0.
        success_commands = {r["command"] for r in success}
        if manifest["purpose"]=="negative":
            require(not success,"negative workload unexpectedly succeeded")
        eligible = [r for r in executions if r["eligible"]]
        if manifest["purpose"]=="negative" and manifest["handler_ms"]==800:
            submitted_ids={r["request_id"] for r in submitted}
            require(any(r["request_id"] in submitted_ids and r["end_ns"]-r["begin_ns"]>=800000000
                        for r in eligible),"slow-handler negative did not execute measured native work")
        if manifest["force_unavailable"]:
            observed_false=any(r["event"]=="ack_handler" and r["mono_ns"]>=t0 and not r["state"]["available"]
                               for r in provider_rows)
            observed_false=observed_false or any(r["event"]=="snapshot_accepted" and r["mono_ns"]>=t0
                                                 and not r["snapshot"]["available"] for r in data)
            require(observed_false and not eligible,"unavailable negative did not reach rejection branch")
        require(success_commands <= {r["command"] for r in eligible}, "success without actual eligible execution")
        selections={(r["sequence"],r["attempt"]):r for r in data if r["event"] in ("ack_selection","advertisement_selection")}
        for terminal in success:
            key=(terminal["sequence"],terminal["success_attempt"])
            require(key in submissions and key in selections, "missing accepted attempt/selection")
            submitted_attempt=submissions[key]
            selected=selections[key]
            provider=selected["selected"] if run["arm"]=="SOLICIT" else selected["snapshot"]["producer"]
            require(provider == terminal["success_provider"] and terminal["success_request_id"] == submitted_attempt["request_id"],
                    "success executor/request binding")
            matching=[r for r in responses if (r["sequence"],r["attempt"])==key and r["status"]
                      and not r["late"] and r["provider"]==provider and r["request_id"]==terminal["success_request_id"]]
            require(matching and terminal["success_response_ns"] < submitted_attempt["deadline_ns"], "success after attempt deadline")
            require(any(r["command"]==terminal["command"] and r["provider"]==provider
                        and r["request_id"]==terminal["success_request_id"]
                        and r["end_ns"] <= terminal["success_response_ns"] for r in eligible),
                    "accepted response without matching completed native execution")
        if run["arm"] != "SOLICIT":
            require(any(r["event"] == "snapshot_accepted" and r["source"] == "SVS-advertisement" for r in data),
                    "actual advertisement delivery not demonstrated")
        wire = {p: {"ip_bytes": 0, "packets": 0} for p in ("before_window", "window", "drain")}
        for peer in ("gs", "drone1", "drone2"):
            capture = manifest["capture"][label+"-"+peer]
            require(capture["exit"] == capture["drops"] == 0, "capture exit/drop")
            require(capture["started_ns"]+capture["guard_ns"] < t0 and capture["stopped_ns"] > t1+2000000000,
                    "capture coverage")
            counts = ip_counts(directory/capture["pcap"], manifest["clock_anchor"], t0, t1)
            for phase in wire:
                for metric in wire[phase]:
                    wire[phase][metric] += counts[phase][metric]
        latency = [(r["terminal_ns"]-r["begin_ns"])/1000000 for r in success]
        all_latency = [(r["terminal_ns"]-r["begin_ns"])/1000000 for r in terminals]
        bounded_latency = [(min(r["terminal_ns"], r["deadline_ns"])-r["begin_ns"])/1000000
                           for r in terminals]
        dwell=manifest["dwell_ms"]
        phase_counts=Counter(((r["begin_ns"]-t0)//(dwell*1000000))%2 if dwell else "stable" for r in terminals)
        selection_ages=[]
        for key,selected in selections.items():
            if run["arm"]!="SOLICIT" and key in submissions:
                selection_ages.append((submissions[key]["begin_ns"]-selected["snapshot"]["published_ns"])/1000000)
        executed_counts = Counter(r["command"] for r in eligible)
        results.append({"arm": run["arm"], "seed": manifest["seed"], "dwell_ms": manifest["dwell_ms"],
            "period_ms":manifest["period_ms"],
            "tasks": len(terminals), "successes": len(success), "success_fraction": len(success)/len(terminals) if terminals else None,
            "terminal_counts": dict(Counter(r["terminal"] for r in terminals)),
            "conditional_latency_p50_ms": percentile(latency,.5), "conditional_latency_p95_ms": percentile(latency,.95),
            "conditional_latency_mean_ms": statistics.mean(latency) if latency else None,
            "all_task_terminal_mean_ms": statistics.mean(all_latency),
            "all_task_deadline_bounded_mean_ms": statistics.mean(bounded_latency),
            "all_task_failure_penalized_mean_ms":
                statistics.mean(failure_penalized_ms(r) for r in terminals),
            "throughput_per_s": len(success)/(manifest["window_ms"]/1000), "wire": wire,
            "window_ip_bytes_per_success": wire["window"]["ip_bytes"]/len(success) if success else None,
            "window_and_drain_ip_bytes_per_success":
                (wire["window"]["ip_bytes"] + wire["drain"]["ip_bytes"])/len(success) if success else None,
            "late_response_count": sum(r["late"] for r in responses),
            "eligible_executions": len(eligible), "ineligible_admissions": len(executions)-len(eligible),
            "duplicate_executions": sum(max(0,v-1) for v in executed_counts.values()),
            "eligible_execution_without_success": sum(r["command"] not in success_commands for r in eligible),
            "refresh_interest_count": sum(r["event"] == "refresh_interest" for r in data)})
        results[-1]["admission_phase_counts"]={str(k):v for k,v in phase_counts.items()}
        if manifest.get("schema") == "ndnsf-dynamic205-e3-v1":
            results[-1]["admission_phase_counts"] = None  # No periodic phase exists for E3.
        results[-1]["selected_snapshot_age_p50_ms"]=percentile(selection_ages,.5)
        if manifest.get("schema") in ("ndnsf-dynamic205-e2-v1", "ndnsf-dynamic205-e3-v1"):
            results[-1]["recovery_condition"] = manifest["recovery_condition"]
            results[-1]["recovery_sensitive"] = recovery_cohort(terminals)
        if manifest.get("schema") == "ndnsf-dynamic205-e3-v1":
            results[-1]["availability_profile"] = manifest["availability_trace"]["profile"]
    return {"qualified": True, "schema": manifest.get("schema"),
        "arrival_policy": manifest.get("arrival_policy", "historical closed-loop pacing"),
        "scope": manifest["scope"], "candidate": {
        k: manifest[k] for k in ("source_sha256", "binaries", "libraries", "configuration", "contract_sha256")},
        "window_ms": manifest["window_ms"], "results": results}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.directory), indent=2, sort_keys=True))
