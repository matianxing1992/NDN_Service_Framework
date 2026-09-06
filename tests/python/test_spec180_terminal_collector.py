"""Focused tests for the candidate-bound Spec180 terminal collector."""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
ROLES = ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")
CANDIDATE_ID = "candidate-test"
CANDIDATE_DIGEST = "sha256:" + "a" * 64
REQUEST_ID = "/spec180-y-b-test"
ATTEMPT_ID = "attempt-test"
PLAN_DIGEST = "sha256:" + "b" * 64
RESPONSE_DIGEST = "sha256:" + "c" * 64


def load(path: str):
    spec = importlib.util.spec_from_file_location(Path(path).stem, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def _digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _children():
    return [{"id": name, "exitStatus": 0, "timedOut": False}
            for name in ("nfd", "controller", "repo", "user",
                         *("provider-" + role for role in ROLES))]


def _supervision():
    return {
        "children": _children(), "childExitCount": 8,
        "allExited": True, "outputClosed": True,
        "pids": {"provider-" + role: 100 + index
                 for index, role in enumerate(ROLES)},
    }


def _lifecycle(root: Path):
    fields = [
        {"referenceDigest": _digest(b"reference")},
        {"requestDigest": _digest(b"request")},
        {"ackSnapshotDigest": _digest(b"acks"), "ackCount": 4},
        {"graphDigest": _digest(b"graph"), "catalogueDigest": _digest(b"catalogue")},
        {"candidateId": CANDIDATE_ID, "candidateDigest": CANDIDATE_DIGEST,
         "candidatePriority": 1, "providerCount": 4},
        {"artifactDigest": _digest(b"artifact"), "artifactCount": 4},
        {"planDigest": PLAN_DIGEST},
        {"selectionDigest": _digest(b"selection"), "selectedRoleCount": 4},
        {"roleDigest": _digest(b"roles"), "providerCount": 4},
        {"resultDigest": RESPONSE_DIGEST, "requestCount": 1, "status": True},
    ]
    lines = []
    for sequence, (milestone, extra) in enumerate(zip(
            ("INPUT_REFERENCE_PUBLISHED", "REQUEST_SENT", "ACK_CLOSED",
             "GRAPH_READY", "PLACEMENT_DECISION", "ARTIFACTS_READY",
             "PLAN_SEALED", "SELECTION_COMMITTED",
             "PROVIDER_EXECUTION_STARTED", "TERMINAL_RESPONSE"), fields)):
        lines.append({
            "schema": "spec180-yolo-lifecycle-event-v1", "caseId": "Y-B",
            "requestId": REQUEST_ID, "attemptId": ATTEMPT_ID,
            "sequence": sequence, "timestampUnix": float(sequence + 1),
            "milestone": milestone, **extra,
        })
    (root / "lifecycle.jsonl").write_text(
        "".join(json.dumps(line, sort_keys=True, separators=(",", ":")) + "\n"
                for line in lines), encoding="utf-8")


def _numerical(root: Path):
    (root / "yolo-numerical.json").write_text(json.dumps({
        "schemaVersion": "spec180-yolo-numerical-v1", "case": "Y-B",
        "requestId": REQUEST_ID, "attemptId": ATTEMPT_ID,
        "planDigest": PLAN_DIGEST, "candidateId": CANDIDATE_ID,
        "candidateDigest": CANDIDATE_DIGEST, "matched": True,
        "responseDigest": RESPONSE_DIGEST,
    }, sort_keys=True), encoding="utf-8")


def _execution(root: Path):
    log_root = root / "runtime" / "log"
    log_root.mkdir(parents=True)
    for index, role in enumerate(ROLES):
        profile_path = "runtime/profile-" + role + ".json"
        profile_digest = ""
        if role != "Merge":
            profile = root / profile_path
            profile.write_text(json.dumps({"provider": "CUDAExecutionProvider"}))
            profile_digest = _digest(profile.read_bytes())
        observation = {
            "schema": "ndnsf-di-execution-evidence-v1",
            "providerName": "/example/provider/" + role,
            "evidenceEpoch": 1, "runnerKind": ("native-yolo-postprocess" if role == "Merge"
                                                 else "onnxruntime-cuda"),
            "realCompute": role != "Merge", "runtimeVersion": "test-runtime",
            "modelDigest": _digest(b"model"), "planDigest": PLAN_DIGEST,
            "roles": [role], "cpuFallbackUsed": False,
            "loadCompleted": True, "warmupCompleted": True,
            "gpuUuid": "" if role == "Merge" else "GPU-test-uuid",
            "providerProfilePath": "" if role == "Merge" else str(root / profile_path),
            "processId": 100 + index,
            "cudaVisibleDevices": "" if role == "Merge" else "0",
            "gpuIdentitySource": "" if role == "Merge" else "cuda-runtime-pci+driver-uuid",
            "requestId": REQUEST_ID, "attemptEpoch": 1,
            "executionCompleted": True, "exactForwardCacheHit": False,
            "profileRequestId": "" if role == "Merge" else REQUEST_ID,
            "profileAttemptEpoch": 0 if role == "Merge" else 1,
            "nodeProviderAssignments": [] if role == "Merge" else [{
                "role": role, "nodeName": role + "Node",
                "provider": "CUDAExecutionProvider", "modelNode": True,
            }],
        }
        (log_root / ("provider-" + role + ".log")).write_text(
            "NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED " +
            json.dumps(observation, sort_keys=True) + "\n", encoding="utf-8")


def evidence_fixture(tmp_path):
    root = tmp_path / "evidence"
    root.mkdir()
    _lifecycle(root)
    _numerical(root)
    _execution(root)
    return root, _supervision()


def test_collector_publishes_only_after_all_oracles_are_bound(tmp_path):
    collector = load("packaging/ndnsf-di-container/jobs/spec180/collect_spec180_result.py")
    validator = load("scripts/validate_spec180_results.py")
    root, supervision = evidence_fixture(tmp_path)
    result = collector.collect_terminal_result(root, CANDIDATE_ID,
                                               CANDIDATE_DIGEST, supervision)
    assert result["status"] == "PASS"
    assert (root / "spec180-result.json").is_file()
    validator.validate_result(json.loads((root / "spec180-result.json").read_text()),
                              CANDIDATE_DIGEST, root)


@pytest.mark.parametrize("mutation,error", [
    ("candidate", "LIFECYCLE_CANDIDATE_MISMATCH"),
    ("missing_numerical_candidate", "NUMERICAL_CANDIDATE_MISMATCH"),
    ("duplicate_observation", "EXECUTION_OBSERVATION_COUNT_MISMATCH:BackboneNeck"),
    ("cache_hit", "EXECUTION_CACHE_HIT:BackboneNeck"),
    ("secret", "REDACTION_SECRET_FINDING"),
])
def test_collector_fails_closed_without_terminal_result(tmp_path, mutation, error):
    collector = load("packaging/ndnsf-di-container/jobs/spec180/collect_spec180_result.py")
    root, supervision = evidence_fixture(tmp_path)
    if mutation == "candidate":
        lines = (root / "lifecycle.jsonl").read_text().replace(CANDIDATE_ID, "other-candidate")
        (root / "lifecycle.jsonl").write_text(lines)
    elif mutation == "missing_numerical_candidate":
        value = json.loads((root / "yolo-numerical.json").read_text())
        value.pop("candidateId")
        (root / "yolo-numerical.json").write_text(json.dumps(value))
    elif mutation == "duplicate_observation":
        path = root / "runtime" / "log" / "provider-BackboneNeck.log"
        path.write_text(path.read_text() + path.read_text())
    elif mutation == "cache_hit":
        path = root / "runtime" / "log" / "provider-BackboneNeck.log"
        path.write_text(path.read_text().replace('"exactForwardCacheHit": false',
                                                  '"exactForwardCacheHit": true'))
    elif mutation == "secret":
        path = root / "yolo-numerical.json"
        value = json.loads(path.read_text())
        value["secret"] = "password=should-not-be-recorded"
        path.write_text(json.dumps(value))
    with pytest.raises(collector.CollectionError, match=error):
        collector.collect_terminal_result(root, CANDIDATE_ID,
                                          CANDIDATE_DIGEST, supervision)
    assert not (root / "spec180-result.json").exists()


def test_validator_rechecks_profile_hash_after_collection(tmp_path):
    collector = load("packaging/ndnsf-di-container/jobs/spec180/collect_spec180_result.py")
    validator = load("scripts/validate_spec180_results.py")
    root, supervision = evidence_fixture(tmp_path)
    collector.collect_terminal_result(root, CANDIDATE_ID, CANDIDATE_DIGEST, supervision)
    profile = root / "runtime/profile-BackboneNeck.json"
    profile.write_text("tampered")
    with pytest.raises(validator.ResultError, match="RUNTIME_PROFILE_DIGEST_MISMATCH"):
        validator.validate_result(json.loads((root / "spec180-result.json").read_text()),
                                  CANDIDATE_DIGEST, root)
