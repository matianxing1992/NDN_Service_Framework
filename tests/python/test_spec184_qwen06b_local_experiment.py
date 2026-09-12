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
