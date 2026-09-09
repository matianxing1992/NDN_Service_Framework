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
    base = tmp_path / "base.sif"
    base.write_bytes(b"base")
    application = tmp_path / "application-manifest.json"
    application.write_text(json.dumps({"schema": "spec183-external-application-v1"}), encoding="utf-8")
    evidence = {}
    for case, kinds in gate.CASE_EVIDENCE.items():
        for kind in kinds:
            path = tmp_path / (case + "-" + kind + ".json")
            request_id = "/example/yolo/run-1/" + case
            if kind == "lifecycle":
                milestones = [
                    "INPUT_REFERENCE_PUBLISHED", "REQUEST_SENT", "ACK_CLOSED",
                    "GRAPH_READY", "PLACEMENT_DECISION", "ARTIFACTS_READY",
                    "PLAN_SEALED", "SELECTION_COMMITTED", "PROVIDER_EXECUTION_STARTED",
                ]
                if case == "normal":
                    milestones.append("TERMINAL_RESPONSE")
                rows = []
                for sequence, milestone in enumerate(milestones):
                    row = {"schema": "spec180-yolo-lifecycle-event-v1", "caseId": "Y-B",
                           "requestId": request_id, "attemptId": "attempt-1",
                           "sequence": sequence, "milestone": milestone}
                    if milestone == "TERMINAL_RESPONSE":
                        row.update(status=True, requestCount=1)
                    if milestone == "PLAN_SEALED":
                        row["planDigest"] = "sha256:" + "2" * 64
                    rows.append(json.dumps(row))
                path.write_text("\n".join(rows) + "\n", encoding="utf-8")
            elif kind == "execution":
                rows = []
                for role in ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge"):
                    merge = role == "Merge"
                    rows.append("NDNSF_DI_EXECUTION_EVIDENCE_UPDATE " + json.dumps({
                        "providerName": "/example/yolo/run-1/" + role,
                        "requestId": request_id,
                        "cpuFallbackUsed": "false", "executionCompleted": "true",
                        "runnerKind": "native-yolo-postprocess" if merge else "onnxruntime-cpu",
                        "realCompute": "false" if merge else "true",
                        "loadCompleted": "false" if merge else "true",
                        "warmupCompleted": "false" if merge else "true"}))
                path.write_text("\n".join(rows) + "\n", encoding="utf-8")
            elif kind == "numeric":
                path.write_text(json.dumps({
                    "schemaVersion": "spec180-yolo-numerical-v1", "matched": True,
                    "requestId": request_id, "shape": [1, 50, 6],
                    "responseDigest": "sha256:" + "1" * 64}), encoding="utf-8")
            elif kind == "failure":
                boundary = ("PROVIDER_GRANT_VERIFICATION" if case == "permission-rejection"
                            else "DEPENDENCY_DATA_MISSING")
                reason = ("DI_PROTECTED_GRANT_REJECTED: test"
                          if case == "permission-rejection" else "DEPENDENCY_DATA_MISSING")
                value = {
                    "schema": "spec180-negative-evidence-v1", "status": "PASS",
                    "requestId": request_id, "provider": "/example/yolo/run-1/BackboneNeck",
                    "boundary": boundary, "reason": reason}
                if case == "negative-dependency":
                    value.update({
                        "observedAfterSelection": True,
                        "reselected": False,
                        "dependency": {
                            "schema": "ndnsf-di-withheld-output-v1",
                            "session": "run-1/session/1",
                            "requestId": request_id,
                            "attempt": "1",
                            "planDigest": "sha256:" + "2" * 64,
                            "producerRole": "DetectShard0",
                            "consumerRole": "Merge",
                            "manifestDataName": "/tensor/head0/MANIFEST",
                            "plannedDataName": "/tensor/head0",
                            "endpointDigest": "sha256:" + "3" * 64,
                            "contentDigest": "sha256:" + "4" * 64,
                            "bytes": "64",
                            "provider": "/example/yolo/run-1/BackboneNeck",
                            "providerBootId": "boot-1",
                            "atMs": "100",
                        },
                    })
                path.write_text(json.dumps(value), encoding="utf-8")
            elif kind == "cleanup":
                path.write_text(json.dumps({
                    "schema": "minindn-owned-cleanup-v1", "errors": [],
                    "networkStopped": True,
                    "children": [{"reaped": True, "forced": False}],
                    "networkResourceObservations": [{"observation": {"clean": True}}]}), encoding="utf-8")
            evidence[(case, kind)] = {"path": path.name, "bytes": path.stat().st_size,
                                      "sha256": digest(path), "kind": kind}
    cases = []
    for case in gate.CASES:
        cases.append({
            "case": case, "runId": "run-1", "status": "PASS", "requestCount": 1,
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
        "runId": "run-1", "baseSifSha256": digest(base),
        "applicationManifestSha256": digest(application),
        "applicationName": "/example/yolo", "providerCount": 4,
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


def test_yolo_host_gate_rejects_hash_valid_but_semantically_empty_evidence(tmp_path):
    source, receipt, value = write_receipt(tmp_path)
    row = next(item for item in value["cases"][0]["evidence"]
               if item["kind"] == "numeric")
    target = tmp_path / row["path"]
    target.write_text(json.dumps({"case": "normal", "kind": "numeric"}), encoding="utf-8")
    row["bytes"] = target.stat().st_size
    row["sha256"] = digest(target)
    receipt.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(ValueError, match="YOLO_HOST_GATE_NUMERICAL"):
        gate.validate_yolo_host_gate(receipt, source_seal_path=source)


def test_yolo_host_gate_rejects_wrong_negative_boundary(tmp_path):
    source, receipt, value = write_receipt(tmp_path)
    row = next(item for item in value["cases"][2]["evidence"]
               if item["kind"] == "failure")
    target = tmp_path / row["path"]
    target.write_text(json.dumps({
        "schema": "spec180-negative-evidence-v1", "status": "PASS",
        "requestId": "/example/yolo/run-1/negative-dependency",
        "provider": "/example/yolo/run-1/Merge",
        "boundary": "PLACEMENT_DECISION", "reason": "NO_FEASIBLE_CANDIDATE",
    }), encoding="utf-8")
    row["bytes"] = target.stat().st_size
    row["sha256"] = digest(target)
    receipt.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(ValueError, match="YOLO_HOST_GATE_DEPENDENCY_BOUNDARY"):
        gate.validate_yolo_host_gate(receipt, source_seal_path=source)
