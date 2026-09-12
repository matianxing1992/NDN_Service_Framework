from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py"


def load_module():
    spec = importlib.util.spec_from_file_location("spec184_qwen06b_local", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_example_profile_resolves_one_native_build_boundary():
    module = load_module()
    profile, profile_sha = module.load_profile(
        ROOT / "Experiments/profiles/ndnsf-di-qwen06b-local.example.json"
    )
    assert profile["buildDir"].endswith("build-spec184-b5-candidate-r4")
    assert profile["controllerBinary"].endswith(
        "build-spec184-b5-candidate-r4/examples/App_ServiceController"
    )
    assert profile_sha.startswith("sha256:")
    assert len(profile_sha) == len("sha256:") + 64


def test_profile_rejects_unknown_fields_before_execution(tmp_path: Path):
    module = load_module()
    profile = {
        "schema": module.PROFILE_SCHEMA,
        "profileId": "local-test",
        "topologyFile": "topology.conf",
        "stageNodes": ["one", "two"],
        "controllerNode": "controller",
        "userNode": "user",
        "buildDir": "build",
        "unexpected": True,
    }
    path = tmp_path / "profile.json"
    path.write_text(json.dumps(profile), encoding="utf-8")
    with pytest.raises(ValueError, match="PROFILE_UNKNOWN_FIELDS:unexpected"):
        module.load_profile(path)


def test_missing_binary_is_a_machine_preflight_failure(tmp_path: Path):
    module = load_module()
    result = module.binary_check(tmp_path / "missing-binary")
    assert result["status"] == "FAIL"
    assert result["reason"] == "MISSING_OR_NOT_EXECUTABLE"


def test_process_census_ignores_the_orchestrator_itself(tmp_path: Path):
    module = load_module()
    result = module.process_census(tmp_path / "isolated-run")
    assert result["status"] == "PASS"
    assert result["alive"] == []


def test_candidate_digest_is_canonical_and_changes_with_inputs():
    module = load_module()
    first = module.canonical_digest({"b": 2, "a": 1})
    same = module.canonical_digest({"a": 1, "b": 2})
    changed = module.canonical_digest({"a": 1, "b": 3})
    assert first == same
    assert first != changed
    assert first.startswith("sha256:")


def test_model_layer_requires_explicit_canonical_source():
    module = load_module()
    result = module.canonical_source_info(None, None)
    assert result == {
        "status": "WAITING_EXTERNAL_INPUT",
        "reason": "MODEL_CANONICAL_SOURCE_REQUIRED",
    }


def test_model_layer_rejects_non_onnx_source(tmp_path: Path):
    module = load_module()
    source = tmp_path / "source.onnx"
    source.write_text("stage metadata is not an ONNX protobuf", encoding="utf-8")
    result = module.canonical_source_info(source, None)
    assert result["status"] == "FAIL"
    assert result["reason"] == "MODEL_CANONICAL_SOURCE_NOT_ONNX"


def test_model_layer_accepts_small_canonical_onnx_fixture():
    module = load_module()
    result = module.canonical_source_info(
        ROOT / "tests/fixtures/spec182/qwen-native-config.onnx", None)
    assert result["status"] == "PASS"
    assert result["nodeCount"] > 0


def test_runtime_failure_is_classified_after_startup_markers(tmp_path: Path):
    module = load_module()
    (tmp_path / "authority.log").write_text(
        "NATIVE_GRANT_AUTHORITY_READY\n", encoding="utf-8")
    for index in range(3):
        (tmp_path / f"provider-{index}.log").write_text(
            "NDNSF_DI_NATIVE_PROVIDER_READY\n", encoding="utf-8")
    (tmp_path / "requester-0.log").write_text(
        "NATIVE_REQUESTER_FAILED: DI_NATIVE_ONNX_PARSE\n", encoding="utf-8")
    assert module.startup_markers_observed(tmp_path)
    assert module.first_failure_marker(tmp_path) == "DI_NATIVE_ONNX_PARSE"
