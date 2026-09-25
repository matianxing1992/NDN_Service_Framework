from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py"


def load_module():
    spec = importlib.util.spec_from_file_location("spec190_qwen_native_minindn", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("ack_timeout_ms", [999, 1000, 1001])
def test_qwen_profile_accepts_explicit_bounded_ack_window(ack_timeout_ms: int):
    module = load_module()
    budget = module.qwen_runtime_budgets(
        1024, 2048, rounds=2, stage_count=3, ack_timeout_ms=ack_timeout_ms)
    assert budget["ack_timeout_ms"] == ack_timeout_ms
    assert budget["timeout_ms"] == 180_000


def test_qwen_profile_defaults_to_one_second_without_changing_core_default():
    module = load_module()
    budget = module.qwen_runtime_budgets(1024, 2048)
    assert budget["ack_timeout_ms"] == 1000
    assert module.DEFAULT_QWEN_ACK_TIMEOUT_MS == 1000


def test_qwen_profile_bounds_conversation_retention_to_authority_grant_ttl():
    module = load_module()
    budget = module.qwen_runtime_budgets(
        module.LARGE_MODEL_THRESHOLD_BYTES, 1, rounds=3, stage_count=2)
    assert budget["retention_ms"] == module.MAX_NATIVE_GRANT_TTL_MS
    assert budget["retention_ms"] <= budget["provider_run_ms"]


@pytest.mark.parametrize("ack_timeout_ms", [0, -1, 180_000, 900_000])
def test_qwen_profile_rejects_non_positive_or_deadline_sized_ack_window(ack_timeout_ms: int):
    module = load_module()
    with pytest.raises(ValueError, match="ACK window"):
        module.qwen_runtime_budgets(1024, 2048, ack_timeout_ms=ack_timeout_ms)


def test_qwen_stage_plan_marks_resident_session_only_when_requested(tmp_path: Path):
    module = load_module()
    stages = [
        {
            "role": "/LLM/Pipeline/Stage/0",
            "path": "/tmp/stage-0.onnx",
            "sha256": "sha256:" + "1" * 64,
            "layerRange": {"start": 0, "endExclusive": 1},
            "inputNames": ["input_ids", "attention_mask", "position_ids",
                            "past_key.0", "past_value.0"],
            "outputNames": ["hidden_states_out", "present_key.0",
                             "present_value.0"],
            "cacheInputs": ["past_key.0", "past_value.0"],
            "cacheOutputs": ["present_key.0", "present_value.0"],
        },
        {
            "role": "/LLM/Pipeline/Stage/1",
            "path": "/tmp/stage-1.onnx",
            "sha256": "sha256:" + "2" * 64,
            "layerRange": {"start": 1, "endExclusive": 2},
            "inputNames": ["hidden_states", "attention_mask", "position_ids",
                            "past_key.1", "past_value.1"],
            "outputNames": ["logits", "present_key.1", "present_value.1"],
            "cacheInputs": ["past_key.1", "past_value.1"],
            "cacheOutputs": ["present_key.1", "present_value.1"],
        },
    ]
    model = {
        "model": "Qwen/Qwen3-0.6B",
        "modelRevision": "test-revision",
        "dtype": "float32",
        "quantization": "int8",
        "eosTokenIds": [151645],
    }

    cold_dir = tmp_path / "cold"
    resident_dir = tmp_path / "resident"
    cold_dir.mkdir()
    resident_dir.mkdir()
    _, cold_manifest = module.stage_plan_and_manifest(
        cold_dir, model, stages, 2, "sha256:" + "3" * 64)
    _, resident_manifest = module.stage_plan_and_manifest(
        resident_dir, model, stages, 2, "sha256:" + "3" * 64,
        resident_session=True)

    cold = json.loads(cold_manifest.read_text())
    resident = json.loads(resident_manifest.read_text())
    assert "residentSession" not in cold["services"][0]["artifacts"][0]["metadata"]
    assert resident["services"][0]["artifacts"][0]["metadata"]["residentSession"] == "true"
