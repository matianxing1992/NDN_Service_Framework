from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "specs/180-ack-driven-cross-model-qualification"
REFERENCE = FEATURE / "contracts/qwen-reference-manifest-v1.json"
TINY = ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1"


def _load_reference() -> dict:
    return json.loads(REFERENCE.read_text(encoding="utf-8"))


def test_qwen_reference_is_machine_bound_to_spec175_tiny_local_subject():
    manifest = _load_reference()
    assert manifest["schemaVersion"] == "spec180-qwen-reference-v1"
    assert manifest["status"] == "FROZEN_LOCAL_REFERENCE"
    assert manifest["sourceFeature"] == "175-ndnsf-di-streamed-invocation"
    assert manifest["sourceRevision"] == "286a0098b9bf0dfc2e0b77a320e9e75c38851dc7"
    assert (ROOT / manifest["sourceClosure"]).is_file()
    assert (ROOT / manifest["sourceEvidence"]).is_file()

    model = manifest["model"]
    tiny_manifest = TINY / "manifest.json"
    assert model["family"] == "spec175-tiny-causal-lm-v1"
    assert model["runtime"] == "onnxruntime-cpu"
    assert model["mtpEnabled"] is False
    assert model["thinkingMode"] == "disabled"
    assert model["fixtureRoot"] == "tests/fixtures/spec175/tiny-causal-lm-v1"
    assert model["manifestPath"] == "tests/fixtures/spec175/tiny-causal-lm-v1/manifest.json"
    digest = hashlib.sha256(tiny_manifest.read_bytes()).hexdigest()
    assert model["manifestSha256"] == "sha256:" + digest


def test_qwen_reference_freezes_controls_and_case_oracles():
    manifest = _load_reference()
    prompts = json.loads((TINY / "prompts.json").read_text(encoding="utf-8"))
    controls = manifest["controls"]
    assert controls == {
        "initialSvsSettleMs": 5000,
        "ackTimeoutMs": 1500,
        "requestTimeoutMs": 60000,
        "samplingMode": "greedy",
        "maxGeneratedTokens": 8,
        "conversationState": "provider-local",
    }
    assert manifest["cases"]["Q-C"] == {
        "sourceCase": "M01",
        "seed": 175021,
        "requestCount": 1,
        "expectedTokens": [4, 5, 6, 7, 8, 9, 10, 2],
        "terminalResponseCount": 1,
        "requiresContinuation": False,
    }
    assert prompts["expectedOutputs"]["normal"] == manifest["cases"]["Q-C"]["expectedTokens"]
    assert prompts["cases"]["normal"]["maxGeneratedTokens"] == controls["maxGeneratedTokens"]
    q_w = manifest["cases"]["Q-W"]
    assert q_w["sourceCase"] == "M11"
    assert q_w["requestCount"] == 2
    assert q_w["validTurns"] == 2
    assert q_w["mismatchNegativeAttempt"] is True
    assert q_w["terminalResponseCount"] == 2
    assert q_w["requiresContinuation"] is True


def test_qwen_reference_does_not_promote_tiger_27b_workload_to_local_q_cases():
    manifest = _load_reference()
    tiger_workload = json.loads(
        (ROOT / "packaging/ndnsf-di-container/jobs/spec175/workload.json").read_text(
            encoding="utf-8"
        )
    )
    assert manifest["evidencePolicy"]["qualifiesQwen27B"] is False
    assert manifest["evidencePolicy"]["qualifiesTiger"] is False
    assert manifest["evidencePolicy"]["inheritsSpec175Evidence"] is False
    assert tiger_workload["model"]["repository"] == "Qwen/Qwen3.6-27B"
    assert tiger_workload["generation"]["maxGeneratedTokens"] == 64
    assert tiger_workload["generation"]["requestDeadlineMs"] == 120000
    assert manifest["controls"]["maxGeneratedTokens"] != tiger_workload["generation"]["maxGeneratedTokens"]
    assert manifest["controls"]["requestTimeoutMs"] != tiger_workload["generation"]["requestDeadlineMs"]


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("sourceRevision", "wrong-revision"),
        ("controls", {"maxGeneratedTokens": 64}),
    ],
)
def test_qwen_reference_mutation_is_detectable(field: str, value,):
    manifest = _load_reference()
    manifest[field] = value
    assert manifest != _load_reference()
    if field == "sourceRevision":
        assert manifest["sourceRevision"] != "286a0098b9bf0dfc2e0b77a320e9e75c38851dc7"
    else:
        assert manifest["controls"]["maxGeneratedTokens"] == 64
