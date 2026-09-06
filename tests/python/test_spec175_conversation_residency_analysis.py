from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts/analyze_spec175_conversation_residency.py"
SPEC = importlib.util.spec_from_file_location(
    "spec175_conversation_residency_analysis", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
ANALYZER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYZER)

ROLES = (
    "/LLM/Pipeline/Stage/0",
    "/LLM/Pipeline/Stage/1",
    "/LLM/Pipeline/Stage/2",
)


def _digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _result(tokens: list[int]) -> dict:
    return {
        "status": "OK",
        "stopReason": "MAX_NEW_TOKENS",
        "exactReferenceMatch": True,
        "tokenCount": len(tokens),
        "tokenSha256": ANALYZER.token_digest(tokens),
        "responseBytes": 4,
        "responseSha256": "sha256:" + "a" * 64,
        "totalMs": 10.0,
    }


def _fixture(tmp_path: Path) -> tuple[Path, Path]:
    bundle = tmp_path / "bundle"
    evidence = tmp_path / "evidence"
    bundle.mkdir()
    evidence.mkdir()
    def turn(first: bool, token: int) -> dict:
        return {
            ("inputTokenIds" if first else "appendedInputTokenIds"): [token],
            "referenceGeneratedTokenIds": [1000 + token],
            "eosTokenIds": [2],
        }

    oracle = {
        "schemaVersion": "ndnsf-di-spec175-g6c-oracle-v1",
        "turnPairs": [
            {"caseId": "two-turn", "conversationId": "g6c-two-turn-0001",
             "firstTurn": turn(True, 10), "secondTurn": turn(False, 11)},
            {"caseId": "paused-pressure",
             "conversationId": "g6c-pressure-0001",
             "firstTurn": turn(True, 20), "secondTurn": turn(False, 21)},
        ],
        "unavailableRole": {
            "conversationId": "g6c-unavailable-0001",
            "role": ROLES[1],
            "firstTurn": turn(True, 30),
            "secondTurn": turn(False, 31),
            "fallback": {
                "referenceGeneratedTokenIds": [1031],
                "fullInputTokenIds": [30, 1030, 31], "eosTokenIds": [2]},
        },
    }
    oracle_path = bundle / "g6c-oracle.json"
    _write_json(oracle_path, oracle)
    campaign = {
        "schemaVersion":
            "ndnsf-di-spec175-conversation-residency-campaign-v1",
        "campaignId": "candidate-r1-g6c",
        "conversationOracleSha256": _digest(oracle_path),
        "turnPairs": oracle["turnPairs"],
        "unavailableRole": oracle["unavailableRole"],
    }
    campaign_path = bundle / "process-groups/g6c/conversation-campaign.json"
    _write_json(campaign_path, campaign)
    _write_json(bundle / "spec175-functional-manifest.json", {
        "schemaVersion": "ndnsf-di-spec175-functional-bundle-v2",
        "gate": "conversation-residency",
        "candidateId": "candidate-r1",
        "conversationOracle": {
            "path": "g6c-oracle.json", "sha256": _digest(oracle_path)},
        "processGroups": [{
            "id": "g6c",
            "conversationCampaign": {
                "path": "process-groups/g6c/conversation-campaign.json",
                "sha256": _digest(campaign_path),
            },
        }],
    })
    first_digest = "sha256:" + "1" * 64
    successor_digest = "sha256:" + "2" * 64
    _write_json(evidence / "conversation-residency.json", {
        "schemaVersion": "ndnsf-di-spec175-g6c-evidence-v1",
        "campaignId": "candidate-r1-g6c",
        "status": "PASS",
        "turnPairs": [
            {
                "caseId": "two-turn", "conversationId": "g6c-two-turn-0001",
                "firstRequestId": "two-first", "secondRequestId": "two-second",
                "firstGenerationId": "two-first-generation",
                "secondGenerationId": "two-second-generation",
                "firstCheckpointEpoch": 1, "successorCheckpointEpoch": 2,
                "firstCheckpointDigest": first_digest,
                "successorCheckpointDigest": successor_digest,
                "deltaPrefillInputTokens": 1,
                "firstResult": _result([1010]), "secondResult": _result([1011]),
            },
            {
                "caseId": "paused-pressure",
                "conversationId": "g6c-pressure-0001",
                "firstRequestId": "pressure-first",
                "secondRequestId": "pressure-second",
                "firstGenerationId": "pressure-first-generation",
                "secondGenerationId": "pressure-second-generation",
                "firstCheckpointEpoch": 1, "successorCheckpointEpoch": 2,
                "firstCheckpointDigest": first_digest,
                "successorCheckpointDigest": successor_digest,
                "deltaPrefillInputTokens": 1,
                "firstResult": _result([1020]), "secondResult": _result([1021]),
            },
        ],
        "unavailableRole": {
            "role": ROLES[1],
            "conversationId": "g6c-unavailable-0001",
            "firstRequestId": "unavailable-first",
            "firstGenerationId": "unavailable-first-generation",
            "firstResult": _result([1030]),
            "negativeRequestId": "unavailable-negative",
            "negativeGenerationId": "unavailable-negative-generation",
            "negativeError": "CONVERSATION_STATE_UNAVAILABLE",
            "fallbackAuthorized": True,
            "fallbackMode": "FULL_CONTEXT",
            "fallbackRequestId": "unavailable-fallback",
            "fallbackGenerationId": "unavailable-fallback-generation",
            "fallbackResult": _result([1031]),
            "checkpointPreservedAtEpoch": 1,
            "checkpointDigest": first_digest,
        },
        "stateTensorBytesOnNdn": 0,
        "cpuModelFallbackCount": 0,
        "incompatibleRunnerCalls": 0,
    })
    for index, role in enumerate(ROLES):
        lines = [
            "LLM_PIPELINE_QWEN_ONNX_STAGE_ARTIFACT_READY "
            f"role={role} device=cuda:0 cpuFallback=false",
            "LLM_PIPELINE_QWEN_STATE_RESIDENCY "
            "storage=device hostRoundTripBytes=0",
            "LLM_PIPELINE_CONVERSATION_HOST_PAUSED "
            "conversationId=g6c-pressure-0001 transferBytes=4096 "
            "transferLatencyMs=1.25",
            "LLM_PIPELINE_CONVERSATION_PREFETCHED "
            "requestId=pressure-second conversationId=g6c-pressure-0001 "
            "transferBytes=4096 "
            "transferLatencyMs=0.75",
            "LLM_PIPELINE_CONVERSATION_PREPARED "
            "requestId=pressure-second mode=append-delta bound=true",
        ]
        if role == ROLES[1]:
            lines.append(
                "LLM_PIPELINE_CONVERSATION_STATE_INVALIDATED "
                "conversationId=g6c-unavailable-0001 invalidatedEntries=1")
        (evidence / f"provider-{index}.log").write_text(
            "\n".join(lines) + "\n", encoding="utf-8")
    return bundle, evidence


def test_g6c_analyzer_accepts_all_role_cuda_host_round_trip(tmp_path: Path):
    bundle, evidence = _fixture(tmp_path)
    result = ANALYZER.analyze(evidence, bundle)
    assert result["status"] == "PASS", result["errors"]
    assert set(result["roleRecords"]) == set(ROLES)


def test_g6c_analyzer_rejects_transfer_byte_mismatch(tmp_path: Path):
    bundle, evidence = _fixture(tmp_path)
    path = evidence / "provider-0.log"
    path.write_text(
        path.read_text(encoding="utf-8").replace(
            "PREFETCHED requestId=pressure-second "
            "conversationId=g6c-pressure-0001 transferBytes=4096",
            "PREFETCHED requestId=pressure-second "
            "conversationId=g6c-pressure-0001 transferBytes=2048"),
        encoding="utf-8")
    result = ANALYZER.analyze(evidence, bundle)
    assert result["status"] == "FAIL"
    assert any("byte accounting mismatch" in item for item in result["errors"])


def test_g6c_analyzer_rejects_wrong_invalidation_role(tmp_path: Path):
    bundle, evidence = _fixture(tmp_path)
    marker = (
        "LLM_PIPELINE_CONVERSATION_STATE_INVALIDATED "
        "conversationId=g6c-unavailable-0001 invalidatedEntries=1\n")
    path = evidence / "provider-0.log"
    path.write_text(path.read_text(encoding="utf-8") + marker, encoding="utf-8")
    result = ANALYZER.analyze(evidence, bundle)
    assert result["status"] == "FAIL"
    assert any("registered unavailable role" in item
               for item in result["errors"])


def test_g6c_analyzer_rejects_prefetch_before_host_residency(tmp_path: Path):
    bundle, evidence = _fixture(tmp_path)
    path = evidence / "provider-0.log"
    rows = path.read_text(encoding="utf-8").splitlines()
    rows[2], rows[3] = rows[3], rows[2]
    path.write_text("\n".join(rows) + "\n", encoding="utf-8")
    result = ANALYZER.analyze(evidence, bundle)
    assert result["status"] == "FAIL"
    assert any("PREFETCH preceded HOST" in item for item in result["errors"])


def test_g6c_analyzer_rejects_cpu_model_fallback(tmp_path: Path):
    bundle, evidence = _fixture(tmp_path)
    path = evidence / "provider-2.log"
    path.write_text(
        path.read_text(encoding="utf-8")
        + "LLM_PIPELINE_QWEN_STATE cpuFallback=true\n",
        encoding="utf-8")
    result = ANALYZER.analyze(evidence, bundle)
    assert result["status"] == "FAIL"
    assert any("CPU model-compute fallback" in item
               for item in result["errors"])
