from __future__ import annotations

import json
import importlib.util
from pathlib import Path

import onnxruntime as ort

ROOT = Path(__file__).resolve().parents[2]
LIB = ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline"
import sys
sys.path.insert(0, str(LIB))

from llm_pipeline_lib import (  # noqa: E402
    TINY_ONNX_RUNTIME,
    encode_qwen_pipeline_delta,
    encode_qwen_pipeline_context,
    role_name,
    run_tiny_onnx_stage,
)


FIXTURE = ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1"


def test_tiny_onnx_stage_runner_preserves_state_and_tokens() -> None:
    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    options.inter_op_num_threads = 1
    sessions = [
        ort.InferenceSession(
            str(FIXTURE / "four-role" / f"role-{index}.onnx"),
            sess_options=options,
            providers=["CPUExecutionProvider"],
        )
        for index in range(4)
    ]
    state_cache = {}
    generated = []
    token = 3
    for epoch in range(8):
        payload = encode_qwen_pipeline_context(
            [[token]],
            attention_mask=[[1]],
            request_id="spec175-tiny-runtime",
            context_epoch=epoch,
            generation={
                "outputMode": "FULL",
                "useCache": True,
                "maxNewTokens": 8,
                "eosTokenIds": [2],
            },
        )
        for index, session in enumerate(sessions):
            payload = run_tiny_onnx_stage(
                payload,
                role=role_name(index),
                stages=4,
                session=session,
                state_cache=state_cache,
                request_id="spec175-tiny-runtime",
            )
        result = json.loads(payload.decode("utf-8"))
        assert result["runtime"] == TINY_ONNX_RUNTIME
        token = int(result["topToken"])
        generated.append(token)
        if token == 2:
            break
    assert generated == [4, 5, 6, 7, 8, 9, 10, 2]
    assert len(state_cache) == 4


def test_conversation_commit_advances_provider_context_epoch_without_duplication():
    """A committed turn must make the next APPEND_DELTA expandable."""
    provider_path = (
        ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline/provider.py"
    )
    spec = importlib.util.spec_from_file_location(
        "spec175_provider_context_cache", provider_path)
    assert spec is not None and spec.loader is not None
    provider = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(provider)

    session_id = "spec175-cache-regression"
    full = encode_qwen_pipeline_context(
        [[3]], attention_mask=[[1]], request_id="/first",
        session_id=session_id, context_epoch=0,
        generation={"outputMode": "FULL", "useCache": True,
                    "maxNewTokens": 2, "eosTokenIds": [2]},
    )
    with provider._QWEN_CONTEXT_CACHE_LOCK:
        provider._QWEN_CONTEXT_CACHE.clear()
    provider._qwen_context_cache_after_commit(
        full, prefix=(3, 4, 5), context_epoch=1)
    with provider._QWEN_CONTEXT_CACHE_LOCK:
        cached = dict(provider._QWEN_CONTEXT_CACHE[session_id])
    assert cached["contextEpoch"] == 1
    assert cached["inputIds"] == [[3, 4, 5]]

    delta = encode_qwen_pipeline_delta(
        [[6]], request_id="/second", session_id=session_id,
        base_context_epoch=1, context_epoch=2)
    expanded = provider._resolve_qwen_context_request(delta)
    expanded_doc = provider.decode_qwen_pipeline_context(expanded)
    assert expanded_doc["contextEpoch"] == 2
    assert expanded_doc["inputIds"] == [[3, 4, 5, 6]]
