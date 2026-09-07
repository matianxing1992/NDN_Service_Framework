import hashlib
import importlib.util
import json
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "packaging/ndnsf-di-container/lib/spec183_yolo_host_gate.py"
spec = importlib.util.spec_from_file_location("spec183_yolo_host_gate", MODULE)
gate = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(gate)


def digest(path):
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def write_receipt(tmp_path):
    source = tmp_path / "source-seal.json"
    source_body = {
        "schemaVersion": "spec170-local-sif-source-v1",
        "sourceRevision": "1" * 40,
        "sealDigest": "sha256:" + "f" * 64,
    }
    source.write_text(json.dumps(source_body), encoding="utf-8")
    evidence = {}
    for case, kinds in gate.CASE_EVIDENCE.items():
        for kind in kinds:
            path = tmp_path / (case + "-" + kind + ".json")
            path.write_text(json.dumps({"case": case, "kind": kind}), encoding="utf-8")
            evidence[(case, kind)] = {"path": path.name, "bytes": path.stat().st_size,
                                      "sha256": digest(path), "kind": kind}
    cases = []
    for case in gate.CASES:
        cases.append({
            "case": case, "status": "PASS", "requestCount": 1,
            "responseStatus": "PASS" if case == "normal" else "REJECTED",
            "success": case == "normal",
            "failureBoundary": None if case == "normal" else (
                "PERMISSION_DENIED" if case == "permission-rejection"
                else "DEPENDENCY_DATA_MISSING"),
            "reselectionCount": 0,
            "evidence": [evidence[(case, kind)] for kind in sorted(gate.CASE_EVIDENCE[case])],
        })
    return source, tmp_path / "host-gate.json", {
        "schema": gate.SCHEMA, "status": "PASS", "workload": gate.WORKLOAD,
        "sourceSeal": {"path": str(source), "sha256": digest(source),
                        "sealDigest": source_body["sealDigest"],
                        "sourceRevision": source_body["sourceRevision"]},
        "applicationName": "/example/yolo/run-1", "providerCount": 4,
        "graph": gate.GRAPH, "cases": cases,
    }


def test_yolo_host_gate_binds_source_and_all_registered_cases(tmp_path):
    source, receipt, value = write_receipt(tmp_path)
    receipt.write_text(json.dumps(value), encoding="utf-8")
    observed = gate.validate_yolo_host_gate(receipt, source_seal_path=source)
    assert observed["qualification"] == "YOLO_HOST_GATE_COMPONENT_ONLY"
    assert set(observed["cases"]) == set(gate.CASES)


@pytest.mark.parametrize("mutation", [
    "schema", "workload", "source-digest", "source-seal-digest",
    "source-path-type", "failed-case", "missing-evidence", "self-reference",
    "path-escape",
])
def test_yolo_host_gate_rejects_unbound_or_failed_receipt(tmp_path, mutation):
    source, receipt, value = write_receipt(tmp_path)
    if mutation == "schema":
        value["schema"] = "spec175-host-minindn-manifest-v2"
    elif mutation == "workload":
        value["workload"] = "tiny-onnx"
    elif mutation == "source-digest":
        value["sourceSeal"]["sha256"] = "sha256:" + "0" * 64
    elif mutation == "source-seal-digest":
        value["sourceSeal"]["sealDigest"] = "sha256:" + "0" * 64
    elif mutation == "source-path-type":
        value["sourceSeal"]["path"] = 17
    elif mutation == "failed-case":
        value["cases"][0]["status"] = "FAIL"
    elif mutation == "missing-evidence":
        value["cases"][0]["evidence"].pop()
    elif mutation == "self-reference":
        row = value["cases"][0]["evidence"][0]
        row["path"] = receipt.name
        row["bytes"] = receipt.stat().st_size if receipt.exists() else 1
        row["sha256"] = "sha256:" + "0" * 64
    else:
        row = value["cases"][0]["evidence"][0]
        row["path"] = "../outside.json"
    receipt.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(ValueError):
        gate.validate_yolo_host_gate(receipt, source_seal_path=source)


def test_yolo_host_gate_rejects_evidence_symlink(tmp_path):
    source, receipt, value = write_receipt(tmp_path)
    target = tmp_path / "outside.json"
    target.write_text("outside", encoding="utf-8")
    row = value["cases"][0]["evidence"][0]
    original = tmp_path / row["path"]
    original.unlink()
    original.symlink_to(target)
    receipt.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(ValueError):
        gate.validate_yolo_host_gate(receipt, source_seal_path=source)
