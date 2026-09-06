"""Focused launch/result contract checks; no SIF, GPU, or network qualification."""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
ROLES = ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")


def load(path):
    spec = importlib.util.spec_from_file_location(Path(path).stem, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def result_fixture(tmp_path):
    plan_digest = "sha256:" + "a" * 64
    result = {
        "schema": "spec180-result-v1", "candidateId": "test-candidate",
        "candidateDigest": "sha256:" + "a" * 64,
        "gate": "yolo-functional", "status": "PASS",
        "requests": {"expected": 1, "completed": 1},
        "children": [{"id": name, "exitStatus": 0, "timedOut": False}
                     for name in ("nfd", "controller", "repo", "user",
                                  *("provider-" + role for role in ROLES))],
    }
    fields = {
        "protocolOracle": {"lifecyclePath": "lifecycle.jsonl", "requestCount": 1,
                           "terminalResponseCount": 1},
        "resultOracle": {"resultPath": "result.json", "requestCount": 1, "matched": True},
        "runtimeOracle": {
            "backend": "cuda-onnxruntime", "deviceIds": ["GPU-test-uuid"],
            "cpuFallback": False, "planDigest": plan_digest,
            "providers": [{"role": role, "provider": "/example/provider/" + role,
                           "pid": 100 + i,
                           "executionProvider": ("native-yolo-postprocess" if role == "Merge"
                                                 else "CUDAExecutionProvider"),
                           "physicalDeviceUuid": "" if role == "Merge" else "GPU-test-uuid",
                           "cudaVisibleDevices": "" if role == "Merge" else "0"}
                          for i, role in enumerate(ROLES)],
        },
        "redaction": {"secretFindings": 0, "redacted": False},
        "cleanup": {"childExitCount": 8, "allExited": True, "outputClosed": True},
    }
    observations = []
    for role in ROLES:
        profile_path = "profile-" + role + ".json"
        profile_digest = ""
        if role != "Merge":
            profile = tmp_path / profile_path
            profile.write_text('{"provider":"CUDAExecutionProvider"}')
            profile_digest = "sha256:" + hashlib.sha256(profile.read_bytes()).hexdigest()
        observations.append({
            "role": role, "provider": "/example/provider/" + role,
            "pid": 100 + ROLES.index(role), "requestId": "request-1",
            "attemptEpoch": 1, "runnerKind": ("native-yolo-postprocess" if role == "Merge"
                                                else "onnxruntime-cuda"),
            "realCompute": role != "Merge", "executionCompleted": True,
            "exactForwardCacheHit": False,
            "physicalDeviceUuid": "" if role == "Merge" else "GPU-test-uuid",
            "cudaVisibleDevices": "" if role == "Merge" else "0",
            "gpuIdentitySource": "" if role == "Merge" else "cuda-runtime-pci+driver-uuid",
            "profileRequestId": "" if role == "Merge" else "request-1",
            "profileAttemptEpoch": 0 if role == "Merge" else 1,
            "profilePath": "" if role == "Merge" else profile_path,
            "profileSha256": profile_digest,
            "nodeProviders": [] if role == "Merge" else ["CUDAExecutionProvider"],
            "planDigest": plan_digest,
        })
    fields["runtimeOracle"]["executionEvidence"] = observations
    for name, value in fields.items():
        result[name] = {"schema": "spec180-oracle-v1", "version": 1,
                        "status": "PASS", "candidateId": result["candidateId"],
                        "fields": value, "path": name + ".json", "sha256": ""}
    seal_oracles(result, tmp_path)
    return result


def seal_oracles(result, root):
    for name in ("protocolOracle", "resultOracle", "runtimeOracle", "redaction", "cleanup"):
        oracle = result[name]
        payload = {k: v for k, v in oracle.items() if k not in {"path", "sha256"}}
        raw = json.dumps(payload, sort_keys=True).encode()
        (root / oracle["path"]).write_bytes(raw)
        oracle["sha256"] = "sha256:" + hashlib.sha256(raw).hexdigest()


def test_one_cold_request_three_cuda_roles_share_one_gpu(tmp_path):
    module = load("scripts/validate_spec180_results.py")
    result = result_fixture(tmp_path)
    assert module.validate_result(result, result["candidateDigest"], tmp_path)


@pytest.mark.parametrize("mutation,error", [
    ("two_requests", "REQUEST_COUNT_MISMATCH"),
    ("boolean_count", "REQUEST_COUNT_MISMATCH"),
    ("qwen", "UNKNOWN_GATE"),
    ("three_gpus", "RUNTIME_DEVICE_MAP_MISMATCH"),
    ("wrong_uuid", "RUNTIME_PROVIDER_DEVICE_MISMATCH"),
    ("cpu_fallback", "RUNTIME_PROVIDER_BACKEND_MISMATCH"),
    ("duplicate_pid", "RUNTIME_PROVIDER_PID_INVALID"),
    ("merge_gpu", "RUNTIME_MERGE_DEVICE_MISMATCH"),
    ("missing_child", "CHILD_INVENTORY_MISMATCH"),
    ("forged_fields", "ORACLE_CONTENT_MISMATCH"),
])
def test_result_rejects_old_profile_and_unbound_evidence(tmp_path, mutation, error):
    module = load("scripts/validate_spec180_results.py")
    result = result_fixture(tmp_path)
    runtime = result["runtimeOracle"]["fields"]
    if mutation == "two_requests":
        result["requests"] = {"expected": 2, "completed": 2}
    elif mutation == "boolean_count":
        result["requests"]["completed"] = True
    elif mutation == "qwen":
        result["gate"] = "qwen-functional"
    elif mutation == "three_gpus":
        runtime["deviceIds"] = ["GPU-a", "GPU-b", "GPU-c"]
    elif mutation == "wrong_uuid":
        runtime["providers"][1]["physicalDeviceUuid"] = "GPU-other"
    elif mutation == "cpu_fallback":
        runtime["providers"][1]["executionProvider"] = "CPUExecutionProvider"
    elif mutation == "duplicate_pid":
        runtime["providers"][1]["pid"] = runtime["providers"][0]["pid"]
    elif mutation == "merge_gpu":
        runtime["providers"][3]["cudaVisibleDevices"] = "0"
    elif mutation == "missing_child":
        result["children"].pop()
    elif mutation == "forged_fields":
        result["resultOracle"]["fields"]["matched"] = False
    if mutation != "forged_fields":
        seal_oracles(result, tmp_path)
    with pytest.raises(module.ResultError, match=error):
        module.validate_result(result, result["candidateDigest"], tmp_path)


def renderer_inputs(tmp_path):
    policy = {"controller": "/example/controller", "group": "/example/group",
              "services": [{"name": "/AI/YOLO/2x2Inference",
                            "providers": [{"identity": "/example/provider/" + role,
                                           "roles": [role]} for role in ROLES]}]}
    config = tmp_path / "policy.json"
    config.write_text(json.dumps(policy))
    env = {"SPEC180_YOLO_" + key: "/models/" + key for key in (
        "CANONICAL_PACKAGE", "CATALOGUE_REGISTRY", "OFFER_TRUST_ROOT",
        "OFFER_PUBLIC_KEY_MAP", "CATALOG_DATA_NAME")}
    env.update(SPEC180_YOLO_CATALOG_SIGNER="/example/controller", SPEC180_YOLO_CONFIG=str(config))
    workload = tmp_path / "workload.json"
    workload.write_text(json.dumps({"schema": "spec180-dispatch-workload-v1",
                                    "gate": "yolo-functional", "case": "Y-B", "environment": env}))
    return workload, config, policy


def test_tiger_uses_four_native_providers_and_no_merge_gpu(tmp_path, monkeypatch):
    renderer = load("packaging/ndnsf-di-container/jobs/spec180/render-tiger-yb-args.py")
    workload, _, _ = renderer_inputs(tmp_path)
    monkeypatch.setattr(renderer, "_render_runtime_publication",
                        lambda path, *args: path.write_text("{}"))
    output = tmp_path / "args"
    renderer.render(workload, output, "/models")
    assert len(list((output / "providers").glob("*.args"))) == 4
    for role in ROLES:
        argv = (output / "providers" / (role + ".args")).read_text().splitlines()
        assert argv[:3] == ["/usr/bin/env", "CUDA_VISIBLE_DEVICES=" + ("" if role == "Merge" else "0"),
                            "/opt/ndnsf-di/current/bin/di-native-provider"]
        assert argv[argv.index("--roles") + 1] == role
        assert argv[argv.index("--provider") + 1] == "/example/provider/" + role
        assert argv[argv.index("--offer-backend") + 1] == (
            "onnxruntime-cpu" if role == "Merge" else "onnxruntime-cuda")
        assert "--serve" in argv and "--local-model-path" not in argv
        assert not any("provider.py" in arg for arg in argv)
    user = (output / "user.args").read_text().splitlines()
    assert user[user.index("--sequential-requests") + 1] == "1"
    assert "--native-tensor-input" in user
    repo = (output / "repo.args").read_text().splitlines()
    assert repo[1].endswith("/yolo_2x2/repo_node.py")
    assert repo[repo.index("--repo-node") + 1] == "/example/provider/Repo"


def test_tiger_rejects_duplicate_identity_before_writes(tmp_path):
    renderer = load("packaging/ndnsf-di-container/jobs/spec180/render-tiger-yb-args.py")
    workload, config, policy = renderer_inputs(tmp_path)
    policy["services"][0]["providers"][1] = copy.deepcopy(policy["services"][0]["providers"][0])
    config.write_text(json.dumps(policy))
    output = tmp_path / "args"
    with pytest.raises(SystemExit, match="PROCESS_PROFILE_MISMATCH"):
        renderer.render(workload, output, "/models")
    assert not output.exists()
