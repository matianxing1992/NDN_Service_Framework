from __future__ import annotations

import importlib.util
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


@pytest.mark.parametrize("ack_timeout_ms", [0, -1, 180_000, 900_000])
def test_qwen_profile_rejects_non_positive_or_deadline_sized_ack_window(ack_timeout_ms: int):
    module = load_module()
    with pytest.raises(ValueError, match="ACK window"):
        module.qwen_runtime_budgets(1024, 2048, ack_timeout_ms=ack_timeout_ms)
