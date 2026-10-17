"""Offline evidence-integrity checks; these do not qualify native execution."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path

import pytest


SOURCE = Path(__file__).resolve().parents[2] / "Experiments/system-benefits/analyze_dynamic.py"
SPEC = importlib.util.spec_from_file_location("spec205_dynamic_analysis", SOURCE)
ANALYSIS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYSIS)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


@pytest.mark.parametrize("status", ["rejected", "attempts_exhausted", "timeout", "error"])
def test_fast_failure_charged_full_deadline(status):
    task = {"begin_ns": 1000000, "deadline_ns": 2001000000,
            "terminal_ns": 1000000, "terminal": status}
    assert ANALYSIS.failure_penalized_ms(task) == 2000
    task.update(terminal="success", terminal_ns=101000000)
    assert ANALYSIS.failure_penalized_ms(task) == 100


@pytest.mark.parametrize("end", [0, 2001000000, 2001000001])
def test_success_outside_deadline_rejected(end):
    task = {"begin_ns": 1000000, "deadline_ns": 2001000000,
            "terminal_ns": end, "terminal": "success"}
    with pytest.raises(ValueError):
        ANALYSIS.failure_penalized_ms(task)


@pytest.fixture
def recovery_terminals():
    return [{"sequence": i, "begin_ns": 0, "deadline_ns": 2000000000,
             "terminal_ns": 100000000, "terminal": "success"} for i in (1, 2, 3)]


def test_recovery_subset_keeps_first_failure(recovery_terminals):
    recovery_terminals[0]["terminal"] = "timeout"
    recovery_terminals[2]["terminal_ns"] = 1900000000
    result = ANALYSIS.recovery_cohort(recovery_terminals)
    assert result["planned_sequences"] == [1, 2]
    assert result["successes"] == 1
    assert result["failure_penalized_mean_ms"] == 1050


@pytest.mark.parametrize("indices", [(0,), (0, 0), (1, 2)])
def test_recovery_subset_rejects_incomplete_or_duplicate(recovery_terminals, indices):
    with pytest.raises(ValueError):
        ANALYSIS.recovery_cohort([recovery_terminals[i] for i in indices])


@pytest.fixture
def archive(tmp_path):
    plan = {"tasks": [{"task": 1}]}
    (tmp_path / "task-plan.json").write_text(json.dumps(plan))
    for actor in ("user", "drone1", "drone2"):
        (tmp_path / ("cell-" + actor + ".jsonl")).write_text('{}\n')
    run = {"id": "cell", "arm": "SOLICIT", "origin_ns": 10, "session": "fixture"}
    manifest = {"schema": "ndnsf-dynamic205-v2", "window_ms": 3000, "period_ms": 100,
                "dwell_ms": 0, "force_unavailable": False,
                "task_plan": {"file": "task-plan.json", "sha256": digest(tmp_path / "task-plan.json")}}
    result = {"event": "dynamic_record_oracle_complete", "qualified": True,
              "run_id": "cell", "tasks": 1}
    output = tmp_path / "cell-record-oracle.log"
    output.write_text(json.dumps(result))
    receipt = {"exit": 0, "output": output.name, "output_sha256": digest(output),
               "result": result, "binary_sha256": "fixture-binary",
               "inputs": {p.name: digest(p) for p in tmp_path.iterdir() if p != output},
               "context": {"run": copy.deepcopy(run), "manifest": copy.deepcopy(manifest)}}
    manifest["binaries"] = {"/usr/local/bin/ndnsf-dynamic-oracle": "fixture-binary"}
    run["record_oracle"] = receipt
    return tmp_path, manifest, run


def test_receipt_nominal(archive):
    assert ANALYSIS.verify_record_receipt(*archive) == {"tasks": [{"task": 1}]}


@pytest.fixture
def trace_archive(archive):
    directory, manifest, run = archive
    manifest.update(schema="ndnsf-dynamic205-e3-v1", recovery_condition="prepared",
                    clock_anchor={"mono_ns": 0, "wall_ns": 1000})
    files = []
    for index in range(2):
        producer = f"/example/hello/provider/{'AB'[index]}"
        path = directory / f"availability-{index}.json"
        path.write_text(json.dumps({"version": 1, "producer": producer, "window_ms": 3000,
                                   "tail": "hold", "changes": [{"offset_ms": 0, "available": True}]}))
        files.append({"producer": producer, "file": path.name, "sha256": digest(path)})
        run["record_oracle"]["inputs"][path.name] = digest(path)
    manifest["availability_trace"] = {"profile": "stable", "files": files}
    run["record_oracle"]["context"]["manifest"] = {
        key: copy.deepcopy(value) for key, value in manifest.items() if key != "binaries"}
    return archive


def test_trace_receipt_nominal(trace_archive):
    assert ANALYSIS.verify_record_receipt(*trace_archive) == {"tasks": [{"task": 1}]}


def test_analyze_preserves_native_qualified_unavailable_execution(trace_archive, monkeypatch):
    """Analysis regression only: native correctness is covered by the C++ archive fixtures."""
    directory, manifest, run = trace_archive
    origin = 10000000000
    run["origin_ns"] = origin
    plan = {"tasks": [{"task": 1, "arrival_ms": 0}, {"task": 2, "arrival_ms": 100}]}
    verified = []

    def native_receipt(*args):
        verified.append(True)
        return plan

    monkeypatch.setattr(ANALYSIS, "verify_record_receipt", native_receipt)
    manifest.update(status="RUN_COMPLETE", record_oracle={"qualified": True}, children=[],
                    runs=[run], seed=20580, purpose="wiring", handler_ms=5, scope="fixture",
                    source_sha256={}, libraries={}, configuration={}, contract_sha256="fixture",
                    capture={})
    users = [{"event": "window", "t0_ns": origin, "t1_ns": origin + 3000000000,
              "period_ms": 100}]
    for task in plan["tasks"]:
        start = origin + task["arrival_ms"] * 1000000
        users.append({"event": "terminal", "sequence": task["task"], "begin_ns": start,
                      "deadline_ns": start + 2000000000, "terminal_ns": start + 2000000000,
                      "attempts": 0, "terminal": "timeout", "command": str(task["task"])})
    users += [{"event": "cohort_closed", "tasks": 2},
              {"event": "shutdown", "started": True, "invalid": 0, "tasks": 2, "terminals": 2}]
    (directory / "cell-user.jsonl").write_text("\n".join(map(json.dumps, users)))
    for index, name in enumerate(("drone1", "drone2")):
        records = [{"event": "ready"}]
        if index == 0:
            records.append({"event": "execution", "task": {"phase": "measure"},
                            "provider": "/example/hello/provider/A", "payload_valid": True,
                            "begin_ns": origin + 800000000, "eligible": False, "command": "1"})
        records.append({"event": "shutdown", "ready": True})
        (directory / f"cell-{name}.jsonl").write_text("\n".join(map(json.dumps, records)))
    for peer in ("gs", "drone1", "drone2"):
        manifest["capture"][f"cell-{peer}"] = {
            "exit": 0, "drops": 0, "started_ns": 0, "guard_ns": 1,
            "stopped_ns": origin + 6000000000, "pcap": "unused.pcap"}
    monkeypatch.setattr(ANALYSIS, "ip_counts", lambda *args: {
        phase: {"ip_bytes": 0, "packets": 0} for phase in ("before_window", "window", "drain")})
    (directory / "manifest.json").write_text(json.dumps(manifest))
    result = ANALYSIS.analyze(directory)
    assert verified and result["results"][0]["ineligible_admissions"] == 1
    assert result["results"][0]["admission_phase_counts"] is None
    manifest["schema"] = "ndnsf-dynamic205-e2-v1"
    (directory / "manifest.json").write_text(json.dumps(manifest))
    with pytest.raises(ValueError, match="independent admission trace oracle"):
        ANALYSIS.analyze(directory)


@pytest.mark.parametrize("mutation", ["bytes", "hash", "producer", "profile", "missing"])
def test_trace_receipt_binding(trace_archive, mutation):
    directory, manifest, run = trace_archive
    if mutation == "bytes":
        with (directory / "availability-0.json").open("a") as stream:
            stream.write(" ")
    elif mutation == "hash":
        manifest["availability_trace"]["files"][0]["sha256"] = "changed"
    elif mutation == "producer":
        manifest["availability_trace"]["files"][0]["producer"] = "/other"
    elif mutation == "profile":
        manifest["availability_trace"]["profile"] = "fast"
    else:
        del run["record_oracle"]["inputs"]["availability-0.json"]
    with pytest.raises(ValueError):
        ANALYSIS.verify_record_receipt(*trace_archive)


@pytest.mark.parametrize("profile", ["stable", "slow", "fast"])
@pytest.mark.parametrize("window", [3000, 60000])
def test_frozen_trace_generation(profile, window):
    spec = importlib.util.spec_from_file_location("spec205_dynamic_runner", SOURCE.with_name("run_dynamic.py"))
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    for seed in (20580, 20581):
        traces = runner.availability_traces(profile, window, seed)
        assert traces == runner.availability_traces(profile, window, seed)
        for trace in traces:
            changes = trace["changes"]
            assert trace["tail"] == "hold" and changes[0]["offset_ms"] == 0
            assert len(changes) <= 256 and all(type(c["available"]) is bool for c in changes)
            intervals = [b["offset_ms"] - a["offset_ms"] for a, b in zip(changes, changes[1:])]
            intervals.append(window - changes[-1]["offset_ms"])
            assert min(intervals) > 0
            occupied = sum(length for change, length in zip(changes, intervals) if change["available"])
            assert occupied == (window if profile == "stable" else window // 2)
        if profile != "stable":
            assert all(a["offset_ms"] == b["offset_ms"] and a["available"] != b["available"]
                       for a, b in zip(traces[0]["changes"], traces[1]["changes"]))


@pytest.mark.parametrize("file", ["task-plan.json", "cell-user.jsonl", "cell-drone1.jsonl",
                                  "cell-drone2.jsonl", "cell-record-oracle.log"])
def test_modified_archive_rejected(archive, file):
    path = archive[0] / file
    original = path.read_bytes()
    path.write_bytes(original + b' ')
    assert path.read_bytes() != original
    with pytest.raises(ValueError):
        ANALYSIS.verify_record_receipt(*archive)


@pytest.mark.parametrize("mutation", ["exit", "missing-input", "binary", "origin", "period",
                                      "result", "output-path", "task-count"])
def test_modified_receipt_rejected(archive, mutation):
    directory, manifest, run = archive
    receipt = run["record_oracle"]
    if mutation == "exit":
        receipt["exit"] = 2
    elif mutation == "missing-input":
        del receipt["inputs"]["cell-drone1.jsonl"]
    elif mutation == "binary":
        receipt["binary_sha256"] = "other"
    elif mutation == "origin":
        run["origin_ns"] += 1
    elif mutation == "period":
        manifest["period_ms"] = 4700
    elif mutation == "result":
        receipt["result"]["qualified"] = False
    elif mutation == "output-path":
        receipt["output"] = "other.log"
    elif mutation == "task-count":
        receipt["result"]["tasks"] = 2
        output = directory / receipt["output"]
        output.write_text(json.dumps(receipt["result"]))
        receipt["output_sha256"] = digest(output)
    with pytest.raises(ValueError):
        ANALYSIS.verify_record_receipt(directory, manifest, run)
