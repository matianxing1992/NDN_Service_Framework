#!/usr/bin/env python3
"""Generate a native NDNSF-DI bundle for proportional LLM stage execution."""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[3]
DIST_INF = REPO / "NDNSF-DistributedInference"
PLAN_LLM = ROOT / "plan_llm_resource_aware.py"
DEFAULT_MODEL_SPEC = ROOT / "llm_model_spec_qwen_tiny_proportional.json"
DEFAULT_PROVIDER_PROFILES = ROOT / "llm_provider_profiles_2_4_8.json"
DEFAULT_ARTIFACT = ROOT / "artifacts/qwen-native-tracer-backbone.onnx"
SERVICE = "/Inference/NativeTracer"
GROUP = "/NDNSF-DI/Tracer/group"
CONTROLLER = "/NDNSF-DI/Tracer/controller"
USER = "/NDNSF-DI/Tracer/user"

sys.path.insert(0, str(DIST_INF))

from ndnsf_distributed_inference.policy import write_policy_bundle  # noqa: E402


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def run_planner(model_spec: Path,
                provider_profiles: Path,
                out: Path,
                mode: str,
                target_rps: float,
                provider_workers: int,
                stage_execution_delay_ms: float,
                stage_execution_delay_scale: float) -> dict[str, Any]:
    prediction_fixed_stage_ms = stage_execution_delay_ms if stage_execution_delay_ms > 0.0 else 0.0
    prediction_compute_scale = 1.0 if prediction_fixed_stage_ms > 0.0 else stage_execution_delay_scale
    subprocess.run([
        sys.executable,
        str(PLAN_LLM),
        "--model-spec", str(model_spec),
        "--provider-profiles", str(provider_profiles),
        "--out", str(out),
        "--mode", mode,
        "--target-rps", str(target_rps),
        "--provider-workers", str(provider_workers),
        "--prediction-compute-scale", str(prediction_compute_scale),
        "--prediction-fixed-stage-ms", str(prediction_fixed_stage_ms),
        "--validate",
        "--expect-shards", "no",
    ], check=True)
    return load_json(out)


def stage_dependency(index: int,
                     producer_role: str,
                     consumer_role: str,
                     estimated_bytes: int) -> dict[str, Any]:
    return {
        "producers": [producer_role],
        "consumers": [consumer_role],
        "key_scope": f"llm-stage-{index}-to-{index + 1}",
        "topic_prefix": "/activation",
        "object_name_template": (
            "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/"
            "{keyScope}/{producerRole}/bundle/{sequence}"
        ),
        "expected_segments": 1,
        "expected_bytes": max(1, estimated_bytes),
        "tensors": [f"hidden-{index}-to-{index + 1}"],
    }


def artifact_for_stage(stage: dict[str, Any],
                       index: int,
                       stage_count: int,
                       artifact_path: Path,
                       stage_execution_delay_ms: float,
                       stage_execution_delay_scale: float) -> dict[str, Any]:
    role = str(stage["role"])
    execution_delay_ms = (
        stage_execution_delay_ms
        if stage_execution_delay_ms > 0.0 else
        float(stage["estimatedComputeMs"]) * stage_execution_delay_scale
    )
    metadata = {
        "sourceModel": "qwen-tiny-proportional-demo",
        "plannerMode": "proportional",
        "layerStart": str(stage["layerStart"]),
        "layerEnd": str(stage["layerEnd"]),
        "layerCount": str(stage["layerCount"]),
        "memoryMb": str(stage["memoryMb"]),
        "estimatedComputeMs": str(stage["estimatedComputeMs"]),
        "executionDelayMs": str(execution_delay_ms),
        "input_tensors": "images" if index == 0 else f"hidden-{index - 1}-to-{index}",
        "output_tensors": "token-logits" if index == stage_count - 1 else f"hidden-{index}-to-{index + 1}",
        "forceOutputBundle": "true",
    }
    if index > 0:
        metadata[f"inputScope.hidden-{index - 1}-to-{index}"] = (
            f"llm-stage-{index - 1}-to-{index}")
    metadata["outputScope.0"] = (
        "final-response" if index == stage_count - 1 else
        f"llm-stage-{index}-to-{index + 1}"
    )
    return {
        "role": role,
        "path": str(artifact_path),
        "artifact": f"/Artifact/LLMProportional/Stage/{index}",
        "filename": f"qwen-tiny-llm-stage-{index}.onnx",
        "kind": "model",
        "backend": "onnxruntime",
        "metadata": metadata,
    }


def provider_entries(stages: list[dict[str, Any]]) -> list[dict[str, Any]]:
    roles_by_provider: dict[str, list[str]] = {}
    for stage in stages:
        roles_by_provider.setdefault(str(stage["provider"]), []).append(str(stage["role"]))
    return [
        {
            "identity": provider,
            "roles": roles,
        }
        for provider, roles in sorted(roles_by_provider.items())
    ]


def build_policy_config(plan: dict[str, Any],
                        artifact_path: Path,
                        stage_execution_delay_ms: float,
                        stage_execution_delay_scale: float) -> dict[str, Any]:
    stages = list(plan.get("stages", []))
    if not stages:
        raise RuntimeError("LLM plan must contain at least one linear stage")
    roles = [str(stage["role"]) for stage in stages]
    dependencies = []
    for index in range(len(stages) - 1):
        dep = plan.get("dependencies", [])[index] if index < len(plan.get("dependencies", [])) else {}
        dependencies.append(stage_dependency(
            index,
            roles[index],
            roles[index + 1],
            int(dep.get("estimatedBytes", 32 * 1024 * 1024)),
        ))
    artifacts = [
        artifact_for_stage(stage, index, len(stages), artifact_path,
                           stage_execution_delay_ms,
                           stage_execution_delay_scale)
        for index, stage in enumerate(stages)
    ]
    return {
        "application": "native-di-llm-proportional",
        "controller": CONTROLLER,
        "group": GROUP,
        "runtime": {
            "user_identity": USER,
            "provider_prefix": "/NDNSF-DI/Tracer/provider",
        },
        "trust": {
            "app_roots": ["/NDNSF-DI"],
        },
        "artifact_security": {
            "allowlist": [],
            "sandbox": {
                "kind": "",
            },
        },
        "services": [
            {
                "name": SERVICE,
                "model": f"/Model/LLM/{plan.get('modelId', 'qwen-tiny')}/v1",
                "users": [USER],
                "providers": provider_entries(stages),
                "roles": roles,
                "dependencies": dependencies,
                "artifacts": artifacts,
                "input": {
                    "codec": "tensor-bundle",
                    "fields": {
                        "images": {
                            "dtype": "float32",
                            "shape": [1, 3, 2, 2],
                        },
                    },
                },
                "output": {
                    "codec": "tensor-bundle",
                    "fields": {
                        "token-logits": {
                            "dtype": "float32",
                        },
                    },
                },
                "metadata": {
                    "modelFamily": "llm",
                    "modelFormat": "onnx",
                    "plannerKind": "llm-pipeline",
                    "runtimeBackend": "onnxruntime",
                    "executionMode": "llm-proportional-deterministic",
                    "llmPipeline": {
                        "plannerMode": plan.get("plannerMode", "proportional"),
                        "planId": plan.get("planId", ""),
                        "stageCount": len(stages),
                        "stageExecutionDelayMs": stage_execution_delay_ms,
                        "stageExecutionDelayScale": stage_execution_delay_scale,
                        "layerAllocation": plan.get("summary", {}).get("layerAllocation", {}),
                    },
                },
            },
        ],
    }


def write_assignment_csv(path: Path,
                         plan: dict[str, Any],
                         assignment_label: str) -> list[dict[str, str]]:
    rows = []
    for stage in plan.get("stages", []):
        rows.append({
            "assignment": assignment_label,
            "role": str(stage["role"]),
            "provider": str(stage["provider"]),
            "node": str(stage.get("node", "")),
            "service": SERVICE,
        })
    if not rows:
        raise RuntimeError("cannot write assignment CSV for plan with no stages")
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=[
            "assignment", "role", "provider", "node", "service",
        ])
        writer.writeheader()
        writer.writerows(rows)
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--model-spec", type=Path, default=DEFAULT_MODEL_SPEC)
    parser.add_argument("--provider-profiles", type=Path, default=DEFAULT_PROVIDER_PROFILES)
    parser.add_argument("--planner-mode", choices=["greedy", "proportional"], default="proportional")
    parser.add_argument("--assignment-label", default="llm-proportional")
    parser.add_argument("--stage-execution-delay-ms", type=float, default=0.0)
    parser.add_argument("--stage-execution-delay-scale", type=float, default=1.0)
    parser.add_argument("--target-rps", type=float, default=0.0)
    parser.add_argument("--provider-workers", type=int, default=1)
    parser.add_argument("--plan-json", type=Path)
    parser.add_argument("--summary-json", type=Path)
    parser.add_argument("--artifact-path", type=Path, default=DEFAULT_ARTIFACT)
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    if args.plan_json is not None:
        plan = load_json(args.plan_json)
    else:
        with tempfile.TemporaryDirectory(prefix="ndnsf-llm-plan-") as tmp:
            plan = run_planner(
                args.model_spec,
                args.provider_profiles,
                Path(tmp) / "llm-plan.json",
                args.planner_mode,
                args.target_rps,
                args.provider_workers,
                args.stage_execution_delay_ms,
                args.stage_execution_delay_scale,
            )
    if args.stage_execution_delay_ms < 0.0:
        raise RuntimeError("--stage-execution-delay-ms must be non-negative")
    if args.stage_execution_delay_scale < 0.0:
        raise RuntimeError("--stage-execution-delay-scale must be non-negative")
    if args.target_rps < 0.0:
        raise RuntimeError("--target-rps must be non-negative")
    if args.provider_workers <= 0:
        raise RuntimeError("--provider-workers must be positive")
    policy_config = build_policy_config(
        plan,
        args.artifact_path,
        args.stage_execution_delay_ms,
        args.stage_execution_delay_scale)
    policy_config_path = args.out / "llm-proportional-policy-config.json"
    write_json(policy_config_path, policy_config)
    deployment = write_policy_bundle(policy_config_path, args.out)
    assignment_csv = args.out / "assignment.csv"
    assignment_rows = write_assignment_csv(assignment_csv, plan, args.assignment_label)
    summary = {
        "status": "executed",
        "service": SERVICE,
        "policyBundle": str(args.out),
        "policyConfig": str(policy_config_path),
        "nativeExecutionPlan": deployment.native_execution_plan_file,
        "nativeExecutionPlanSha256": deployment.native_execution_plan_sha256,
        "serviceManifest": deployment.service_manifest_file,
        "serviceManifestSha256": deployment.service_manifest_sha256,
        "controllerPolicy": deployment.policy_file,
        "trustSchema": deployment.trust_schema,
        "assignment": args.assignment_label,
        "assignmentCsv": str(assignment_csv),
        "assignmentRows": assignment_rows,
        "planId": plan.get("planId", ""),
        "plannerMode": plan.get("plannerMode", args.planner_mode),
        "modelFamily": "llm",
        "modelFormat": "onnx",
        "plannerKind": "llm-pipeline",
        "configuredRunnerProfile": "deterministic-fixture",
        "stageExecutionDelayMs": args.stage_execution_delay_ms,
        "stageExecutionDelayScale": args.stage_execution_delay_scale,
        "targetRps": args.target_rps,
        "providerWorkers": args.provider_workers,
        "roles": [row["role"] for row in assignment_rows],
        "stages": plan.get("stages", []),
        "prediction": plan.get("prediction", {}),
        "summary": plan.get("summary", {}),
    }
    if args.summary_json is not None:
        write_json(args.summary_json, summary)
    print(
        "NDNSF_DI_LLM_PROPORTIONAL_BUNDLE_READY "
        f"out={args.out} roles={len(summary['roles'])} planId={summary['planId']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
