#!/usr/bin/env python3
"""Requalify every raw cell; aggregate matched blocks without dropping failures."""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import statistics
from analyze_credentials import estimate
from analyze_dynamic import analyze, require
from dynamic_campaign import E3_CONDITIONS, e3_formal_cells

ARMS=("SOLICIT","ADS100","ADS1000","ADS_EVENT")
PRIMARY = ("all_task_failure_penalized_mean_ms", "window_and_drain_ip_bytes_per_success")
# Fixed E1 design: 16 paired runs, six two-sided Bonferroni contrasts, df=15.
T15_BONFERRONI6 = 3.0362832225931182
T15_BONFERRONI30 = 3.8219733981616257


def descriptive(values):
    missing = sum(value is None for value in values)
    require(all(value is None or math.isfinite(value) for value in values), "nonfinite metric")
    return {"n": len(values), "missing": missing,
            "mean": statistics.mean(values) if values and not missing else None}


def paired_interval(values, formal, critical=T15_BONFERRONI6):
    result = descriptive(values)
    result.update(ci95_half_width=None, ci95=None)
    if formal:
        require(len(values) == 16, "formal comparison requires sixteen paired runs")
        if not result["missing"]:
            half = critical * statistics.stdev(values) / math.sqrt(16)
            result.update(ci95_half_width=half,
                          ci95=[result["mean"] - half, result["mean"] + half])
    return result


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def summarize_v2(directory, manifest):
    recovery = manifest["schema"] == "ndnsf-dynamic-campaign205-e2-v1"
    irregular = manifest["schema"] == "ndnsf-dynamic-campaign205-e3-v1"
    require(manifest["status"] == "CAMPAIGN_COMPLETE" and not manifest.get("first_failure"),
            "incomplete campaign")
    require(digest(directory / "matrix.json") == manifest["matrix_sha256"], "matrix hash")
    require(manifest["design_snapshot"] == "design.md" and
            digest(directory / "design.md") == manifest["design_sha256"], "design snapshot hash")
    matrix = json.loads((directory / "matrix.json").read_text())
    require(matrix["version"] == (3 if irregular else 2 if recovery else 1) and matrix["purpose"] == manifest["purpose"] and
            matrix["purpose"] in ("pilot", "formal"), "matrix purpose/version")
    expected = matrix["cells"]
    require(expected == manifest["matrix"] and len(expected) == len(manifest["cells"]),
            "frozen matrix coverage")
    fields = {"period_ms", "dwell_ms", "seed", "window_ms"}
    require(expected and all(set(c) == fields | ({"recovery_condition"} if recovery or irregular else set())
                            | ({"availability_profile"} if irregular else set())
                            and all(type(c[k]) is int for k in fields)
                            and c["period_ms"] in ((100, 4700) if irregular else (4700,)) and c["dwell_ms"] == 0
                            and c["window_ms"] == 60000 for c in expected), "stable E1 matrix")
    seeds = [c["seed"] for c in expected]
    formal = matrix["purpose"] == "formal"
    if irregular:
        require(not formal or expected == e3_formal_cells(), "frozen E3 formal matrix/order mismatch")
        identities = [(c["seed"], c["availability_profile"], c["period_ms"]) for c in expected]
        require(len(identities) == len(set(identities)) and
                set(identities) == {(seed, profile, period) for seed in seeds
                                    for profile, period in E3_CONDITIONS} and
                all(c["recovery_condition"] == "prepared" for c in expected),
                "incomplete/duplicate E3 condition block")
    if recovery:
        require(len(expected) % 2 == 0, "incomplete recovery pairs")
        pair_seeds = seeds[::2]
        require(len(pair_seeds) == len(set(pair_seeds)), "duplicate recovery pair")
        for i in range(len(pair_seeds)):
            pair = expected[2*i:2*i+2]
            order = ["prepared", "reregister"]
            if (i + i//4) % 2:
                order.reverse()
            require(pair[0]["seed"] == pair[1]["seed"]
                    and {c["recovery_condition"] for c in pair} == {"prepared", "reregister"}
                    and (not formal or [c["recovery_condition"] for c in pair] == order),
                    "recovery pair/order mismatch")
        if formal:
            require(pair_seeds == list(range(20560, 20576)), "frozen E2 formal seeds/order")
    elif not irregular:
        require(len(seeds) == len(set(seeds)), "duplicate paired block")
    if formal and not recovery and not irregular:
        require(seeds == list(range(20521, 20537)), "frozen E1 formal seeds/order")
    frozen = manifest["candidate"]
    blocks, raw_hashes, task_plan_hashes = [], {}, []
    matched_plans = {}
    matched_traces = {}
    required_sources = {"dynamic-provider.cpp", "dynamic-user.cpp", "dynamic-common.hpp",
                        "dynamic-oracle.cpp", "run_dynamic.py", "analyze_dynamic.py",
                        "dynamic_campaign.py", "summarize_dynamic.py", "analyze_credentials.py",
                        "common.hpp", "run_minindn.py", "wscript"}
    binaries = {"/usr/local/bin/" + name for name in
                ("ndnsf-dynamic-provider", "ndnsf-dynamic-user", "ndnsf-dynamic-oracle",
                 "App_ServiceController")}
    for index, (wanted, cell) in enumerate(zip(expected, manifest["cells"])):
        require(all(cell[k] == v for k, v in wanted.items()) and cell["qualified"], "cell identity")
        label = f"c{index:02d}-p{wanted['period_ms']}-d0-b{wanted['seed']}"
        if recovery or irregular:
            label += "-" + wanted["recovery_condition"]
        if irregular:
            label += "-" + wanted["availability_profile"]
        require(cell["id"] == label, "cell path/order")
        path = directory / label
        own = json.loads((path / "manifest.json").read_text())
        require(own["schema"] == ("ndnsf-dynamic205-e3-v1" if irregular else "ndnsf-dynamic205-e2-v1" if recovery else "ndnsf-dynamic205-v2") and own["purpose"] == "performance"
                and own["handler_ms"] == 5 and not own["force_unavailable"], "cell scope")
        require(all((own["availability_trace"]["profile"] if k == "availability_profile"
                     else own[k]) == v for k, v in wanted.items()), "cell configuration")
        offset = (wanted["seed"] - 20401) % 4
        require(own["arm_order"] == list(ARMS[offset:] + ARMS[:offset]), "arm order")
        require(set(own["binaries"]) == binaries, "binary inventory")
        require(required_sources <= set(own["source_sha256"]), "source inventory")
        require(set(own["configuration"]) == {"hello.policies", "trust-schema.conf"}, "configuration inventory")
        # Compare the recorded identities, never today's installed files/symlink targets.
        expected_names = required_sources | set(own["configuration"]) | {"contracts.md"}
        named = {}
        for name in expected_names:
            entries = [value for key, value in frozen.items() if Path(key).name == name]
            require(len(entries) == 1, "missing/ambiguous frozen source: " + name)
            named[name] = entries[0]
        for name in required_sources:
            require(own["source_sha256"][name] == named[name], "source identity: " + name)
        for name, value in own["configuration"].items():
            require(value == named[name], "configuration identity")
        require(own["contract_sha256"] == named["contracts.md"], "contract identity")
        expected_runtime = {key: value for key, value in frozen.items()
                            if key in binaries or ".so" in Path(key).name}
        require(own["libraries"] and {**own["libraries"], **own["binaries"]} == expected_runtime,
                "complete binary/library identity")
        result = analyze(path)
        if irregular:
            key = (wanted["seed"], wanted["period_ms"])
            plan_hash = digest(path / "task-plan.json")
            require(matched_plans.setdefault(key, plan_hash) == plan_hash,
                    "paired task plans differ")
            trace_key = (wanted["seed"], wanted["availability_profile"], wanted["window_ms"])
            trace_hashes = tuple((entry["producer"], entry["sha256"])
                                 for entry in own["availability_trace"]["files"])
            require(matched_traces.setdefault(trace_key, trace_hashes) == trace_hashes,
                    "paired availability traces differ")
        if recovery:
            task_plan_hashes.append(digest(path / "task-plan.json"))
            if index % 2:
                require(task_plan_hashes[-1] == task_plan_hashes[-2], "paired task plans differ")
        require(result["qualified"] and result["window_ms"] == 60000
                and len(result["results"]) == 4
                and {r["arm"] for r in result["results"]} == set(ARMS), "four-arm qualification")
        denominator = len(range(0, wanted["window_ms"], wanted["period_ms"]))
        require(all(r["tasks"] == denominator and r["seed"] == wanted["seed"]
                    for r in result["results"]), "fixed cohort denominator")
        blocks.append({r["arm"]: r for r in result["results"]})
        raw_hashes[label] = {p.name: digest(p) for p in path.iterdir()
                             if p.suffix in (".json", ".jsonl", ".pcap")
                             or p.name.endswith("-record-oracle.log")}
    metrics = PRIMARY + ("success_fraction", "conditional_latency_mean_ms",
                         "conditional_latency_p50_ms", "conditional_latency_p95_ms",
                         "all_task_terminal_mean_ms", "all_task_deadline_bounded_mean_ms",
                         "ineligible_admissions", "duplicate_executions", "late_response_count",
                         "eligible_execution_without_success", "refresh_interest_count")
    groups, contrasts = [], []
    if irregular:
        for profile, period in E3_CONDITIONS:
            selected = [b for b, c in zip(blocks, expected)
                        if (c["availability_profile"], c["period_ms"]) == (profile, period)]
            for arm in ARMS:
                rows = [b[arm] for b in selected]
                counts = Counter()
                for row in rows:
                    counts.update(row["terminal_counts"])
                groups.append({"arm": arm, "profile": profile, "period_ms": period,
                               "blocks": len(rows), "tasks": sum(r["tasks"] for r in rows),
                               "successes": sum(r["successes"] for r in rows),
                               "terminal_counts": dict(counts),
                               "metrics": {k: descriptive([r[k] for r in rows]) for k in metrics},
                               "window_and_drain_bytes_per_planned_task": descriptive([
                                   (r["wire"]["window"]["ip_bytes"] + r["wire"]["drain"]["ip_bytes"])
                                   / r["tasks"] for r in rows]),
                               "wire": {p: descriptive([r["wire"][p]["ip_bytes"] for r in rows])
                                        for p in ("before_window", "window", "drain")}})
                if arm == "SOLICIT":
                    continue
                for metric, target, effect in zip(PRIMARY, (100, 20480), (100, 10240)):
                    values = [None if b["SOLICIT"][metric] is None or b[arm][metric] is None
                              else b["SOLICIT"][metric] - b[arm][metric] for b in selected]
                    interval = paired_interval(values, formal, T15_BONFERRONI30)
                    half = interval["ci95_half_width"]
                    contrasts.append({"baseline": arm, "profile": profile, "period_ms": period,
                                      "metric": metric, "paired_values": values,
                                      **interval, "precision_target": target,
                                      "reporting_effect_threshold": effect,
                                      "precision_met": half <= target if half is not None else None})
    if recovery:
        for arm in ARMS:
            for condition in ("prepared", "reregister"):
                rows = [b[arm] for b, c in zip(blocks, expected) if c["recovery_condition"] == condition]
                counts = Counter()
                for row in rows:
                    counts.update(row["terminal_counts"])
                groups.append({"arm": arm, "condition": condition, "blocks": len(rows),
                               "tasks": sum(r["tasks"] for r in rows),
                               "successes": sum(r["successes"] for r in rows),
                               "terminal_counts": dict(counts),
                               "metrics": {k: descriptive([r[k] for r in rows]) for k in metrics},
                               "recovery_sensitive": [r["recovery_sensitive"] for r in rows],
                               "wire": {p: descriptive([r["wire"][p]["ip_bytes"] for r in rows])
                                        for p in ("before_window", "window", "drain")}})
        for arm in ARMS[1:]:
            for metric, target in (("first_two_failure_penalized_mean_ms", 100), (PRIMARY[1], 20480)):
                differences = []
                for i in range(0, len(blocks), 2):
                    pair = {expected[j]["recovery_condition"]: blocks[j] for j in (i, i+1)}
                    values = []
                    for condition, treatment in (("reregister", "SOLICIT"), ("prepared", "SOLICIT"),
                                                 ("reregister", arm), ("prepared", arm)):
                        row = pair[condition][treatment]
                        values.append(row["recovery_sensitive"]["failure_penalized_mean_ms"]
                                      if metric.startswith("first_two") else row[metric])
                    differences.append(None if any(v is None for v in values)
                                       else values[0] - values[1] - values[2] + values[3])
                interval = paired_interval(differences, formal)
                half = interval["ci95_half_width"]
                contrasts.append({"baseline": arm, "metric": metric, "paired_values": differences,
                                  **interval, "precision_target": target,
                                  "precision_met": half <= target if half is not None else None})
    for arm in ARMS:
        if recovery or irregular:
            break
        rows = [block[arm] for block in blocks]
        counts = Counter()
        for row in rows:
            counts.update(row["terminal_counts"])
        groups.append({"arm": arm, "blocks": len(rows), "tasks": sum(r["tasks"] for r in rows),
                       "successes": sum(r["successes"] for r in rows), "terminal_counts": dict(counts),
                       "metrics": {key: descriptive([r[key] for r in rows]) for key in metrics},
                       "wire": {phase: descriptive([r["wire"][phase]["ip_bytes"] for r in rows])
                                for phase in ("before_window", "window", "drain")}})
        if arm == "SOLICIT":
            continue
        for metric, target in zip(PRIMARY, (20, 20 * 1024)):
            differences = [None if b["SOLICIT"][metric] is None or b[arm][metric] is None
                           else b["SOLICIT"][metric] - b[arm][metric] for b in blocks]
            interval = paired_interval(differences, formal)
            half = interval["ci95_half_width"]
            contrasts.append({"baseline": arm, "metric": metric, "paired_values": differences,
                              **interval, "precision_target": target,
                              "precision_met": half <= target if half is not None else None})
    return {"qualified": True, "purpose": matrix["purpose"], "formal": formal,
            "scope": ("E3 irregular availability and offered load; single-inflight strategy; finite trace distribution"
                      if irregular else "stable E2 service re-registration overall interaction; not pure feedback or device crash"
                      if recovery else "stable two-provider E1; simulated task suitability; no dynamic/reliability claim"),
            "statistical_unit": ("seed/trace paired runs within profile/period" if irregular else
                                 "separately started paired runs; seed controls class/order only"),
            "descriptive_weighting": "equal-weight block statistics; percentiles are not pooled request percentiles",
            "interval_method": (("approximate paired Student t, thirty Bonferroni contrasts" if irregular else
                                 "approximate paired Student t, six Bonferroni contrasts") if formal else None),
            "df": 15 if formal else None,
            "critical": (T15_BONFERRONI30 if irregular else T15_BONFERRONI6) if formal else None,
            "candidate": frozen, "matrix_sha256": manifest["matrix_sha256"],
            "design_sha256": manifest["design_sha256"], "raw_sha256": raw_hashes,
            "postprocessing_sha256": {p.name: digest(p) for p in
                                      (Path(__file__), Path(__file__).with_name("analyze_dynamic.py"))},
            "results": groups, "contrasts": contrasts}


def summarize(directory):
    manifest=json.loads((directory/"manifest.json").read_text())
    if manifest.get("schema") in ("ndnsf-dynamic-campaign205-v2", "ndnsf-dynamic-campaign205-e2-v1",
                                 "ndnsf-dynamic-campaign205-e3-v1"):
        return summarize_v2(directory, manifest)
    require(manifest.get("schema") == "ndnsf-dynamic-campaign204-v1", "unknown campaign schema")
    require(manifest["status"]=="CAMPAIGN_COMPLETE" and not manifest.get("first_failure"),"incomplete campaign")
    expected={(p,d,s) for p in (0,4700) for d in (0,500,5000) for s in range(20401,20406)}
    actual=[(r["period_ms"],r["dwell_ms"],r["seed"]) for r in manifest["cells"]]
    require(len(actual)==30 and set(actual)==expected,"complete thirty-cell matrix")
    blocks={};raw_hashes={}
    for cell in manifest["cells"]:
        path=directory/cell["id"];result=analyze(path)
        require(result["qualified"] and result["window_ms"]==60000,"formal window qualification")
        own=json.loads((path/"manifest.json").read_text())
        require(own["purpose"]=="performance" and own["handler_ms"]==5 and not own["force_unavailable"],"negative/smoke exclusion")
        require({r["arm"] for r in result["results"]}==set(ARMS),"four-arm inventory")
        require({"dynamic-provider.cpp","dynamic-user.cpp","dynamic-common.hpp","run_dynamic.py","analyze_dynamic.py",
                 "common.hpp","run_minindn.py","wscript"}
                <=set(own["source_sha256"]),"required source inventory")
        require(set(own["configuration"])=={"hello.policies","trust-schema.conf"},"required configuration inventory")
        binaries={"/usr/local/bin/ndnsf-dynamic-provider","/usr/local/bin/ndnsf-dynamic-user","/usr/local/bin/App_ServiceController"}
        require(set(own["binaries"])==binaries,"required binary inventory")
        frozen_paths={str(Path(p).resolve()):value for p,value in manifest["candidate"].items()}
        for kind in ("binaries","libraries"):
            require(own[kind],"empty "+kind)
            for recorded,value in own[kind].items():
                canonical=str(Path(recorded).resolve())
                require(canonical in frozen_paths and value==frozen_paths[canonical],kind+" identity")
        for source,frozen in manifest["candidate"].items():
            name=Path(source).name
            if name in own["configuration"]:require(own["configuration"][name]==frozen,"configuration identity")
            if name=="code-design.md":require(own["contract_sha256"]==frozen,"contract identity")
            if name in own["source_sha256"]:require(own["source_sha256"][name]==frozen,"source identity")
        for row in result["results"]:
            require((row["period_ms"],row["dwell_ms"],row["seed"])==(cell["period_ms"],cell["dwell_ms"],cell["seed"]),"matched cell identity")
            key=(row["period_ms"],row["dwell_ms"],row["seed"],row["arm"])
            require(key not in blocks,"duplicate block");blocks[key]=row
        raw_hashes[cell["id"]]={str(p.relative_to(path)):hashlib.sha256(p.read_bytes()).hexdigest()
                                for p in path.iterdir() if p.suffix in (".jsonl",".pcap") or p.name=="manifest.json"}
    groups=[]
    metrics=("success_fraction","throughput_per_s","conditional_latency_p50_ms","window_ip_bytes_per_success",
             "ineligible_admissions","eligible_execution_without_success","duplicate_executions","late_response_count","refresh_interest_count")
    for period in (0,4700):
        for dwell in (0,5000,500):
            for arm in ARMS:
                rows=[blocks[(period,dwell,seed,arm)] for seed in range(20401,20406)]
                totals=Counter()
                for row in rows:totals.update(row["terminal_counts"])
                phases=Counter()
                for row in rows:phases.update(row["admission_phase_counts"])
                own={"period_ms":period,"dwell_ms":dwell,"arm":arm,"blocks":5,
                     "tasks":sum(r["tasks"] for r in rows),"successes":sum(r["successes"] for r in rows),
                     "terminal_counts":dict(totals),"admission_phase_counts":dict(phases),"metrics":{},"paired_solicit_difference":{}}
                for metric in metrics:
                    values=[r[metric] for r in rows if r[metric] is not None]
                    own["metrics"][metric]=estimate(values) if values else {"n":0,"mean":None,"ci95_half_width":None}
                own["wire"]={phase:estimate([r["wire"][phase]["ip_bytes"] for r in rows])
                              for phase in ("before_window","window","drain")}
                for metric in ("success_fraction","throughput_per_s","conditional_latency_p50_ms","window_ip_bytes_per_success"):
                    differences=[]
                    for seed,row in zip(range(20401,20406),rows):
                        solicit=blocks[(period,dwell,seed,"SOLICIT")][metric]
                        if solicit is not None and row[metric] is not None:differences.append(solicit-row[metric])
                    own["paired_solicit_difference"][metric]=estimate(differences) if differences else {"n":0,"mean":None,"ci95_half_width":None}
                groups.append(own)
    return {"qualified":True,"scope":"two-provider availability trace, closedloop profiles; physical-link IP bytes; no flight/capacity claim",
            "statistical_unit":"five matched blocks, equal weight; t4 95% intervals when all five contribute",
            "candidate":manifest["candidate"],"raw_sha256":raw_hashes,
            "postprocessing_sha256":{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in
                (Path(__file__).resolve(),Path(__file__).with_name("analyze_dynamic.py"),Path(__file__).with_name("analyze_credentials.py"))},
            "results":groups}


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("directory",type=Path)
    args=parser.parse_args();print(json.dumps(summarize(args.directory),indent=2))
