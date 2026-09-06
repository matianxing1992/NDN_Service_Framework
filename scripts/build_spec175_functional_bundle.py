#!/usr/bin/env python3
"""Build a process-bound Spec175 G6/G7 functional bundle.

The input base bundle supplies only the already-qualified NFD/controller/repo
and Provider launch configuration.  This builder binds the frozen workload,
G5 CUDA oracle, automatic planning input, and one exact User command per fresh
process group.  It never accepts a caller-side Provider-role map.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import sys


SCHEMA = "ndnsf-di-spec175-functional-bundle-v2"
G6C_ORACLE_SCHEMA = "ndnsf-di-spec175-g6c-oracle-v1"
G6C_CAMPAIGN_SCHEMA = "ndnsf-di-spec175-conversation-residency-campaign-v1"
MODEL = "Qwen/Qwen3.6-27B"
REVISION = "6a9e13bd6fc8f0983b9b99948120bc37f49c13e9"
ROLES = (
    "/LLM/Pipeline/Stage/0",
    "/LLM/Pipeline/Stage/1",
    "/LLM/Pipeline/Stage/2",
)
BASE_FILES = (
    "nfd.conf",
    "controller-wrapper.sh",
    "controller.args",
    "policy.yaml",
    "repo-wrapper.sh",
    "selection-offer-key-map.json",
    "selection-residency-0.json",
    "selection-residency-1.json",
    "selection-residency-2.json",
)


def sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def record(root: Path, relative: str) -> dict[str, str]:
    return {"path": relative, "sha256": sha256(root / relative)}


def load_object(path: Path, label: str) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"{label} cannot be read: {exc}") from exc
    if not isinstance(value, dict):
        raise SystemExit(f"{label} root must be an object")
    return value


def _canonical_sha256(value: object, label: str) -> str:
    raw = str(value).strip().lower()
    if raw.startswith("sha256:"):
        raw = raw[7:]
    if not re.fullmatch(r"[0-9a-f]{64}", raw):
        raise SystemExit(f"{label} must be a canonical sha256 digest")
    return "sha256:" + raw


def validate_automatic_planning_manifest(
        stage: dict, planning: dict) -> str:
    """Reject a planning manifest produced by a different runtime contract.

    The User recomputes this candidate after the SIF starts.  Recompute it at
    bundle construction as well so a stale preSplit catalog cannot cross the
    packaging boundary and fail only on TigerCluster.
    """
    if planning.get("schemaVersion") != (
            "ndnsf-di-spec162-automatic-planning-v1"):
        raise SystemExit("automatic planning manifest schema mismatch")
    model_doc = planning.get("model")
    if not isinstance(model_doc, dict):
        raise SystemExit("automatic planning manifest model is missing")
    if (model_doc.get("name") != stage.get("repository")
            or model_doc.get("revision") != stage.get("revision")):
        raise SystemExit("automatic planning model identity mismatches stage manifest")
    if planning.get("layerRanges") != stage.get("layerRanges"):
        raise SystemExit("automatic planning layer ranges mismatch stage manifest")

    stage_rows = stage.get("stages")
    planning_rows = planning.get("stages")
    if not isinstance(stage_rows, list) or not isinstance(planning_rows, list) \
            or len(stage_rows) != 3 or len(planning_rows) != 3:
        raise SystemExit("automatic planning requires three stage rows")
    stage_by_role = {str(item.get("role")): item for item in stage_rows}
    planning_by_role = {str(item.get("role")): item for item in planning_rows}
    if set(stage_by_role) != set(ROLES) or set(planning_by_role) != set(ROLES):
        raise SystemExit("automatic planning roles do not match the frozen role set")
    artifact_digests = {}
    weight_bytes = {}
    for role in ROLES:
        source = stage_by_role[role]
        planned = planning_by_role[role]
        source_digest = _canonical_sha256(
            source.get("sha256"), f"stage {role} digest")
        planned_digest = _canonical_sha256(
            planned.get("sha256"), f"planning stage {role} digest")
        if source_digest != planned_digest:
            raise SystemExit(
                f"automatic planning artifact digest mismatch for {role}")
        try:
            source_bytes = int(source["bytes"])
            planned_bytes = int(planned["bytes"])
        except (KeyError, TypeError, ValueError) as exc:
            raise SystemExit(
                f"automatic planning stage {role} bytes are invalid") from exc
        if source_bytes != planned_bytes:
            raise SystemExit(
                f"automatic planning artifact bytes mismatch for {role}")
        artifact_digests[role] = planned_digest
        weight_bytes[role] = planned_bytes

    source_root = Path(__file__).resolve().parents[1] / "NDNSF-DistributedInference"
    if not source_root.is_dir():
        raise SystemExit(f"NDNSF-DI planner source is missing: {source_root}")
    if str(source_root) not in sys.path:
        sys.path.insert(0, str(source_root))
    try:
        from ndnsf_distributed_inference.adapters.qwen import (
            build_qwen_three_stage_adapter,
        )
    except Exception as exc:  # pragma: no cover - environment diagnosis
        raise SystemExit(
            f"cannot load NDNSF-DI planner for manifest validation: {exc}") from exc
    precision = str(
        planning.get("precision") or planning.get("dtype")
        or model_doc.get("dtype") or stage.get("dtype") or "")
    if not precision:
        raise SystemExit("automatic planning precision is missing")
    try:
        layer_ranges = tuple(
            (int(item[0]), int(item[1]))
            for item in planning["layerRanges"]
        )
        adapter = build_qwen_three_stage_adapter(
            model_name=str(model_doc["name"]),
            revision=str(model_doc["revision"]),
            layer_ranges=layer_ranges,
            artifact_digests_by_role=artifact_digests,
            weight_bytes_by_role=weight_bytes,
            precision=precision,
        )
        described = adapter.describe_model(
            str(model_doc["name"]),
            _canonical_sha256(model_doc["contentDigest"], "model contentDigest"),
            _canonical_sha256(model_doc["semanticsDigest"], "model semanticsDigest"),
            source_revision=str(model_doc["revision"]),
        )
        graph = adapter.graph.inspect(described)
        candidate = adapter.splitter.enumerate_candidates(described, graph)[0]
    except (KeyError, TypeError, ValueError, IndexError) as exc:
        raise SystemExit(f"automatic planning manifest cannot be recomputed: {exc}") from exc

    if planning.get("adapterDescriptorDigest") != adapter.descriptor.descriptor_digest:
        raise SystemExit("automatic planning adapter descriptor digest mismatch")
    if planning.get("adapterCompositionDigest") != adapter.composition_digest:
        raise SystemExit("automatic planning adapter composition digest mismatch")
    if planning.get("graphDigest") != graph.graph_digest:
        raise SystemExit("automatic planning graph digest mismatch")
    expected = candidate.candidate_digest
    if planning.get("candidateDigest") != expected:
        raise SystemExit(
            "automatic planning candidate digest mismatch: "
            f"manifest={planning.get('candidateDigest')} runtime={expected}")
    catalog = planning.get("preSplitCatalog")
    if not isinstance(catalog, dict) or catalog.get("candidateDigest") != expected:
        raise SystemExit("automatic planning preSplitCatalog candidate digest mismatch")
    return expected


def update_flag(argv: list[str], flag: str, value: str) -> None:
    positions = [index for index, token in enumerate(argv) if token == flag]
    if len(positions) > 1:
        raise SystemExit(f"duplicate User flag in base bundle: {flag}")
    if positions:
        index = positions[0]
        if index + 1 >= len(argv):
            raise SystemExit(f"base User flag lacks a value: {flag}")
        argv[index + 1] = value
    else:
        argv.extend((flag, value))


def remove_flag(argv: list[str], flag: str) -> None:
    positions = [index for index, token in enumerate(argv) if token == flag]
    if len(positions) > 1:
        raise SystemExit(f"duplicate User flag in base bundle: {flag}")
    if positions:
        index = positions[0]
        if index + 1 >= len(argv):
            raise SystemExit(f"base User flag lacks a value: {flag}")
        del argv[index:index + 2]


def rewrite_user_args(
    path: Path,
    replacements: dict[str, str],
    *,
    removals: tuple[str, ...] = (),
    boolean_flags: tuple[str, ...] = (),
) -> str:
    lines = [line.strip() for line in path.read_text(encoding="utf-8").splitlines()
             if line.strip() and not line.lstrip().startswith("#")]
    if not lines:
        raise SystemExit("base bundle user.args is empty")
    shell_prefix = ""
    header: list[str] = []
    if len(lines) >= 3 and lines[0] in {"bash", "sh"} and lines[1] in {"-c", "-lc"}:
        command = lines[2]
        match = re.search(r"(?:^|[;&])\s*exec\s+", command)
        if match is None:
            raise SystemExit("base user shell wrapper must contain explicit exec")
        shell_prefix = command[:match.end()]
        header = lines[:2]
        argv = shlex.split(command[match.end():])
    else:
        argv = list(lines)
    for forbidden in ("--provider-map", "--provider-role-map", "--placement"):
        if forbidden in argv:
            raise SystemExit("base User args contain a caller-side Provider-role map")
    while "--diagnostic-token-loop" in argv:
        argv.remove("--diagnostic-token-loop")
    for flag in removals:
        remove_flag(argv, flag)
    for flag in boolean_flags:
        if flag not in argv:
            argv.append(flag)
    for flag, value in replacements.items():
        update_flag(argv, flag, value)
    if header:
        return "\n".join((*header, shell_prefix + shlex.join(argv))) + "\n"
    return "\n".join(argv) + "\n"


def schedules_for(gate: str) -> list[list[dict[str, object]]]:
    if gate == "multi-provider":
        return [
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
    if gate == "performance":
        schedule = [
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
        return [list(schedule) for _ in range(3)]
    if gate == "conversation-residency":
        return [[]]
    raise SystemExit(f"unsupported builder gate: {gate}")


def build(args: argparse.Namespace) -> Path:
    base = args.base_bundle.expanduser().resolve()
    output = args.output.expanduser().resolve()
    if not base.is_dir():
        raise SystemExit(f"base bundle is not a directory: {base}")
    if output.exists():
        raise SystemExit(f"output already exists: {output}")
    if not re.fullmatch(r"[A-Za-z0-9._-]+", args.candidate_id):
        raise SystemExit("candidate-id must be a safe nonempty component")
    user_template = base / "user.args"
    provider_root = base / "providers"
    if not user_template.is_file() or not provider_root.is_dir():
        raise SystemExit("base bundle lacks user.args or providers/")
    providers = sorted(provider_root.glob("*.args"))
    if len(providers) != 3:
        raise SystemExit("base bundle must contain exactly three Provider args")
    for provider in providers:
        text = provider.read_text(encoding="utf-8")
        if "--selection-dataflow-v3" not in text or "--selection-dataflow-v2" in text:
            raise SystemExit("base Provider args must use selection-dataflow-v3 only")
    for relative in BASE_FILES:
        if not (base / relative).is_file():
            raise SystemExit(f"base bundle lacks {relative}")

    workload = load_object(args.workload, "workload")
    oracle = load_object(args.g5_oracle, "G5 oracle")
    stage = load_object(args.stage_manifest, "stage manifest")
    planning = load_object(args.automatic_planning, "automatic planning manifest")
    validate_automatic_planning_manifest(stage, planning)
    if workload.get("schema") != "ndnsf-di-spec175-workload-v1":
        raise SystemExit("workload schema is not frozen Spec175 v1")
    if workload.get("model", {}).get("repository") != MODEL \
            or workload.get("model", {}).get("revision") != REVISION:
        raise SystemExit("workload model identity mismatch")
    if oracle.get("schemaVersion") != "ndnsf-di-spec175-g5-oracle-v1" \
            or oracle.get("model", {}).get("repository") != MODEL \
            or oracle.get("model", {}).get("revision") != REVISION:
        raise SystemExit("G5 oracle identity mismatch")
    workload_digest = sha256(args.workload)
    if oracle.get("workloadSha256") != workload_digest:
        raise SystemExit("G5 oracle is not bound to the selected workload")
    if stage.get("repository") != MODEL or stage.get("revision") != REVISION:
        raise SystemExit("stage manifest identity mismatch")

    conversation_oracle = None
    if args.gate == "conversation-residency":
        if args.conversation_oracle is None:
            raise SystemExit(
                "conversation-residency requires --conversation-oracle")
        conversation_oracle = load_object(
            args.conversation_oracle, "G6C conversation oracle")
        if conversation_oracle.get("schemaVersion") != G6C_ORACLE_SCHEMA:
            raise SystemExit("G6C conversation oracle schema mismatch")
        if conversation_oracle.get("model") != workload.get("model"):
            raise SystemExit("G6C conversation oracle model mismatch")
        if conversation_oracle.get("workloadSha256") != workload_digest:
            raise SystemExit("G6C conversation oracle workload mismatch")
        if conversation_oracle.get("g5OracleSha256") != sha256(args.g5_oracle):
            raise SystemExit("G6C conversation oracle G5 binding mismatch")
        if conversation_oracle.get("generation") != {
                "strategy": "greedy", "maxNewTokens": 8,
                "maxEvents": 9, "requestDeadlineMs": 120000,
                "useCache": True, "thinkingMode": "disabled",
                "mtpEnabled": False}:
            raise SystemExit("G6C conversation generation contract mismatch")
        pairs = conversation_oracle.get("turnPairs")
        unavailable = conversation_oracle.get("unavailableRole")
        if (not isinstance(pairs, list) or len(pairs) != 2
                or {str(item.get("caseId", "")) for item in pairs
                    if isinstance(item, dict)}
                != {"two-turn", "paused-pressure"}
                or not isinstance(unavailable, dict)
                or unavailable.get("expectedError")
                != "CONVERSATION_STATE_UNAVAILABLE"):
            raise SystemExit("G6C conversation cases are incomplete")

    workload_prompts = {
        str(item["id"]): item for item in workload.get("prompts", ())
        if isinstance(item, dict) and item.get("id")
    }
    oracle_prompts = {
        str(item["promptId"]): item for item in oracle.get("prompts", ())
        if isinstance(item, dict) and item.get("promptId")
    }
    if set(workload_prompts) != {"P1", "P2"} \
            or set(oracle_prompts) != {"P1", "P2"}:
        raise SystemExit("workload and G5 oracle must contain exactly P1/P2")
    require_eos_values = {
        str(item.get("stopReason")) == "EOS" for item in oracle_prompts.values()
    }
    if len(require_eos_values) != 1:
        raise SystemExit("P1/P2 oracle stop policies differ")
    require_eos = require_eos_values.pop()
    eos_ids = workload.get("generation", {}).get("eosTokenIds")
    if not isinstance(eos_ids, list) or not eos_ids:
        raise SystemExit("workload EOS IDs are missing")

    output.mkdir(parents=True)
    for relative in BASE_FILES:
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(base / relative, target)
    (output / "providers").mkdir()
    pressure_conversation_id = ""
    unavailable_conversation_id = ""
    unavailable_role = ""
    if conversation_oracle is not None:
        pressure_case = next(
            item for item in conversation_oracle["turnPairs"]
            if item["caseId"] == "paused-pressure")
        pressure_conversation_id = str(pressure_case["conversationId"])
        unavailable_conversation_id = str(
            conversation_oracle["unavailableRole"]["conversationId"])
        unavailable_role = str(conversation_oracle["unavailableRole"]["role"])
        if unavailable_role not in ROLES:
            raise SystemExit("G6C unavailable role is not in the frozen role map")
    for index, provider in enumerate(providers):
        replacements = {
            "--device": f"cuda:{index}",
            "--selection-gpu-capacity-mib": str(args.gpu_capacity_mib),
            "--qwen-stage-manifest": "/bundle/qwen36-stage-manifest.json",
        }
        if conversation_oracle is not None:
            replacements.update({
                "--spec175-host-tier-conversation-id": pressure_conversation_id,
            })
            # Boolean flags are appended after the value-bearing rewrite.
        if conversation_oracle is not None and ROLES[index] == unavailable_role:
            replacements["--spec175-invalidate-conversation-id"] = (
                unavailable_conversation_id)
        rewritten = rewrite_user_args(
            provider, replacements,
            boolean_flags=(
                ("--spec175-host-tier-after-commit",)
                if conversation_oracle is not None else ()),
        )
        (output / "providers" / provider.name).write_text(
            rewritten, encoding="utf-8")
    wrapper = provider_root / "provider-wrapper.sh"
    if wrapper.is_file():
        shutil.copy2(wrapper, output / "providers/provider-wrapper.sh")
    for source, relative in (
        (args.stage_manifest, "qwen36-stage-manifest.json"),
        (args.workload, "workload.json"),
        (args.g5_oracle, "g5-oracle.json"),
        (args.automatic_planning, "automatic-planning.json"),
    ):
        shutil.copy2(source, output / relative)
    if conversation_oracle is not None:
        shutil.copy2(args.conversation_oracle, output / "g6c-oracle.json")
        analyzer = Path(__file__).resolve().parent / \
            "analyze_spec175_conversation_residency.py"
        if not analyzer.is_file():
            raise SystemExit("repository G6C analyzer is missing")
        shutil.copy2(analyzer, output / "analyze-conversation-residency.py")
    if args.gate == "performance":
        scripts_root = Path(__file__).resolve().parent
        for source_name, destination in (
            ("analyze_spec175_performance.py", "analyze-performance.py"),
            ("collect_spec175_resources.py", "collect-resources.py"),
        ):
            source = scripts_root / source_name
            if not source.is_file():
                raise SystemExit(f"repository helper is missing: {source_name}")
            shutil.copy2(source, output / destination)

    process_groups = []
    oracle_digest = sha256(output / "g5-oracle.json")
    for index, schedule in enumerate(schedules_for(args.gate)):
        group_id = f"{args.gate}-group-{index:02d}"
        relative_root = f"process-groups/{group_id}"
        group_root = output / relative_root
        group_root.mkdir(parents=True)
        if args.gate == "conversation-residency":
            assert conversation_oracle is not None
            campaign = {
                "schemaVersion": G6C_CAMPAIGN_SCHEMA,
                "campaignId": f"{args.candidate_id}-g6c",
                "model": workload["model"],
                "workloadSha256": workload_digest,
                "g5OracleSha256": oracle_digest,
                "conversationOracleSha256": sha256(
                    output / "g6c-oracle.json"),
                "generation": conversation_oracle["generation"],
                "turnPairs": conversation_oracle["turnPairs"],
                "unavailableRole": conversation_oracle["unavailableRole"],
            }
            campaign_relative = f"{relative_root}/conversation-campaign.json"
            user_relative = f"{relative_root}/user.args"
            (output / campaign_relative).write_text(
                json.dumps(campaign, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")
            user_text = rewrite_user_args(
                user_template,
                {
                    "--conversation-residency-manifest":
                        f"/bundle/{campaign_relative}",
                    "--conversation-evidence-json":
                        "/evidence/conversation-residency.json",
                    "--automatic-planning-manifest":
                        "/bundle/automatic-planning.json",
                    "--qwen-stage-manifest":
                        "/model/qwen36-stage-manifest.json",
                    "--qwen-tokenizer-dir": "/model/qwen-onnx-tokenizer",
                    "--max-new-tokens": "8",
                    "--timeout-ms": "120000",
                    "--request-id": f"/{args.candidate_id}/{group_id}",
                    "--workload-digest": workload_digest,
                    "--stages": "3",
                    "--initial-sync-settle-s": "5",
                },
                removals=("--generation-campaign-manifest",
                          "--generation-jsonl"),
            )
            (output / user_relative).write_text(user_text, encoding="utf-8")
            process_groups.append({
                "id": group_id,
                "conversationCampaign": record(output, campaign_relative),
                "userArgs": record(output, user_relative),
            })
            continue

        prompt_ids = list(dict.fromkeys(str(item["promptId"]) for item in schedule))
        campaign = {
            "schemaVersion": "ndnsf-di-qwen-generation-campaign-v2",
            "campaignId": f"{args.candidate_id}-{group_id}",
            "model": workload["model"],
            "workloadSha256": workload_digest,
            "oracleSha256": oracle_digest,
            "generation": {
                "strategy": "greedy",
                "maxNewTokens": 64,
                "maxEvents": 65,
                "requestDeadlineMs": 120000,
                "requireEos": require_eos,
                "useCache": True,
                "thinkingMode": "disabled",
                "mtpEnabled": False,
            },
            "prompts": [
                {
                    "promptId": prompt_id,
                    "formattedInputIds": workload_prompts[prompt_id]["inputTokenIds"],
                    "referenceGeneratedTokenIds": oracle_prompts[prompt_id][
                        "referenceGeneratedTokenIds"],
                    "eosTokenIds": eos_ids,
                }
                for prompt_id in prompt_ids
            ],
            "schedule": schedule,
        }
        campaign_relative = f"{relative_root}/campaign.json"
        user_relative = f"{relative_root}/user.args"
        (output / campaign_relative).write_text(
            json.dumps(campaign, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        user_text = rewrite_user_args(user_template, {
            "--generation-campaign-manifest": f"/bundle/{campaign_relative}",
            "--generation-jsonl": "/evidence/generation.jsonl",
            "--automatic-planning-manifest": "/bundle/automatic-planning.json",
            "--qwen-stage-manifest": "/model/qwen36-stage-manifest.json",
            "--qwen-tokenizer-dir": "/model/qwen-onnx-tokenizer",
            "--max-new-tokens": "64",
            "--timeout-ms": "120000",
            "--request-id": f"/{args.candidate_id}/{group_id}",
            "--workload-digest": workload_digest,
            "--stages": "3",
            "--initial-sync-settle-s": "5",
        })
        (output / user_relative).write_text(user_text, encoding="utf-8")
        process_groups.append({
            "id": group_id,
            "generationCampaign": record(output, campaign_relative),
            "userArgs": record(output, user_relative),
        })

    manifest = {
        "schemaVersion": SCHEMA,
        "gate": args.gate,
        "candidateId": args.candidate_id,
        "model": {"repository": MODEL, "revision": REVISION},
        "roles": list(ROLES),
        "stageManifest": record(output, "qwen36-stage-manifest.json"),
        "workload": record(output, "workload.json"),
        "g5Oracle": record(output, "g5-oracle.json"),
        "automaticPlanningManifest": record(output, "automatic-planning.json"),
        "processGroups": process_groups,
    }
    if conversation_oracle is not None:
        manifest["conversationOracle"] = record(output, "g6c-oracle.json")
        manifest["conversationAnalyzer"] = record(
            output, "analyze-conversation-residency.py")
    if args.gate == "performance":
        manifest["performanceAnalyzer"] = record(
            output, "analyze-performance.py")
        manifest["resourceCollector"] = record(
            output, "collect-resources.py")
    (output / "spec175-functional-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-bundle", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--gate", required=True,
        choices=("multi-provider", "conversation-residency", "performance"))
    parser.add_argument("--candidate-id", required=True)
    parser.add_argument("--stage-manifest", required=True, type=Path)
    parser.add_argument("--workload", required=True, type=Path)
    parser.add_argument("--g5-oracle", required=True, type=Path)
    parser.add_argument("--automatic-planning", required=True, type=Path)
    parser.add_argument("--conversation-oracle", type=Path)
    parser.add_argument(
        "--gpu-capacity-mib", type=int, default=32760,
        help="Per-Provider capacity advertised by the target RTX 6000 allocation.",
    )
    args = parser.parse_args()
    if args.gpu_capacity_mib <= 0:
        parser.error("--gpu-capacity-mib must be positive")
    output = build(args)
    print(json.dumps({
        "schemaVersion": SCHEMA,
        "status": "PASS",
        "gate": args.gate,
        "bundle": str(output),
        "manifestSha256": sha256(output / "spec175-functional-manifest.json"),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
