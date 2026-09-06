from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts/analyze_spec175_performance.py"
SPEC = importlib.util.spec_from_file_location("spec175_performance", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
ANALYZER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYZER)

ROLES = (
    "/LLM/Pipeline/Stage/0",
    "/LLM/Pipeline/Stage/1",
    "/LLM/Pipeline/Stage/2",
)


def _write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value) + "\n", encoding="utf-8")


def _digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _record(root: Path, relative: str) -> dict[str, str]:
    return {"path": relative, "sha256": _digest(root / relative)}


def _fixture(tmp_path: Path, interval_ms: float = 40.0):
    bundle = tmp_path / "bundle"
    output = tmp_path / "output"
    bundle.mkdir()
    output.mkdir()
    groups = []
    expected = [7, 8, 9, 2]
    for process in range(3):
        group_id = f"performance-group-{process:02d}"
        relative = f"process-groups/{group_id}"
        schedule = [{"promptId": "P1", "phase": "cold", "repetition": 0}]
        schedule.extend({
            "promptId": "P1" if index % 2 == 0 else "P2",
            "phase": "measured", "repetition": index // 2,
        } for index in range(10))
        campaign = {
            "schemaVersion": "ndnsf-di-qwen-generation-campaign-v2",
            "campaignId": f"candidate-r1-{group_id}",
            "prompts": [
                {"promptId": prompt, "referenceGeneratedTokenIds": expected}
                for prompt in ("P1", "P2")],
            "schedule": schedule,
        }
        campaign_path = bundle / relative / "campaign.json"
        _write_json(campaign_path, campaign)
        user_path = bundle / relative / "user.args"
        user_path.write_text(
            "python3\nuser.py\n--request-id\n"
            f"/candidate-r1/{group_id}\n", encoding="utf-8")
        groups.append({
            "id": group_id,
            "generationCampaign": _record(bundle, f"{relative}/campaign.json"),
            "userArgs": _record(bundle, f"{relative}/user.args"),
        })
        group_output = output / "process-groups" / group_id
        group_output.mkdir(parents=True)
        rows = []
        request_ids = []
        for sample in schedule:
            marker = "c" if sample["phase"] == "cold" else "m"
            generation_id = (
                f"{campaign['campaignId']}-{sample['promptId']}-"
                f"{marker}{sample['repetition']}")
            rows.append({
                "schemaVersion": "ndnsf-di-qwen-generation-sample-v1",
                "campaignId": campaign["campaignId"],
                "promptId": sample["promptId"],
                "phase": sample["phase"],
                "repetition": sample["repetition"],
                "generationId": generation_id,
                "status": "OK", "exactReferenceMatch": True,
                "generatedTokenIds": expected,
                "ttftMs": 100.0,
                "interTokenMs": [interval_ms, interval_ms, interval_ms],
                "totalMs": 250.0,
                "tokenSteps": [{"mode": "FULL", "metadata": {
                    "timingSummary": {
                        "eventCount": len(expected), "attemptCount": 1,
                        "replacementCount": 0, "staleEventCount": 0,
                        "lineageRejectCount": 0, "gapRejectCount": 0,
                        "duplicateRejectCount": 0, "callbackErrorCount": 0,
                    }}}],
            })
            if sample["phase"] == "measured":
                request_ids.append(ANALYZER.canonical_request_id(
                    f"/candidate-r1/{group_id}", generation_id))
        (group_output / "generation.jsonl").write_text(
            "".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
        for role_index, role in enumerate(ROLES):
            lines = []
            for request_id in request_ids:
                for epoch in range(len(expected)):
                    lines.extend((
                        "LLM_PIPELINE_QWEN_STAGE_TIMING "
                        f"role={role} requestId={request_id} epoch={epoch} "
                        f"phase={'prefill' if epoch == 0 else 'decode'} "
                        "compute_ms=10 runner_total_ms=11 decode_ms=1 "
                        "serialize_ms=1 cpuFallback=0 state_storage=device "
                        "state_host_round_trip_bytes=0 "
                        f"prefix_token_count={10 + epoch}",
                        "LLM_PIPELINE_QWEN_EPOCH_DATAFLOW_TIMING "
                        f"role={role} requestId={request_id} epoch={epoch} "
                        "activationFetchMs=2 activationPublishMs=1 "
                        "feedbackWaitMs=3 feedbackPublishMs=1 samplingMs=1 "
                        "eventPublishMs=1",
                        "LLM_PIPELINE_QWEN_STATE_RESIDENCY "
                        f"requestId={request_id} role={role} epoch={epoch} "
                        "storage=device hostRoundTripBytes=0",
                    ))
                lines.extend((
                    "NDNSF_DI_PROVIDER_HANDLER_TIMING event=end "
                    f"session={request_id} role={role} queue_wait_ms=1",
                    "LLM_PIPELINE_QWEN_REQUEST_STATE_CLEANUP "
                    f"requestId={request_id} role={role} "
                    f"generationEpochs={len(expected)} releasedStateBytes=1024 "
                    "remainingRequestStates=0 managerRequestLocalEntries=0 "
                    "managerConversationEntries=0",
                ))
            (group_output / f"provider-{role_index}.log").write_text(
                "\n".join(lines) + "\n", encoding="utf-8")
        resources = []
        for sample_index in range(2):
            resources.append({
                "schemaVersion": "ndnsf-di-spec175-resource-sample-v1",
                "gpuError": "", "networkError": "", "rssBytes": 1000,
                "monotonicMs": sample_index * 500.0,
                "cpuTimeNs": sample_index * 250_000_000,
                "processCount": 8,
                "networkRxBytes": 100 + sample_index * 10,
                "networkTxBytes": 200 + sample_index * 20,
                "gpus": [
                    {"uuid": f"GPU-{gpu}", "utilizationPercent": 75.0,
                     "memoryUsedMiB": 20000.0, "powerDrawW": 175.0}
                    for gpu in range(3)],
            })
        (group_output / "resource-samples.jsonl").write_text(
            "".join(json.dumps(row) + "\n" for row in resources),
            encoding="utf-8")
    _write_json(bundle / "spec175-functional-manifest.json", {
        "schemaVersion": "ndnsf-di-spec175-functional-bundle-v2",
        "gate": "performance", "candidateId": "candidate-r1",
        "processGroups": groups,
    })
    prerequisites = []
    for gate in ("G5", "G6", "G6C"):
        path = tmp_path / f"{gate.lower()}-prerequisite.json"
        _write_json(path, {
            "schemaVersion": "ndnsf-di-spec175-gate-prerequisite-v1",
            "gate": gate, "status": "PASS", "candidateId": "candidate-r1",
            "sourceSealSha256": "sha256:" + "1" * 64,
            "sifSha256": "sha256:" + "2" * 64,
            "workloadSha256": "sha256:" + "3" * 64,
            "modelManifestSha256": "sha256:" + "4" * 64,
            "evidenceSha256": "sha256:" + "5" * 64,
        })
        prerequisites.append(path)
    return bundle, output, prerequisites


def test_g7_analyzer_reports_registered_performance_pass(tmp_path: Path):
    bundle, output, prerequisites = _fixture(tmp_path, interval_ms=40.0)
    result = ANALYZER.analyze(output, bundle, prerequisites)
    assert result["status"] == "PASS", result["errors"]
    assert result["verdict"] == "PERFORMANCE_PASS"
    assert result["counts"] == {
        "coldExcluded": 3, "warmMeasured": 30, "failures": 0}
    assert result["steadyStateTokensPerSecond"]["median"] == 25.0
    assert result["bootstrapMedianTokensPerSecond95Ci"]["seed"] == 1750003


def test_g7_analyzer_keeps_functional_pass_when_threshold_missed(tmp_path: Path):
    bundle, output, prerequisites = _fixture(tmp_path, interval_ms=60.0)
    result = ANALYZER.analyze(output, bundle, prerequisites)
    assert result["status"] == "PASS", result["errors"]
    assert result["verdict"] == "FUNCTIONAL_PASS_PERFORMANCE_MISS"
    assert result["counts"]["warmMeasured"] == 30


def test_g7_analyzer_rejects_missing_unfiltered_warm_unit(tmp_path: Path):
    bundle, output, prerequisites = _fixture(tmp_path)
    path = output / "process-groups/performance-group-01/generation.jsonl"
    lines = path.read_text(encoding="utf-8").splitlines()
    path.write_text("\n".join(lines[:-1]) + "\n", encoding="utf-8")
    result = ANALYZER.analyze(output, bundle, prerequisites)
    assert result["status"] == "FAIL"
    assert result["verdict"] == "FAIL"
    assert any("expected 11 unfiltered samples" in error
               for error in result["errors"])


def test_g7_analyzer_rejects_mixed_candidate_prerequisite(tmp_path: Path):
    bundle, output, prerequisites = _fixture(tmp_path)
    value = json.loads(prerequisites[-1].read_text(encoding="utf-8"))
    value["sifSha256"] = "sha256:" + "9" * 64
    _write_json(prerequisites[-1], value)
    result = ANALYZER.analyze(output, bundle, prerequisites)
    assert result["status"] == "FAIL"
    assert any("different subjects" in error for error in result["errors"])
