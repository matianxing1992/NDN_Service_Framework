"""Offline statistics checks, not native execution qualification."""
import importlib.util
import copy
import json
import math
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2] / "Experiments/system-benefits"
sys.path.insert(0, str(ROOT))
SPEC = importlib.util.spec_from_file_location("spec205_dynamic_summary", ROOT / "summarize_dynamic.py")
SUMMARY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUMMARY)


def test_fixed_paired_interval():
    result = SUMMARY.paired_interval(list(range(16)), True)
    assert result["mean"] == 7.5
    # Sample variance of integers 0..15 is 68/3; reference quantile frozen in CD17.
    half = 3.0362832225931182 * math.sqrt(68 / 3) / 4
    assert result["ci95_half_width"] == pytest.approx(half)
    assert result["ci95"] == pytest.approx([7.5 - half, 7.5 + half])


def test_missing_pair_never_deleted():
    result = SUMMARY.paired_interval([1] * 15 + [None], True)
    assert result["n"] == 16 and result["missing"] == 1
    assert result["mean"] is None and result["ci95"] is None


def test_thirty_contrast_interval():
    result = SUMMARY.paired_interval(list(range(16)), True, SUMMARY.T15_BONFERRONI30)
    half = 3.8219733981616257 * math.sqrt(68 / 3) / 4
    assert result["ci95"] == pytest.approx([7.5 - half, 7.5 + half])
    missing = SUMMARY.paired_interval([1] * 15 + [None], True, SUMMARY.T15_BONFERRONI30)
    assert missing["n"] == 16 and missing["mean"] is None and missing["ci95"] is None


def test_pilot_has_no_inference():
    result = SUMMARY.paired_interval([1, 2, 3], False)
    assert result["mean"] == 2 and result["ci95"] is None


@pytest.mark.parametrize("size", [0, 3, 15, 17])
def test_wrong_formal_sample_rejected(size):
    with pytest.raises(ValueError):
        SUMMARY.paired_interval([1] * size, True)


@pytest.mark.parametrize("value", [math.nan, math.inf, -math.inf])
def test_nonfinite_metric_rejected(value):
    with pytest.raises(ValueError):
        SUMMARY.paired_interval([1] * 15 + [value], True)


@pytest.fixture
def campaign(tmp_path, monkeypatch):
    """Small archive fixture isolates orchestration; native analyzer is tested separately."""
    cell = {"period_ms": 4700, "dwell_ms": 0, "seed": 20511, "window_ms": 60000}
    matrix = {"version": 1, "purpose": "pilot", "design_reference": "original.md", "cells": [cell]}
    (tmp_path / "matrix.json").write_text(json.dumps(matrix))
    (tmp_path / "design.md").write_text("frozen design")
    names = ("dynamic-provider.cpp", "dynamic-user.cpp", "dynamic-common.hpp", "dynamic-oracle.cpp",
             "run_dynamic.py", "analyze_dynamic.py", "dynamic_campaign.py", "summarize_dynamic.py",
             "analyze_credentials.py", "common.hpp", "run_minindn.py", "wscript")
    sources = {name: "source-" + name for name in names}
    binaries = {"/usr/local/bin/" + name: "binary-" + name for name in
                ("ndnsf-dynamic-provider", "ndnsf-dynamic-user", "ndnsf-dynamic-oracle", "App_ServiceController")}
    libraries = {"/usr/local/lib/libfixture.so": "library"}
    config = {"hello.policies": "policy", "trust-schema.conf": "trust"}
    candidate = {"/source/" + name: value for name, value in {**sources, **config, "contracts.md": "contract"}.items()}
    candidate.update(binaries)
    candidate.update(libraries)
    label = "c00-p4700-d0-b20511"
    directory = tmp_path / label
    directory.mkdir()
    own = {**cell, "schema": "ndnsf-dynamic205-v2", "purpose": "performance", "handler_ms": 5,
           "force_unavailable": False, "arm_order": ["ADS1000", "ADS_EVENT", "SOLICIT", "ADS100"],
           "binaries": binaries, "libraries": libraries, "source_sha256": sources,
           "configuration": config, "contract_sha256": "contract"}
    (directory / "manifest.json").write_text(json.dumps(own))
    manifest = {"schema": "ndnsf-dynamic-campaign205-v2", "status": "CAMPAIGN_COMPLETE",
                "purpose": "pilot", "candidate": candidate, "matrix": [cell],
                "matrix_sha256": SUMMARY.digest(tmp_path / "matrix.json"), "design_snapshot": "design.md",
                "design_sha256": SUMMARY.digest(tmp_path / "design.md"),
                "cells": [{**cell, "id": label, "qualified": True}]}
    (tmp_path / "manifest.json").write_text(json.dumps(manifest))
    metrics = SUMMARY.PRIMARY + ("success_fraction", "conditional_latency_mean_ms", "conditional_latency_p50_ms",
        "conditional_latency_p95_ms", "all_task_terminal_mean_ms", "all_task_deadline_bounded_mean_ms",
        "ineligible_admissions", "duplicate_executions", "late_response_count",
        "eligible_execution_without_success", "refresh_interest_count")
    results = [{**dict.fromkeys(metrics, 1), "arm": arm, "seed": 20511, "tasks": 13, "successes": 13,
                "terminal_counts": {"success": 13}, "wire": {p: {"ip_bytes": 1}
                    for p in ("before_window", "window", "drain")}} for arm in SUMMARY.ARMS]
    monkeypatch.setattr(SUMMARY, "analyze", lambda path: {"qualified": True, "window_ms": 60000, "results": results})
    return tmp_path, manifest, own, directory


def test_archive_nominal(campaign):
    result = SUMMARY.summarize(campaign[0])
    assert result["qualified"] and not result["formal"]
    assert len(result["contrasts"]) == 6


@pytest.mark.parametrize("mutation", [None, "missing", "duplicate", "plan", "denominator",
                                    "profile", "formal", "zero", "trace", "formal_full", "formal_zero"])
def test_irregular_pilot_archive(campaign, monkeypatch, mutation):
    """Exercise aggregation with an explicit analyzer stub, not native qualification."""
    root, manifest, own, directory = campaign
    template = copy.deepcopy(SUMMARY.analyze(directory)["results"])
    cells, records = [], []
    full = mutation in ("formal_full", "formal_zero")
    planned = SUMMARY.e3_formal_cells() if full else [
        {"period_ms": period, "dwell_ms": 0, "window_ms": 60000,
         "seed": 20511, "recovery_condition": "prepared", "availability_profile": profile}
        for profile, period in SUMMARY.E3_CONDITIONS]
    for index, cell in enumerate(planned):
        profile, period, seed = cell["availability_profile"], cell["period_ms"], cell["seed"]
        label = f"c{index:02d}-p{period}-d0-b{seed}-prepared-{profile}"
        path = root / label
        path.mkdir()
        (path / "task-plan.json").write_text(str(period) + ("bad" if mutation == "plan" and index == 1 else ""))
        child = {**own, **cell, "schema": "ndnsf-dynamic205-e3-v1",
                 "availability_trace": {"profile": "bad" if mutation == "profile" else profile,
                    "files": [{"producer": provider, "sha256": profile + provider +
                               ("bad" if mutation == "trace" and period == 100 else "")}
                              for provider in ("A", "B")]}}
        del child["availability_profile"]
        offset = (seed - 20401) % 4
        child["arm_order"] = list(SUMMARY.ARMS[offset:] + SUMMARY.ARMS[:offset])
        (path / "manifest.json").write_text(json.dumps(child))
        cells.append(cell)
        records.append({**cell, "id": label, "qualified": True})
    if mutation == "missing":
        cells.pop(); records.pop()
    if mutation == "duplicate":
        cells[-1] = cells[0]; records[-1] = records[0]
    purpose = "formal" if mutation == "formal" or full else "pilot"
    matrix = {"version": 3, "purpose": purpose, "cells": cells}
    (root / "matrix.json").write_text(json.dumps(matrix))
    manifest.update(schema="ndnsf-dynamic-campaign205-e3-v1", purpose=purpose,
                    matrix=cells, cells=records, matrix_sha256=SUMMARY.digest(root / "matrix.json"))
    (root / "manifest.json").write_text(json.dumps(manifest))
    def analyze(path):
        rows = copy.deepcopy(template)
        own = json.loads((path / "manifest.json").read_text())
        period, seed = own["period_ms"], own["seed"]
        for row in rows:
            count = 13 if period == 4700 or mutation == "denominator" else 600
            row.update(seed=seed, tasks=count, successes=count, terminal_counts={"success": count})
            if mutation == "formal_full" and row["arm"] == "SOLICIT":
                for metric in SUMMARY.PRIMARY:
                    row[metric] += seed - 20590
            if (mutation == "zero" or mutation == "formal_zero" and seed == 20590) and row["arm"] == "SOLICIT":
                row.update(successes=0, success_fraction=0, terminal_counts={"rejected": count})
                row[SUMMARY.PRIMARY[0]] = 2000
                row[SUMMARY.PRIMARY[1]] = None
        return {"qualified": True, "window_ms": 60000, "results": rows}
    monkeypatch.setattr(SUMMARY, "analyze", analyze)
    if mutation not in (None, "zero", "formal_full", "formal_zero"):
        with pytest.raises(ValueError):
            SUMMARY.summarize(root)
        return
    result = SUMMARY.summarize(root)
    assert result["formal"] == full and len(result["results"]) == 20
    assert sum(r["tasks"] for r in result["results"]) == 4956 * (16 if full else 1)
    assert len(result["contrasts"]) == 30
    if not full:
        assert all(r["ci95"] is None for r in result["contrasts"])
    else:
        assert result["critical"] == SUMMARY.T15_BONFERRONI30
        assert all(r["n"] == 16 and len(r["paired_values"]) == 16 for r in result["contrasts"])
        if mutation == "formal_full":
            half = 3.8219733981616257 * math.sqrt(68 / 3) / 4
            assert all(r["paired_values"] == list(range(16)) for r in result["contrasts"])
            assert all(r["ci95"] == pytest.approx([7.5 - half, 7.5 + half])
                       for r in result["contrasts"])
        else:
            costs = [r for r in result["contrasts"] if r["metric"] == SUMMARY.PRIMARY[1]]
            assert all(r["mean"] is None and r["missing"] == 1 and r["ci95"] is None for r in costs)
    if mutation == "zero":
        costs = [r for r in result["contrasts"] if r["metric"] == SUMMARY.PRIMARY[1]]
        assert all(r["mean"] is None and r["missing"] == 1 for r in costs)
        assert all(r["terminal_counts"] == {"rejected": r["tasks"]}
                   for r in result["results"] if r["arm"] == "SOLICIT")


@pytest.mark.parametrize("missing", [False, True])
@pytest.mark.parametrize("reverse", [False, True])
@pytest.mark.parametrize("different_plan", [False, True])
def test_recovery_interaction_and_missing_pair(campaign, monkeypatch, missing, reverse, different_plan):
    root, manifest, own, directory = campaign
    original_analyze = SUMMARY.analyze
    template = copy.deepcopy(original_analyze(directory)["results"])
    cells = []
    records = []
    order = ["prepared", "reregister"]
    if reverse:
        order.reverse()
    for i, condition in enumerate(order):
        cell = {"period_ms": 4700, "dwell_ms": 0, "window_ms": 60000,
                "seed": 20511, "recovery_condition": condition}
        label = f"c{i:02d}-p4700-d0-b20511-{condition}"
        path = root / label
        path.mkdir()
        (path / "task-plan.json").write_text("different" if different_plan and i else "same plan")
        child = {**own, **cell, "schema": "ndnsf-dynamic205-e2-v1"}
        (path / "manifest.json").write_text(json.dumps(child))
        cells.append(cell)
        records.append({**cell, "id": label, "qualified": True})
    matrix = {"version": 2, "purpose": "pilot", "cells": cells}
    (root / "matrix.json").write_text(json.dumps(matrix))
    manifest.update(schema="ndnsf-dynamic-campaign205-e2-v1", matrix=cells, cells=records,
                    matrix_sha256=SUMMARY.digest(root / "matrix.json"))
    (root / "manifest.json").write_text(json.dumps(manifest))
    def analyze(path):
        rows = copy.deepcopy(template)
        recovery = path.name.endswith("reregister")
        for row in rows:
            value = (110 if row["arm"] == "SOLICIT" else 800) if recovery else 100
            row["recovery_sensitive"] = {"failure_penalized_mean_ms": value}
            row[SUMMARY.PRIMARY[1]] = None if missing and recovery else value * 10
        return {"qualified": True, "window_ms": 60000, "results": rows}
    monkeypatch.setattr(SUMMARY, "analyze", analyze)
    if different_plan:
        with pytest.raises(ValueError, match="paired task plans differ"):
            SUMMARY.summarize(root)
        return
    result = SUMMARY.summarize(root)
    assert len(result["results"]) == 8 and len(result["contrasts"]) == 6
    for contrast in result["contrasts"]:
        traffic = contrast["metric"] == SUMMARY.PRIMARY[1]
        expected = None if missing and traffic else -6900 if traffic else -690
        assert contrast["mean"] == expected
        assert contrast["ci95"] is None


@pytest.mark.parametrize("mutation", ["matrix", "design", "oracle", "library", "schema", "cohort", "contract"])
def test_archive_corruption_rejected(campaign, mutation):
    root, manifest, own, directory = campaign
    if mutation in ("matrix", "design"):
        path = root / ("matrix.json" if mutation == "matrix" else "design.md")
        path.write_bytes(path.read_bytes() + b' ')
    elif mutation == "oracle":
        del own["binaries"]["/usr/local/bin/ndnsf-dynamic-oracle"]
    elif mutation == "library":
        own["libraries"]["/usr/local/lib/libfixture.so"] = "wrong"
    elif mutation == "schema":
        manifest["schema"] = "ndnsf-dynamic-campaign206-v3"
    elif mutation == "cohort":
        manifest["cells"] = []
    elif mutation == "contract":
        own["contract_sha256"] = "wrong"
    (root / "manifest.json").write_text(json.dumps(manifest))
    (directory / "manifest.json").write_text(json.dumps(own))
    with pytest.raises(ValueError):
        SUMMARY.summarize(root)
