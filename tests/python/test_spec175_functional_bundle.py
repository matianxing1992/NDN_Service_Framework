from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
PREFLIGHT = ROOT / "packaging/ndnsf-di-container/bin/ndnsf-di-spec175-functional-preflight"
BUILDER = ROOT / "scripts/build_spec175_functional_bundle.py"
G6C_ANALYZER = ROOT / "scripts/analyze_spec175_conversation_residency.py"
G7_ANALYZER = ROOT / "scripts/analyze_spec175_performance.py"
RESOURCE_COLLECTOR = ROOT / "scripts/collect_spec175_resources.py"
ROLES = (
    "/LLM/Pipeline/Stage/0",
    "/LLM/Pipeline/Stage/1",
    "/LLM/Pipeline/Stage/2",
)
WORKLOAD = ROOT / "packaging/ndnsf-di-container/jobs/spec175/workload.json"


def digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def record(bundle: Path, relative: str) -> dict[str, str]:
    return {"path": relative, "sha256": digest(bundle / relative)}


def first_user_args(bundle: Path) -> Path:
    return sorted((bundle / "process-groups").glob("*/user.args"))[0]


def load_bundle_builder():
    spec = importlib.util.spec_from_file_location("spec175_bundle_builder", BUILDER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def complete_planning_fixture() -> tuple[dict, dict]:
    """Return a planning manifest and its stage input for boundary tests."""
    source_root = str(ROOT / "NDNSF-DistributedInference")
    if source_root not in sys.path:
        sys.path.insert(0, source_root)
    from ndnsf_distributed_inference.adapters.qwen import (
        build_qwen_three_stage_adapter,
    )

    model = "Qwen/Qwen3.6-27B"
    revision = "6a9e13bd6fc8f0983b9b99948120bc37f49c13e9"
    layer_ranges = ((0, 21), (21, 42), (42, 64))
    artifact_digests = {
        role: "sha256:" + f"{index + 1:064x}"
        for index, role in enumerate(ROLES)
    }
    weight_bytes = {role: 1000 + index for index, role in enumerate(ROLES)}
    adapter = build_qwen_three_stage_adapter(
        model_name=model,
        revision=revision,
        layer_ranges=layer_ranges,
        artifact_digests_by_role=artifact_digests,
        weight_bytes_by_role=weight_bytes,
        precision="float16",
    )
    described = adapter.describe_model(
        model,
        "sha256:" + "a" * 64,
        "sha256:" + "b" * 64,
        source_revision=revision,
    )
    graph = adapter.graph.inspect(described)
    candidate = adapter.splitter.enumerate_candidates(described, graph)[0]
    stage = {
        "repository": model,
        "revision": revision,
        "modelDigest": described.content_digest,
        "dtype": "float16",
        "quantization": "none",
        "layerRanges": [list(item) for item in layer_ranges],
        "tokenizer": {"digest": "sha256:" + "c" * 64},
        "stages": [
            {"role": role, "bytes": weight_bytes[role],
             "sha256": artifact_digests[role]}
            for role in ROLES
        ],
    }
    planning = {
        "schemaVersion": "ndnsf-di-spec162-automatic-planning-v1",
        "model": {
            "name": model,
            "revision": revision,
            "contentDigest": described.content_digest,
            "semanticsDigest": described.semantics_digest,
        },
        "dtype": "float16",
        "precision": "float16",
        "layerRanges": [list(item) for item in layer_ranges],
        "stages": stage["stages"],
        "adapterDescriptorDigest": adapter.descriptor.descriptor_digest,
        "adapterCompositionDigest": adapter.composition_digest,
        "graphDigest": graph.graph_digest,
        "candidateDigest": candidate.candidate_digest,
        "preSplitCatalog": {
            "candidateDigest": candidate.candidate_digest,
        },
    }
    return stage, planning


def test_bundle_builder_rejects_stale_planning_candidate_digest(tmp_path: Path) -> None:
    builder = load_bundle_builder()
    stage, planning = complete_planning_fixture()
    planning["candidateDigest"] = "sha256:" + "0" * 64
    planning["preSplitCatalog"]["candidateDigest"] = planning["candidateDigest"]
    with pytest.raises(SystemExit, match="candidate digest"):
        builder.validate_automatic_planning_manifest(stage, planning)


def make_bundle(root: Path, gate: str = "multi-provider") -> Path:
    bundle = root / "bundle"
    (bundle / "providers").mkdir(parents=True)
    (bundle / "nfd.conf").write_text("face_system { };\n", encoding="utf-8")
    (bundle / "controller-wrapper.sh").write_text(
        "from pathlib import Path\n"
        "Path('/evidence/app-envelope.key').write_bytes(b'x' * 32)\n"
        "controller.start()\n"
        "import time\n"
        "print('NDNSF_DI_CONTROLLER_READY', flush=True)\n"
        "while True:\n"
        "    time.sleep(1)\n",
        encoding="utf-8",
    )
    (bundle / "controller.args").write_text(
        "bash\n-lc\nexec /bundle/controller-wrapper.sh\n",
        encoding="utf-8",
    )
    for index, role in enumerate(ROLES):
        (bundle / "providers" / f"provider-{index}.args").write_text(
            f"python3\nprovider.py\n--runtime\nqwen-onnx\n"
            f"--roles\n{role}\n--device\ncuda:{index}\n"
            "--qwen-stage-manifest\n/bundle/stage-manifest.json\n"
            f"--selection-local-artifact\n{role}=/model/qwen-onnx-stage-artifacts/stage-{index}.onnx\n"
            "--require-cuda\n--require-onnx-runtime\n"
            "--selection-dataflow-v3\n"
            "--selection-model-type\nqwen3_5\n"
            "--selection-gpu-capacity-mib\n24576\n"
            "--selection-offered-gpu-mib\n2254\n"
            "--selection-offer-lease-ms\n7200000\n"
            "--selection-max-prepare-ms\n600000\n"
            "--selection-residency-ttl-ms\n7200000\n"
            f"--selection-wal-path\n/evidence/selection-{index}.wal\n"
            f"--selection-storage-key-file\n/evidence/selection-storage-{index}.key\n"
            f"--selection-signing-key-file\n/evidence/selection-signing-{index}.key\n"
            f"--selection-residency-json\n/bundle/selection-residency-{index}.json\n"
            f"--selection-model-cache-dir\n/evidence/model-cache-{index}\n"
            f"--repo-client-state-root\n/evidence/repo-client-{index}\n"
            "--selection-repo-registration\n/evidence/repo-registration.json\n"
            "--stages\n3\n",
            encoding="utf-8",
        )
    stage = {
        "repository": "Qwen/Qwen3.6-27B",
        "revision": "6a9e13bd6fc8f0983b9b99948120bc37f49c13e9",
        "decodeMode": "single-token-autoregressive",
        "modality": "text-only",
        "mtpEnabled": False,
        "stages": [
            {
                "role": role,
                "layerRange": {"start": start, "endExclusive": end},
                "metadata": {
                    "sequencePolicy": "stateful-prefill-decode-v1",
                    "stateInputNames": [
                        "attention_kv_in", "recurrent_state_in",
                        "convolution_state_in"],
                    "stateOutputNames": [
                        "attention_kv_out", "recurrent_state_out",
                        "convolution_state_out"],
                },
            }
            for role, (start, end) in zip(ROLES, ((0, 21), (21, 42), (42, 64)))
        ],
    }
    planning_stage, planning = complete_planning_fixture()
    stage.update({
        "modelDigest": planning_stage["modelDigest"],
        "dtype": planning_stage["dtype"],
        "quantization": planning_stage["quantization"],
        "layerRanges": planning_stage["layerRanges"],
        "tokenizer": planning_stage["tokenizer"],
    })
    for target, source in zip(stage["stages"], planning_stage["stages"]):
        target.update({"bytes": source["bytes"], "sha256": source["sha256"]})
    for name, value in {
        "stage-manifest.json": stage,
        "automatic-planning.json": planning,
    }.items():
        (bundle / name).write_text(json.dumps(value), encoding="utf-8")
    shutil.copyfile(WORKLOAD, bundle / "workload.json")
    workload = json.loads((bundle / "workload.json").read_text(encoding="utf-8"))
    references = {
        prompt["id"]: [1000 + index, workload["generation"]["eosTokenIds"][0]]
        for index, prompt in enumerate(workload["prompts"])
    }
    oracle = {
        "schemaVersion": "ndnsf-di-spec175-g5-oracle-v1",
        "model": workload["model"],
        "workloadSha256": digest(bundle / "workload.json"),
        "prompts": [
            {
                "promptId": prompt_id,
                "referenceGeneratedTokenIds": tokens,
                "stopReason": "EOS",
                "decodedTextBytes": 8,
                "decodedTextSha256": "sha256:" + str(index + 1) * 64,
            }
            for index, (prompt_id, tokens) in enumerate(references.items())
        ],
    }
    (bundle / "g5-oracle.json").write_text(json.dumps(oracle), encoding="utf-8")

    g6c_oracle = None
    if gate == "conversation-residency":
        def g6c_turn(first: bool, token: int) -> dict:
            return {
                ("inputTokenIds" if first else "appendedInputTokenIds"): [token],
                "referenceGeneratedTokenIds": [1000 + token],
                "eosTokenIds": workload["generation"]["eosTokenIds"],
            }

        g6c_oracle = {
            "schemaVersion": "ndnsf-di-spec175-g6c-oracle-v1",
            "model": workload["model"],
            "workloadSha256": digest(bundle / "workload.json"),
            "g5OracleSha256": digest(bundle / "g5-oracle.json"),
            "generation": {
                "strategy": "greedy", "maxNewTokens": 8, "maxEvents": 9,
                "requestDeadlineMs": 120000, "useCache": True,
                "thinkingMode": "disabled", "mtpEnabled": False},
            "turnPairs": [
                {"caseId": "two-turn", "conversationId": "g6c-two-turn-0001",
                 "firstTurn": g6c_turn(True, 10),
                 "secondTurn": g6c_turn(False, 11)},
                {"caseId": "paused-pressure",
                 "conversationId": "g6c-pressure-0001",
                 "firstTurn": g6c_turn(True, 20),
                 "secondTurn": g6c_turn(False, 21)},
            ],
            "unavailableRole": {
                "conversationId": "g6c-unavailable-0001",
                "role": ROLES[1],
                "expectedError": "CONVERSATION_STATE_UNAVAILABLE",
                "firstTurn": g6c_turn(True, 30),
                "secondTurn": g6c_turn(False, 31),
                "fallback": {
                    "authorized": True, "fullInputTokenIds": [30, 1030, 31],
                    "referenceGeneratedTokenIds": [1031],
                    "eosTokenIds": workload["generation"]["eosTokenIds"]},
            },
        }
        (bundle / "g6c-oracle.json").write_text(
            json.dumps(g6c_oracle), encoding="utf-8")
        for index in range(3):
            provider = bundle / "providers" / f"provider-{index}.args"
            extra = (
                "--spec175-host-tier-after-commit\n"
                "--spec175-host-tier-conversation-id\n"
                "g6c-pressure-0001\n")
            if index == 1:
                extra += (
                    "--spec175-invalidate-conversation-id\n"
                    "g6c-unavailable-0001\n")
            provider.write_text(
                provider.read_text(encoding="utf-8") + extra,
                encoding="utf-8")

    if gate == "performance":
        schedules = [
            [
                {"promptId": "P1", "phase": "cold", "repetition": 0},
                *[
                    {
                        "promptId": "P1" if index % 2 == 0 else "P2",
                        "phase": "measured",
                        "repetition": index // 2,
                    }
                    for index in range(10)
                ],
            ]
            for _ in range(3)
        ]
    elif gate == "diagnostic-multi-provider":
        schedules = [[{"promptId": "P1", "phase": "measured", "repetition": 0}]]
    elif gate == "conversation-residency":
        schedules = [[{"promptId": "P1", "phase": "measured", "repetition": 0}]]
    else:
        schedules = [
            [{"promptId": "P1", "phase": "cold", "repetition": 0}],
            *[
                [{
                    "promptId": prompt_id,
                    "phase": "measured",
                    "repetition": repetition,
                }]
                for repetition in range(3)
                for prompt_id in ("P1", "P2")
            ],
        ]

    process_groups = []
    workload_prompts = {prompt["id"]: prompt for prompt in workload["prompts"]}
    for index, schedule in enumerate(schedules):
        group_id = f"group-{index:02d}"
        relative_root = f"process-groups/{group_id}"
        group = bundle / relative_root
        group.mkdir(parents=True)
        if gate == "conversation-residency":
            assert g6c_oracle is not None
            campaign = {
                "schemaVersion":
                    "ndnsf-di-spec175-conversation-residency-campaign-v1",
                "campaignId": "candidate-175-g6c",
                "model": workload["model"],
                "workloadSha256": digest(bundle / "workload.json"),
                "g5OracleSha256": digest(bundle / "g5-oracle.json"),
                "conversationOracleSha256": digest(bundle / "g6c-oracle.json"),
                "generation": g6c_oracle["generation"],
                "turnPairs": g6c_oracle["turnPairs"],
                "unavailableRole": g6c_oracle["unavailableRole"],
            }
            campaign_relative = f"{relative_root}/conversation-campaign.json"
            user_relative = f"{relative_root}/user.args"
            (bundle / campaign_relative).write_text(
                json.dumps(campaign), encoding="utf-8")
            (bundle / user_relative).write_text(
                "python3\nuser.py\n--runtime\nqwen-onnx\n"
                f"--conversation-residency-manifest\n/bundle/{campaign_relative}\n"
                "--conversation-evidence-json\n/evidence/conversation-residency.json\n"
                "--automatic-planning-manifest\n/bundle/automatic-planning.json\n"
                "--qwen-stage-manifest\n/model/stage-manifest.json\n"
                "--qwen-tokenizer-dir\n/model/qwen-onnx-tokenizer\n"
                "--max-new-tokens\n8\n--timeout-ms\n120000\n"
                f"--request-id\n/candidate-175/{group_id}\n"
                "--stages\n3\n--initial-sync-settle-s\n5\n",
                encoding="utf-8")
            process_groups.append({
                "id": group_id,
                "conversationCampaign": record(bundle, campaign_relative),
                "userArgs": record(bundle, user_relative),
            })
            continue

        scheduled_prompt_ids = list(dict.fromkeys(
            entry["promptId"] for entry in schedule))
        campaign = {
            "schemaVersion": "ndnsf-di-qwen-generation-campaign-v2",
            "campaignId": f"candidate-175-{group_id}",
            "model": workload["model"],
            "workloadSha256": digest(bundle / "workload.json"),
            "oracleSha256": digest(bundle / "g5-oracle.json"),
            "generation": {
                "strategy": "greedy",
                "maxNewTokens": 64,
                "maxEvents": 65,
                "requestDeadlineMs": 120000,
                "requireEos": True,
                "useCache": True,
                "thinkingMode": "disabled",
                "mtpEnabled": False,
            },
            "prompts": [
                {
                    "promptId": prompt_id,
                    "formattedInputIds": workload_prompts[prompt_id]["inputTokenIds"],
                    "referenceGeneratedTokenIds": references[prompt_id],
                    "eosTokenIds": workload["generation"]["eosTokenIds"],
                }
                for prompt_id in scheduled_prompt_ids
            ],
            "schedule": schedule,
        }
        campaign_relative = f"{relative_root}/campaign.json"
        user_relative = f"{relative_root}/user.args"
        (bundle / campaign_relative).write_text(json.dumps(campaign), encoding="utf-8")
        (bundle / user_relative).write_text(
            "python3\nuser.py\n--runtime\nqwen-onnx\n"
            f"--generation-campaign-manifest\n/bundle/{campaign_relative}\n"
            "--generation-jsonl\n/evidence/generation.jsonl\n"
            "--automatic-planning-manifest\n/bundle/automatic-planning.json\n"
            "--qwen-stage-manifest\n/model/stage-manifest.json\n"
            "--qwen-tokenizer-dir\n/model/qwen-onnx-tokenizer\n"
            "--max-new-tokens\n64\n"
            "--timeout-ms\n120000\n"
            f"--request-id\n/candidate-175/{group_id}\n"
            "--stages\n3\n"
            "--initial-sync-settle-s\n5\n",
            encoding="utf-8",
        )
        process_groups.append({
            "id": group_id,
            "generationCampaign": record(bundle, campaign_relative),
            "userArgs": record(bundle, user_relative),
        })

    manifest = {
        "schemaVersion": "ndnsf-di-spec175-functional-bundle-v2",
        "gate": gate,
        "candidateId": "candidate-175",
        "model": {
            "repository": "Qwen/Qwen3.6-27B",
            "revision": "6a9e13bd6fc8f0983b9b99948120bc37f49c13e9",
        },
        "roles": list(ROLES),
        "stageManifest": record(bundle, "stage-manifest.json"),
        "workload": record(bundle, "workload.json"),
        "g5Oracle": record(bundle, "g5-oracle.json"),
        "automaticPlanningManifest": record(bundle, "automatic-planning.json"),
        "processGroups": process_groups,
    }
    if g6c_oracle is not None:
        manifest["conversationOracle"] = record(bundle, "g6c-oracle.json")
        shutil.copyfile(G6C_ANALYZER, bundle / "analyze-conversation-residency.py")
        manifest["conversationAnalyzer"] = record(
            bundle, "analyze-conversation-residency.py")
    if gate == "performance":
        shutil.copyfile(G7_ANALYZER, bundle / "analyze-performance.py")
        shutil.copyfile(RESOURCE_COLLECTOR, bundle / "collect-resources.py")
        manifest["performanceAnalyzer"] = record(
            bundle, "analyze-performance.py")
        manifest["resourceCollector"] = record(
            bundle, "collect-resources.py")
    (bundle / "spec175-functional-manifest.json").write_text(
        json.dumps(manifest), encoding="utf-8")
    return bundle


def run(bundle: Path, gate: str = "multi-provider") -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(PREFLIGHT), "--bundle", str(bundle), "--gate", gate],
        cwd=ROOT, text=True, capture_output=True, check=False,
    )


def test_functional_bundle_preflight_accepts_complete_subject(tmp_path: Path) -> None:
    result = run(make_bundle(tmp_path))
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)["status"] == "PASS"


def test_functional_bundle_rejects_control_only_bundle(tmp_path: Path) -> None:
    result = run(make_bundle(tmp_path, gate="control"))
    assert result.returncode != 0
    assert "gate does not match" in result.stdout


def test_functional_bundle_rejects_missing_invocation(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    path = bundle / "spec175-functional-manifest.json"
    value = json.loads(path.read_text())
    value["processGroups"] = value["processGroups"][:-1]
    path.write_text(json.dumps(value))
    result = run(bundle)
    assert result.returncode != 0
    assert "exactly 7 process group(s)" in result.stdout


def test_functional_bundle_rejects_v1_for_formal_gate(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    path = bundle / "spec175-functional-manifest.json"
    value = json.loads(path.read_text())
    value["schemaVersion"] = "ndnsf-di-spec175-functional-bundle-v1"
    value["invocations"] = [{"id": f"invocation-{index}"} for index in range(6)]
    path.write_text(json.dumps(value))
    result = run(bundle)
    assert result.returncode != 0
    assert "formal gate requires" in result.stdout


def test_functional_bundle_rejects_declared_invocations_without_fresh_groups(
        tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    path = bundle / "spec175-functional-manifest.json"
    value = json.loads(path.read_text())
    value["processGroups"] = value["processGroups"][:1]
    value["invocations"] = [{"id": f"invocation-{index}"} for index in range(6)]
    path.write_text(json.dumps(value))
    result = run(bundle)
    assert result.returncode != 0
    assert "exactly 7 process group(s)" in result.stdout


def test_functional_bundle_rejects_use_cache_false(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    campaign = sorted((bundle / "process-groups").glob("*/campaign.json"))[1]
    value = json.loads(campaign.read_text())
    value["generation"]["useCache"] = False
    campaign.write_text(json.dumps(value))
    manifest_path = bundle / "spec175-functional-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    manifest["processGroups"][1]["generationCampaign"]["sha256"] = digest(campaign)
    manifest_path.write_text(json.dumps(manifest))
    result = run(bundle)
    assert result.returncode != 0
    assert "useCache must be true" in result.stdout


def test_functional_bundle_rejects_campaign_deadline_drift(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    campaign = sorted((bundle / "process-groups").glob("*/campaign.json"))[1]
    value = json.loads(campaign.read_text())
    value["generation"]["requestDeadlineMs"] = 119999
    campaign.write_text(json.dumps(value))
    manifest_path = bundle / "spec175-functional-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    manifest["processGroups"][1]["generationCampaign"]["sha256"] = digest(campaign)
    manifest_path.write_text(json.dumps(manifest))
    result = run(bundle)
    assert result.returncode != 0
    assert "requestDeadlineMs must be 120000" in result.stdout


def test_functional_bundle_rejects_oracle_token_drift(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    campaign = sorted((bundle / "process-groups").glob("*/campaign.json"))[1]
    value = json.loads(campaign.read_text())
    value["prompts"][0]["referenceGeneratedTokenIds"][0] += 1
    campaign.write_text(json.dumps(value))
    manifest_path = bundle / "spec175-functional-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    manifest["processGroups"][1]["generationCampaign"]["sha256"] = digest(campaign)
    manifest_path.write_text(json.dumps(manifest))
    result = run(bundle)
    assert result.returncode != 0
    assert "reference tokens differ from G5 oracle" in result.stdout


def test_functional_bundle_accepts_performance_three_fresh_processes(
        tmp_path: Path) -> None:
    result = run(make_bundle(tmp_path, gate="performance"), gate="performance")
    assert result.returncode == 0, result.stdout


def test_functional_bundle_accepts_frozen_g6c_process_group(
        tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path, gate="conversation-residency")
    result = run(bundle, gate="conversation-residency")
    assert result.returncode == 0, result.stdout
    report = json.loads(result.stdout)
    assert report["processGroupCount"] == 1
    assert report["invocationCount"] == 7


def test_functional_bundle_rejects_g6c_without_exact_pressure_binding(
        tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path, gate="conversation-residency")
    provider = bundle / "providers/provider-0.args"
    provider.write_text(
        provider.read_text(encoding="utf-8").replace(
            "g6c-pressure-0001", "g6c-pressure-wrong"),
        encoding="utf-8")
    result = run(bundle, gate="conversation-residency")
    assert result.returncode != 0
    assert "pressure conversation binding mismatch" in result.stdout


def test_functional_bundle_rejects_g6c_oracle_drift(
        tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path, gate="conversation-residency")
    campaign = bundle / "process-groups/group-00/conversation-campaign.json"
    document = json.loads(campaign.read_text(encoding="utf-8"))
    document["turnPairs"][0]["secondTurn"]["referenceGeneratedTokenIds"] = [999]
    campaign.write_text(json.dumps(document), encoding="utf-8")
    manifest_path = bundle / "spec175-functional-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["processGroups"][0]["conversationCampaign"]["sha256"] = digest(
        campaign)
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    result = run(bundle, gate="conversation-residency")
    assert result.returncode != 0
    assert "turnPairs differs from the sealed G6C oracle" in result.stdout


def test_builder_produces_preflight_clean_g6_process_groups(tmp_path: Path) -> None:
    base = make_bundle(tmp_path / "source")
    shutil.copyfile(first_user_args(base), base / "user.args")
    for relative in (
        "policy.yaml",
        "repo-wrapper.sh",
        "selection-offer-key-map.json",
        "selection-residency-0.json",
        "selection-residency-1.json",
        "selection-residency-2.json",
    ):
        (base / relative).write_text("{}\n", encoding="utf-8")
    output = tmp_path / "built"
    result = subprocess.run([
        sys.executable, str(BUILDER),
        "--base-bundle", str(base),
        "--output", str(output),
        "--gate", "multi-provider",
        "--candidate-id", "candidate-175-built",
        "--stage-manifest", str(base / "stage-manifest.json"),
        "--workload", str(base / "workload.json"),
        "--g5-oracle", str(base / "g5-oracle.json"),
        "--automatic-planning", str(base / "automatic-planning.json"),
    ], cwd=ROOT, text=True, capture_output=True, check=False)
    assert result.returncode == 0, result.stderr
    preflight = run(output)
    assert preflight.returncode == 0, preflight.stdout
    document = json.loads(
        (output / "spec175-functional-manifest.json").read_text())
    assert len(document["processGroups"]) == 7
    assert "invocations" not in document
    assert not (output / "user.args").exists()


def test_builder_produces_preflight_clean_g6c_process_group(
        tmp_path: Path) -> None:
    base = make_bundle(tmp_path / "source", gate="conversation-residency")
    shutil.copyfile(first_user_args(base), base / "user.args")
    for relative in (
        "policy.yaml", "repo-wrapper.sh", "selection-offer-key-map.json",
        "selection-residency-0.json", "selection-residency-1.json",
        "selection-residency-2.json",
    ):
        (base / relative).write_text("{}\n", encoding="utf-8")
    output = tmp_path / "built-g6c"
    result = subprocess.run([
        sys.executable, str(BUILDER),
        "--base-bundle", str(base), "--output", str(output),
        "--gate", "conversation-residency",
        "--candidate-id", "candidate-175-built-g6c",
        "--stage-manifest", str(base / "stage-manifest.json"),
        "--workload", str(base / "workload.json"),
        "--g5-oracle", str(base / "g5-oracle.json"),
        "--automatic-planning", str(base / "automatic-planning.json"),
        "--conversation-oracle", str(base / "g6c-oracle.json"),
    ], cwd=ROOT, text=True, capture_output=True, check=False)
    assert result.returncode == 0, result.stderr
    preflight = run(output, gate="conversation-residency")
    assert preflight.returncode == 0, preflight.stdout
    document = json.loads(
        (output / "spec175-functional-manifest.json").read_text())
    assert len(document["processGroups"]) == 1
    assert "conversationCampaign" in document["processGroups"][0]


def test_functional_bundle_rejects_obsolete_selection_dataflow_v2(
        tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    provider = bundle / "providers/provider-0.args"
    provider.write_text(
        provider.read_text(encoding="utf-8").replace(
            "--selection-dataflow-v3", "--selection-dataflow-v2"
        ),
        encoding="utf-8",
    )
    result = run(bundle)
    assert result.returncode != 0
    assert "--selection-dataflow-v2 is obsolete" in result.stdout


def test_functional_bundle_requires_provider_state_manifest_binding(
        tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    provider = bundle / "providers/provider-0.args"
    provider.write_text(
        provider.read_text(encoding="utf-8").replace(
            "--qwen-stage-manifest\n/bundle/stage-manifest.json\n", ""),
        encoding="utf-8",
    )
    result = run(bundle)
    assert result.returncode != 0
    assert "Provider must bind the exact --qwen-stage-manifest" in result.stdout


def test_functional_bundle_rejects_mixed_selection_dataflow_versions(
        tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    provider = bundle / "providers/provider-0.args"
    provider.write_text(
        provider.read_text(encoding="utf-8") + "--selection-dataflow-v2\n",
        encoding="utf-8",
    )
    result = run(bundle)
    assert result.returncode != 0
    assert "--selection-dataflow-v2 is obsolete" in result.stdout


def test_functional_bundle_rejects_provider_role_map(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    path = bundle / "spec175-functional-manifest.json"
    value = json.loads(path.read_text())
    value["providerRoleMap"] = {"P0": ROLES[0]}
    path.write_text(json.dumps(value))
    result = run(bundle)
    assert result.returncode != 0
    assert "Provider-role map is not allowed" in result.stdout


def test_functional_bundle_rejects_runtime_source_overlay(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    provider = bundle / "providers/provider-0.args"
    provider.write_text(provider.read_text() + "/source/provider.py\n")
    result = run(bundle)
    assert result.returncode != 0
    assert "runtime source overlay" in result.stdout


def test_functional_bundle_requires_external_model_mount(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    provider = bundle / "providers/provider-0.args"
    provider.write_text(
        provider.read_text().replace("=/model/", "=/tmp/model/"),
        encoding="utf-8",
    )
    result = run(bundle)
    assert result.returncode != 0
    assert "mounted below /model" in result.stdout


def test_functional_bundle_accepts_explicit_shell_exec_wrappers(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    for path in [
        *sorted((bundle / "process-groups").glob("*/user.args")),
        *sorted((bundle / "providers").glob("*.args")),
    ]:
        argv = path.read_text(encoding="utf-8").splitlines()
        command = " ".join(shlex.quote(value) for value in argv)
        path.write_text(
            "bash\n-lc\nexport HOME=/evidence/home; exec " + command + "\n",
            encoding="utf-8",
        )
    manifest_path = bundle / "spec175-functional-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    for group in manifest["processGroups"]:
        group["userArgs"]["sha256"] = digest(bundle / group["userArgs"]["path"])
    manifest_path.write_text(json.dumps(manifest))
    result = run(bundle)
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)["status"] == "PASS"


def test_functional_bundle_rejects_missing_generation_jsonl(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    user = first_user_args(bundle)
    user.write_text(
        user.read_text(encoding="utf-8").replace(
            "--generation-jsonl\n/evidence/generation.jsonl\n", ""),
        encoding="utf-8",
    )
    manifest_path = bundle / "spec175-functional-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    manifest["processGroups"][0]["userArgs"]["sha256"] = digest(user)
    manifest_path.write_text(json.dumps(manifest))
    result = run(bundle)
    assert result.returncode != 0
    assert "missing --generation-jsonl" in result.stdout


def test_functional_bundle_rejects_shell_wrapper_without_exec(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    provider = bundle / "providers/provider-0.args"
    argv = provider.read_text(encoding="utf-8").splitlines()
    provider.write_text(
        "bash\n-lc\n" + " ".join(shlex.quote(value) for value in argv) + "\n",
        encoding="utf-8",
    )
    result = run(bundle)
    assert result.returncode != 0
    assert "shell wrapper must use an explicit exec" in result.stdout


def test_functional_bundle_rejects_controller_ready_before_start(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    wrapper = bundle / "controller-wrapper.sh"
    wrapper.write_text(
        "print('NDNSF_DI_CONTROLLER_READY', flush=True)\n"
        "controller.start()\ncontroller.run()\n",
        encoding="utf-8",
    )
    result = run(bundle)
    assert result.returncode != 0
    assert "controller readiness marker precedes controller.start()" in result.stdout
