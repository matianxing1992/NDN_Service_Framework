from __future__ import annotations

import importlib.util
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py"


def load_module():
    spec = importlib.util.spec_from_file_location(
        "spec190_qwen_native_minindn_timing_parser", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_phase_timing_summary_keeps_marker_before_trailing_token_text():
    module = load_module()
    text = "\n".join((
        "NDNSF_PHASE_TIMING requestId=/r-0 phase=tokenReceived "
        "steady_us=390 timestamp_us=390000 tokenIndex=3",
        "NDNSF_PHASE_TIMING requestId=/r-0 phase=tokenReceived "
        "steady_us=400 timestamp_us=400000 tokenIndex=4 can",
    ))

    summary = module.phase_timing_summary(text, request_id="/r-0")

    assert summary["recordCount"] == 2
    assert summary["requests"]["/r-0"]["phaseTimesUs"]["tokenReceived"] == [390, 400]
    assert summary["requests"]["/r-0"]["tokenReceiveIntervalsUs"] == [10]


def test_phase_timing_summary_still_ignores_incomplete_marker():
    module = load_module()
    text = (
        "NDNSF_PHASE_TIMING requestId=/r-0 phase=tokenReceived "
        "steady_us=400 tokenIndex=4 can"
    )

    summary = module.phase_timing_summary(text, request_id="/r-0")

    assert summary == {"recordCount": 0, "requests": {}}


def test_structured_marker_still_rejects_duplicate_fields():
    module = load_module()
    line = (
        "NDNSF_PHASE_TIMING requestId=/r-0 phase=tokenReceived "
        "phase=terminal steady_us=400 timestamp_us=400000"
    )

    assert module._structured_marker_fields(line, "NDNSF_PHASE_TIMING") is None
