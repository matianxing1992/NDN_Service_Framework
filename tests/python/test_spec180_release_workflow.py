from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys

import pytest

from test_spec180_tiger_contract import result_fixture, seal_oracles


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/spec180_release.py"
PROFILE_PATH = ROOT / "packaging/ndnsf-di-container/jobs/spec180/profile.json"


def load_release():
    spec = importlib.util.spec_from_file_location("spec180_release", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def dispatch_workload(gate: str) -> dict[str, object]:
    if gate == "yolo-functional":
        return {
            "schema": "spec180-dispatch-workload-v1",
            "gate": gate,
            "case": "Y-B",
            "entrypoint": "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py",
            "args": ["--case", "Y-B"],
            "environment": {
                "SPEC180_YOLO_CANONICAL_PACKAGE": "/bundle/candidate/yolo26n",
                "SPEC180_YOLO_CATALOGUE_REGISTRY": "/bundle/candidate/registry.json",
                "SPEC180_YOLO_CATALOG_DATA_NAME": "/example/controller/NDNSF/DI/catalogue/v1",
                "SPEC180_YOLO_CATALOG_SIGNER": "/example/controller",
                "SPEC180_YOLO_OFFER_TRUST_ROOT": "/bundle/candidate/trust-root.json",
                "SPEC180_YOLO_OFFER_PUBLIC_KEY_MAP": "/bundle/candidate/key-map.json",
                "SPEC180_YOLO_NATIVE_REQUESTER_CONFIG": "/models/native-requester.json",
                "SPEC180_YOLO_TOPOLOGY": "/bundle/candidate/topology.conf",
                "SPEC180_YOLO_CONFIG": "/bundle/candidate/config.json",
            },
            "evidenceSchema": "spec180-result-v1",
        }
    return {
        "schema": "spec180-dispatch-workload-v1",
        "gate": gate,
        "case": "QWEN-F",
        "entrypoint": "Experiments/NDNSF_DI_QwenAckDriven_Minindn.py",
        "args": ["--case", "QWEN-F"],
        "environment": {
            "SPEC180_QWEN_MODEL": "Qwen/Qwen3.6-27B",
            "SPEC180_QWEN_REVISION": "model-revision-1",
            "SPEC180_QWEN_MODEL_IDENTITY_DIGEST": "sha256:" + "a" * 64,
            "SPEC180_QWEN_PROMPT_DIGEST": "sha256:" + "b" * 64,
        },
        "evidenceSchema": "spec180-result-v1",
    }


def make_run(module, tmp_path: Path, *, gate: str = "yolo-functional"):
    sif = tmp_path / "runtime.sif"
    manifest = tmp_path / "model.manifest.json"
    workload = tmp_path / "workload.json"
    sif.write_bytes(b"candidate sif")
    manifest.write_text('{"schema":"model-manifest-v1"}\n', encoding="utf-8")
    workload.write_text(
        json.dumps(dispatch_workload(gate), sort_keys=True) + "\n",
        encoding="utf-8")
    profile = module.load_json(PROFILE_PATH)
    return {
        "schema": module.RUN_SCHEMA,
        "runId": "spec180-test-run",
        "gate": gate,
        "profileId": profile["profileId"],
        "profileSha256": module.canonical_digest(profile),
        "candidate": {
            "candidateId": "candidate-test",
            "candidateDigest": "sha256:" + "a" * 64,
        },
        "sif": {"path": str(sif), "sha256": digest_bytes(sif.read_bytes())},
        "model": {
            "manifest": str(manifest),
            "manifestSha256": digest_bytes(manifest.read_bytes()),
            "root": str(tmp_path / "models"),
        },
        "workload": {
            "id": "yolo-yb-test",
            "manifest": str(workload),
            "manifestSha256": digest_bytes(workload.read_bytes()),
        },
        "output": {"root": str(tmp_path / "output")},
    }


def test_fixed_profiles_render_without_scheduler_side_effect(tmp_path: Path):
    module = load_release()
    profile = module.load_profile(PROFILE_PATH)
    run = make_run(module, tmp_path)
    rendered = module.render(profile, run, ROOT, check_files=True)
    assert rendered["status"] == "PASS"
    assert rendered["gate"] == "yolo-functional"
    assert rendered["effectiveConfig"]["ackTimeoutMs"] == 1500
    assert rendered["effectiveConfig"]["requestTimeoutMs"] == 60000
    assert rendered["effectiveConfig"]["initialSvsSettleMs"] == 5000
    assert rendered["effectiveConfig"]["cwd"] == "/bundle"
    assert rendered["argv"][-1].endswith("yolo-functional.sbatch")


def test_unknown_run_field_is_rejected_before_scheduler(tmp_path: Path):
    module = load_release()
    profile = module.load_profile(PROFILE_PATH)
    run = make_run(module, tmp_path)
    run["ambientTimeoutMs"] = 1
    calls: list[object] = []
    with pytest.raises(module.ReleaseError, match="UNKNOWN_RUN_FIELD"):
        module.submit(
            "yolo-functional", profile, run, ROOT,
            scheduler=lambda argv, env: calls.append((argv, env)),
        )
    assert calls == []


def test_invalid_dispatch_workload_is_rejected_before_scheduler(tmp_path: Path):
    module = load_release()
    profile = module.load_profile(PROFILE_PATH)
    run = make_run(module, tmp_path)
    workload_path = Path(run["workload"]["manifest"])
    workload = json.loads(workload_path.read_text(encoding="utf-8"))
    workload["environment"]["SPEC180_UNSAFE"] = "ambient"
    workload_path.write_text(json.dumps(workload, sort_keys=True) + "\n",
                             encoding="utf-8")
    run["workload"]["manifestSha256"] = digest_bytes(workload_path.read_bytes())
    calls: list[object] = []
    with pytest.raises(module.ReleaseError, match="WORKLOAD_DISPATCH_INVALID"):
        module.submit(
            "yolo-functional", profile, run, ROOT,
            scheduler=lambda argv, env: calls.append((argv, env)),
        )
    assert calls == []


def test_remote_wrapper_rechecks_mounted_hashes_before_output_creation():
    wrapper = (ROOT / "packaging/ndnsf-di-container/jobs/spec180"
               / "run-functional.sh").read_text(encoding="utf-8")
    model_check = wrapper.index("actual_model_manifest_sha256")
    workload_check = wrapper.index("actual_workload_sha256")
    output_creation = wrapper.index("mkdir -p \"$SPEC180_OUTPUT_ROOT\"")
    assert "SPEC180_MODEL_MANIFEST_DIGEST_MISMATCH" in wrapper
    assert "SPEC180_WORKLOAD_DIGEST_MISMATCH" in wrapper
    assert model_check < output_creation
    assert workload_check < output_creation


def test_scheduler_is_called_once_only_after_full_validation(tmp_path: Path):
    module = load_release()
    profile = module.load_profile(PROFILE_PATH)
    run = make_run(module, tmp_path)
    calls: list[object] = []
    report = module.submit(
        "yolo-functional", profile, run, ROOT,
        scheduler=lambda argv, env: calls.append((list(argv), dict(env))),
    )
    assert report["status"] == "SUBMITTED"
    assert len(calls) == 1
    assert calls[0][0][0] == "sbatch"
    assert calls[0][0][1].startswith("--export=NONE,")
    for key, value in calls[0][1].items():
        assert f"{key}={value}" in calls[0][0][1]


def test_slurm_export_delimiter_is_rejected_before_scheduler(tmp_path: Path):
    module = load_release()
    profile = module.load_profile(PROFILE_PATH)
    run = make_run(module, tmp_path)
    run["output"]["root"] = str(tmp_path / "unsafe,output")
    with pytest.raises(module.ReleaseError, match="UNSAFE_SLURM_EXPORT"):
        module.render(profile, run, ROOT)


def test_profile_tamper_and_gate_mismatch_are_fail_closed(tmp_path: Path):
    module = load_release()
    profile = module.load_profile(PROFILE_PATH)
    run = make_run(module, tmp_path)
    run["profileSha256"] = "sha256:" + "b" * 64
    with pytest.raises(module.ReleaseError, match="PROFILE_DIGEST_MISMATCH"):
        module.render(profile, run, ROOT)
    run = make_run(module, tmp_path, gate="qwen-functional")
    run["model"]["root"] = "/tmp/qwen-models"
    # The manifest is not a signed Spec180 Qwen3.6-27B identity.  Release
    # must reject it before scheduler mutation, rather than allowing the
    # dispatcher to route a tiny/placeholder runtime under the QWEN-F label.
    with pytest.raises(module.ReleaseError, match="MODEL_MANIFEST_SCHEMA_UNSUPPORTED"):
        module.render(profile, run, ROOT, check_files=True)

    tampered = json.loads(json.dumps(profile))
    tampered["gates"]["yolo-functional"]["time"] = "12:00:00"
    with pytest.raises(module.ReleaseError, match="GATE_FIXED_VALUE_MISMATCH"):
        module.validate_profile(tampered)


def test_result_validator_binds_candidate_and_child_oracles(tmp_path: Path):
    validator_path = ROOT / "scripts/validate_spec180_results.py"
    spec = importlib.util.spec_from_file_location("spec180_results", validator_path)
    assert spec is not None and spec.loader is not None
    validator = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = validator
    spec.loader.exec_module(validator)
    candidate_digest = "sha256:" + "a" * 64
    evidence = tmp_path / "oracle-record.json"
    evidence.write_text('{"schema":"spec180-oracle-v1"}\n', encoding="utf-8")
    evidence_digest = digest_bytes(evidence.read_bytes())

    def oracle(fields):
        return {
            "schema": "spec180-oracle-v1",
            "version": 1,
            "status": "PASS",
            "candidateId": "candidate-test",
            "path": evidence.name,
            "sha256": evidence_digest,
            "fields": fields,
        }

    result = {
        "schema": validator.RESULT_SCHEMA,
        "candidateId": "candidate-test",
        "candidateDigest": candidate_digest,
        "gate": "yolo-functional",
        "status": "PASS",
        "requests": {"expected": 2, "completed": 2},
        "children": [{"id": "provider-0", "exitStatus": 0, "timedOut": False}],
        "protocolOracle": oracle({"lifecyclePath": "lifecycle.jsonl",
                                   "requestCount": 2,
                                   "terminalResponseCount": 2}),
        "resultOracle": oracle({"resultPath": "result.json",
                                 "requestCount": 2, "matched": True}),
        "runtimeOracle": oracle({"backend": "cuda-onnxruntime",
                                  "deviceIds": ["cuda:0", "cuda:1", "cuda:2"],
                                  "cpuFallback": False}),
        "redaction": oracle({"secretFindings": 0, "redacted": False}),
        "cleanup": oracle({"childExitCount": 1, "allExited": True,
                            "outputClosed": True}),
    }
    # The historical fixture above documents the retired two-request shape.
    # It must not be accepted under the revision-112 single-request contract.
    with pytest.raises(validator.ResultError, match="REQUEST_COUNT_MISMATCH"):
        validator.validate_result(result, candidate_digest, tmp_path)
    result = result_fixture(tmp_path)
    candidate_digest = result["candidateDigest"]
    assert validator.validate_result(result, candidate_digest,
                                     evidence_root=tmp_path) is True
    result["runtimeOracle"] = "PASS"
    with pytest.raises(validator.ResultError, match="ORACLE_NOT_OBJECT"):
        validator.validate_result(result, candidate_digest,
                                  evidence_root=tmp_path)
    result["candidateDigest"] = "sha256:" + "c" * 64
    with pytest.raises(validator.ResultError, match="CANDIDATE_DIGEST_MISMATCH"):
        validator.validate_result(result, candidate_digest, tmp_path)


def test_result_validator_rejects_semantically_incomplete_oracles(tmp_path: Path):
    validator_path = ROOT / "scripts/validate_spec180_results.py"
    spec = importlib.util.spec_from_file_location("spec180_results_semantic", validator_path)
    assert spec is not None and spec.loader is not None
    validator = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = validator
    spec.loader.exec_module(validator)
    candidate_digest = "sha256:" + "b" * 64
    evidence = tmp_path / "oracle-record.json"
    evidence.write_text('{"schema":"spec180-oracle-v1"}\n', encoding="utf-8")
    evidence_digest = digest_bytes(evidence.read_bytes())

    def oracle(fields):
        return {
            "schema": "spec180-oracle-v1", "version": 1, "status": "PASS",
            "candidateId": "candidate-test", "path": evidence.name,
            "sha256": evidence_digest, "fields": fields,
        }

    result = {
        "schema": validator.RESULT_SCHEMA, "candidateId": "candidate-test",
        "candidateDigest": candidate_digest, "gate": "yolo-functional",
        "status": "PASS", "requests": {"expected": 2, "completed": 2},
        "children": [{"id": "provider-0", "exitStatus": 0, "timedOut": False}],
        "protocolOracle": oracle({"lifecyclePath": "lifecycle.jsonl",
                                   "requestCount": 2, "terminalResponseCount": 2}),
        "resultOracle": oracle({"resultPath": "result.json",
                                 "requestCount": 2, "matched": True}),
        "runtimeOracle": oracle({"backend": "cuda-onnxruntime",
                                  "deviceIds": ["cuda:0", "cuda:1", "cuda:2"],
                                  "cpuFallback": False}),
        "redaction": oracle({"secretFindings": 0, "redacted": False}),
        "cleanup": oracle({"childExitCount": 1, "allExited": True,
                            "outputClosed": True}),
    }
    result = result_fixture(tmp_path)
    candidate_digest = result["candidateDigest"]
    result["protocolOracle"]["fields"]["requestCount"] = 2
    seal_oracles(result, tmp_path)
    with pytest.raises(validator.ResultError, match="PROTOCOL_COUNT_MISMATCH"):
        validator.validate_result(result, candidate_digest, tmp_path)
    result["protocolOracle"]["fields"]["requestCount"] = 1
    result["runtimeOracle"]["fields"]["backend"] = "cpu-onnxruntime"
    seal_oracles(result, tmp_path)
    with pytest.raises(validator.ResultError, match="RUNTIME_BACKEND_MISMATCH"):
        validator.validate_result(result, candidate_digest, tmp_path)
    result["runtimeOracle"]["fields"]["backend"] = "cuda-onnxruntime"
    result["cleanup"]["fields"]["childExitCount"] = 0
    seal_oracles(result, tmp_path)
    with pytest.raises(validator.ResultError, match="CLEANUP_ORACLE_MISMATCH"):
        validator.validate_result(result, candidate_digest, tmp_path)
