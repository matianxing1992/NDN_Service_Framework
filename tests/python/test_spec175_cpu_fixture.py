from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys

import numpy as np
import onnx
import onnxruntime as ort
import pytest


REPO = Path(__file__).resolve().parents[2]
GENERATOR = REPO / "tests/fixtures/spec175/build_tiny_causal_onnx.py"
COMMITTED = REPO / "tests/fixtures/spec175/tiny-causal-lm-v1"


def _generate(output: Path) -> None:
    subprocess.run(
        [sys.executable, str(GENERATOR), "--output", str(output)],
        cwd=REPO,
        text=True,
        capture_output=True,
        check=True,
    )


def _tree_hashes(root: Path) -> dict[str, str]:
    return {
        str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def _sessions(partition: str) -> list[ort.InferenceSession]:
    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    options.inter_op_num_threads = 1
    return [
        ort.InferenceSession(
            str(path), sess_options=options, providers=["CPUExecutionProvider"]
        )
        for path in sorted((COMMITTED / partition).glob("role-*.onnx"))
    ]


def _initial_states(
    sessions: list[ort.InferenceSession],
) -> list[dict[str, np.ndarray]]:
    rows = []
    for session in sessions:
        row = {}
        for value in session.get_inputs():
            if value.name.endswith("_in") and value.name != "hidden_in":
                row[value.name] = np.zeros(tuple(value.shape), dtype=np.float32)
        rows.append(row)
    return rows


def _step(
    sessions: list[ort.InferenceSession],
    states: list[dict[str, np.ndarray]],
    token: int,
) -> tuple[int, list[dict[str, np.ndarray]]]:
    hidden = None
    logits = None
    next_states = []
    for role_index, session in enumerate(sessions):
        feed = dict(states[role_index])
        if role_index == 0:
            feed["input_ids"] = np.asarray([[token]], dtype=np.int64)
        else:
            assert hidden is not None
            feed["hidden_in"] = hidden
        names = [value.name for value in session.get_outputs()]
        output = dict(zip(names, session.run(names, feed)))
        hidden = output.get("hidden_out")
        logits = output.get("logits", logits)
        next_states.append({
            name.replace("_out", "_in"): value
            for name, value in output.items()
            if name.endswith("_out") and name != "hidden_out"
        })
    assert logits is not None
    return int(np.argmax(logits[0, -1])), next_states


def _generate_tokens(partition: str, case: str) -> list[int]:
    documents = json.loads((COMMITTED / "prompts.json").read_text(encoding="utf-8"))
    prompt = documents["cases"][case]
    sessions = _sessions(partition)
    states = _initial_states(sessions)
    token = int(prompt["inputIds"][-1])
    generated = []
    for _ in range(int(prompt["maxGeneratedTokens"])):
        token, states = _step(sessions, states, token)
        generated.append(token)
        if token == 2:
            break
    return generated


def test_tiny_causal_fixture_is_byte_reproducible_and_frozen(
    tmp_path: Path,
) -> None:
    first = tmp_path / "first"
    second = tmp_path / "second"

    _generate(first)
    _generate(second)

    first_hashes = _tree_hashes(first)
    assert first_hashes == _tree_hashes(second)
    assert first_hashes == _tree_hashes(COMMITTED)
    manifest = json.loads((first / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["schemaVersion"] == "spec175-tiny-causal-lm-v1"
    assert manifest["vocabularySize"] == 32
    assert manifest["hiddenSize"] == 8
    assert manifest["blockCount"] == 4
    assert sorted(manifest["partitions"]) == ["four-role", "one-role", "two-role"]
    assert manifest["expectedOutputs"] == {
        "early-eos": [14, 15, 2],
        "max-eight": [20, 21, 22, 23, 24, 25, 26, 27],
        "normal": [4, 5, 6, 7, 8, 9, 10, 2],
    }


@pytest.mark.parametrize("partition", ["one-role", "two-role", "four-role"])
def test_real_ort_prefill_and_incremental_decode_matches_oracle(
    partition: str,
) -> None:
    manifest = json.loads((COMMITTED / "manifest.json").read_text(encoding="utf-8"))
    roles = manifest["partitions"][partition]
    assert roles[0]["blockStart"] == 0
    assert roles[-1]["blockEndExclusive"] == 4
    assert all(
        left["blockEndExclusive"] == right["blockStart"]
        for left, right in zip(roles, roles[1:])
    )
    for role in roles:
        model = onnx.load(str(COMMITTED / role["path"]))
        onnx.checker.check_model(model, full_check=True)
        inputs = {value.name for value in model.graph.input}
        assert {
            "attention_kv_in",
            "recurrent_state_in",
            "convolution_state_in",
        } <= inputs

    assert _generate_tokens(partition, "normal") == manifest["expectedOutputs"]["normal"]
    assert _generate_tokens(partition, "early-eos") == manifest["expectedOutputs"]["early-eos"]
    assert _generate_tokens(partition, "max-eight") == manifest["expectedOutputs"]["max-eight"]


def test_next_token_requires_both_decode_state_families() -> None:
    sessions = _sessions("one-role")
    states = _initial_states(sessions)
    first, states = _step(sessions, states, 3)
    assert first == 4

    correct, _ = _step(sessions, states, first)
    assert correct == 5

    missing_attention = [{**states[0], "attention_kv_in": np.zeros_like(states[0]["attention_kv_in"])}]
    without_attention, _ = _step(sessions, missing_attention, first)
    assert without_attention != correct

    missing_linear = [{
        **states[0],
        "recurrent_state_in": np.zeros_like(states[0]["recurrent_state_in"]),
        "convolution_state_in": np.zeros_like(states[0]["convolution_state_in"]),
    }]
    without_linear, _ = _step(sessions, missing_linear, first)
    assert without_linear != correct

    incomplete_feed = dict(states[0])
    del incomplete_feed["attention_kv_in"]
    incomplete_feed["input_ids"] = np.asarray([[first]], dtype=np.int64)
    with pytest.raises(ValueError, match="attention_kv_in"):
        sessions[0].run(None, incomplete_feed)
