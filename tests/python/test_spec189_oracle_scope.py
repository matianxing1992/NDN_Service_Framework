"""Orchestration-only checks; the C++ CLI fixture owns oracle assertions."""
from pathlib import Path
from types import SimpleNamespace
import importlib.util
import sys

import pytest


def load_module():
    path = Path(__file__).resolve().parents[2] / "Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py"
    spec = importlib.util.spec_from_file_location("spec189_oracle_scope", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("cache", [False, True])
@pytest.mark.parametrize("multi", [False, True])
@pytest.mark.parametrize("rounds", [1, 3])
def test_oracle_scope_is_explicit_and_preserved(monkeypatch, tmp_path, cache, multi, rounds):
    module = load_module()
    marker = "SPEC189_CPP_CACHE_DIAGNOSTIC_PASS" if cache else "SPEC189_CPP_ORACLE_PASS"
    def run(arguments, **kwargs):
        assert arguments == (["/installed/oracle"] +
                             (["--cache-compatibility"] if cache else []) +
                             (["--require-multi-token"] if multi else []) +
                             (["--rounds", str(rounds)] if rounds != 1 else []) +
                             ["--run-root", str(tmp_path)])
        assert kwargs["timeout"] == 30
        kwargs["stdout"].write(marker + " {}\n")
        return SimpleNamespace(returncode=0)
    monkeypatch.setattr(module.subprocess, "run", run)
    result = module.run_cpp_oracle(Path("/installed/oracle"), tmp_path, cache, 30, multi, rounds)
    assert result["scope"] == ("cache-compatible-execution" if cache else "full-path")
    assert result["status"] == ("CACHE_DIAGNOSTIC_PASS" if cache else "PASS")


def test_continuation_paths_form_chain_without_overwriting_parent():
    module = load_module()
    first = {"request": {"options_file": "options.json"}, "conversation": {
        "turn": {"mode": "FULL_CONTEXT"}, "checkpoint_output_file": "conversation-state.json",
        "journal": {"state_root": "conversation-state"}}}
    parent = "conversation-state.json"
    for index in range(1, 8):
        cfg = module.continuation_config(first, index, [10, 20])
        assert cfg["conversation"]["turn"]["parent_state_file"] == parent
        assert cfg["conversation"]["turn"]["mode"] == "APPEND_DELTA"
        assert cfg["conversation"]["turn"]["delta_token_ids"] == [10, 20]
        assert cfg["request"]["options_file"] == f"options-{index}.json"
        successor = cfg["conversation"]["checkpoint_output_file"]
        assert successor == f"conversation-state-{index}.json" and successor != parent
        assert cfg["conversation"]["journal"] == first["conversation"]["journal"]
        parent = successor
    assert first["conversation"]["turn"] == {"mode": "FULL_CONTEXT"}
    assert first["request"]["options_file"] == "options.json"
    for invalid in [0, 8, -1]:
        with pytest.raises(ValueError):
            module.continuation_config(first, invalid, [10])


def test_continuation_preserves_complete_model_generation_contract():
    module = load_module()
    first = {"generationId": "initial", "sampling": {"mode": "Greedy", "seed": 18406},
             "stateInputNames": ["past_key_values.0.key", "past_key_values.0.value"],
             "stateOutputNames": ["present.0.key", "present.0.value"],
             "stateSuccessorMap": "past_key_values.0.key=present.0.key,past_key_values.0.value=present.0.value",
             "positionInputPolicy": "qwen-causal-position-v1", "positionIdsInputName": "position_ids",
             "attentionMaskInputName": "attention_mask", "maxNewTokens": 1024,
             "eosTokenIds": [151645], "tokenizerDigest": "sha256:tokenizer",
             "futureModelField": {"must_survive": [1, 2]}}
    for index in [1, 2, 7]:
        later = module.continuation_options(first, index)
        assert later["generationId"] == f"{index:032x}"
        assert later["sampling"]["seed"] == 18406 + index
        later["generationId"] = "initial"
        later["sampling"]["seed"] = 18406
        assert later == first
        later["stateInputNames"].append("mutation")
        assert len(first["stateInputNames"]) == 2
    for invalid in [0, 8, -1]:
        with pytest.raises(ValueError):
            module.continuation_options(first, invalid)


@pytest.mark.parametrize("cache", [False, True])
@pytest.mark.parametrize("failure", ["wrong-scope", "exit", "embedded"])
def test_oracle_cannot_accept_wrong_scope_or_failed_process(monkeypatch, tmp_path, cache, failure):
    module = load_module()
    correct = "SPEC189_CPP_CACHE_DIAGNOSTIC_PASS" if cache else "SPEC189_CPP_ORACLE_PASS"
    other = "SPEC189_CPP_ORACLE_PASS" if cache else "SPEC189_CPP_CACHE_DIAGNOSTIC_PASS"
    def run(arguments, **kwargs):
        marker = other if failure == "wrong-scope" else correct
        kwargs["stdout"].write(("error mentioning " if failure == "embedded" else "") + marker + " {}\n")
        return SimpleNamespace(returncode=1 if failure == "exit" else 0)
    monkeypatch.setattr(module.subprocess, "run", run)
    with pytest.raises(RuntimeError, match="SPEC189_CPP_ORACLE_FAIL"):
        module.run_cpp_oracle(Path("/installed/oracle"), tmp_path, cache, 30)
