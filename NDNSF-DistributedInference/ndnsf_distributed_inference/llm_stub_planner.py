"""Stub LLM planners for validating DI planner dispatch.

These planners intentionally do not execute LLM inference.  They produce
abstract roles and dependencies so deployment, policy generation, native-plan
schema, and planner switching can be tested before KV-cache, token streaming,
or a concrete LLM runtime is implemented.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .plan import InferenceDependency, ModelFamily, ModelFormat, PlannerKind
from .planner_registry import (
    PlannerBackend,
    PlannerBackendRegistry,
    PlannerRequest,
    PlannerResult,
)
from .splitter import SplitServiceSpec, SplitterOutput


DEFAULT_LLM_SERVICE = "/AI/LLM/StubInference"
DEFAULT_LLM_CONTROLLER = "/NDNSF-DistributeInference/example/controller"
DEFAULT_LLM_GROUP = "/NDNSF-DistributeInference/example/group"
DEFAULT_LLM_USER = "/NDNSF-DistributeInference/example/user"
DEFAULT_LLM_PROVIDER_PREFIX = "/NDNSF-DistributeInference/example/provider"


@dataclass(frozen=True)
class LlmPlannerShape:
    roles: list[str]
    dependencies: list[InferenceDependency]
    description: str


def _positive_int(value: Any, default: int) -> int:
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        parsed = default
    return max(1, parsed)


def _llm_pipeline_shape(stages: int) -> LlmPlannerShape:
    roles = [f"/LLM/Pipeline/Stage/{index}" for index in range(stages)]
    dependencies = [
        InferenceDependency(
            producers=[roles[index]],
            consumers=[roles[index + 1]],
            key_scope=f"pipeline-stage-{index}-to-{index + 1}",
            topic_prefix="/activation/llm",
            tensors=["hidden-state"],
            object_name_template=(
                "{producerProvider}/NDNSF/DI/ACTIVATION/"
                "{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}"
            ),
        )
        for index in range(stages - 1)
    ]
    return LlmPlannerShape(
        roles=roles,
        dependencies=dependencies,
        description="abstract LLM pipeline parallel plan",
    )


def _llm_prefill_decode_shape() -> LlmPlannerShape:
    roles = ["/LLM/Prefill", "/LLM/Decode"]
    return LlmPlannerShape(
        roles=roles,
        dependencies=[
            InferenceDependency(
                producers=["/LLM/Prefill"],
                consumers=["/LLM/Decode"],
                key_scope="prefill-to-decode",
                topic_prefix="/activation/llm",
                tensors=["kv-cache", "last-token-state"],
                object_name_template=(
                    "{producerProvider}/NDNSF/DI/ACTIVATION/"
                    "{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}"
                ),
            )
        ],
        description="abstract LLM prefill/decode split plan",
    )


def _llm_tensor_parallel_shape(shards: int) -> LlmPlannerShape:
    shard_roles = [f"/LLM/TensorShard/{index}" for index in range(shards)]
    merge_role = "/LLM/TensorMerge"
    return LlmPlannerShape(
        roles=[*shard_roles, merge_role],
        dependencies=[
            InferenceDependency(
                producers=[role],
                consumers=[merge_role],
                key_scope=f"tensor-shard-{index}-to-merge",
                topic_prefix="/activation/llm",
                tensors=["partial-logits"],
                object_name_template=(
                    "{producerProvider}/NDNSF/DI/ACTIVATION/"
                    "{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}"
                ),
            )
            for index, role in enumerate(shard_roles)
        ],
        description="abstract LLM tensor-parallel shard plan",
    )


def llm_stub_plan_from_request(request: PlannerRequest) -> PlannerResult:
    planner_kind = request.normalized_planner_kind()
    stages = _positive_int(request.option("stages", 2), 2)
    shards = _positive_int(request.option("shards", 2), 2)

    if planner_kind == PlannerKind.LLM_PIPELINE.value:
        shape = _llm_pipeline_shape(stages)
    elif planner_kind == PlannerKind.LLM_PREFILL_DECODE.value:
        shape = _llm_prefill_decode_shape()
    elif planner_kind == PlannerKind.LLM_TENSOR_PARALLEL.value:
        shape = _llm_tensor_parallel_shape(shards)
    else:
        raise ValueError(f"unsupported LLM planner kind: {planner_kind}")

    split_plan = {
        "service": str(request.option("service", DEFAULT_LLM_SERVICE)),
        "model": str(request.model_path),
        "model_family": request.normalized_model_family(),
        "model_format": request.normalized_model_format(),
        "planner_kind": planner_kind,
        "roles": list(shape.roles),
        "dependencies": list(shape.dependencies),
        "layout": request.layout or planner_kind,
        "description": shape.description,
    }
    return PlannerResult(
        request=request,
        split_plan=split_plan,
        score_summary={
            "roleCount": len(shape.roles),
            "dependencyCount": len(shape.dependencies),
            "executionImplemented": False,
        },
        selected_candidate={
            "mode": planner_kind,
            "selected": True,
        },
        metadata={
            "stub": True,
            "executionImplemented": False,
            "modelFormat": request.normalized_model_format(),
        },
    )


def llm_planner_registry() -> PlannerBackendRegistry:
    registry = PlannerBackendRegistry()
    for planner_kind, name in (
        (PlannerKind.LLM_PIPELINE, "LLM pipeline stub planner"),
        (PlannerKind.LLM_PREFILL_DECODE, "LLM prefill/decode stub planner"),
        (PlannerKind.LLM_TENSOR_PARALLEL, "LLM tensor-parallel stub planner"),
    ):
        registry.register(PlannerBackend(
            planner_kind=planner_kind,
            model_family=ModelFamily.LLM,
            model_format=ModelFormat.UNKNOWN,
            name=name,
            description="Stub LLM planner; emits abstract roles only.",
            metadata={"stub": True},
            handler=llm_stub_plan_from_request,
        ))
    return registry


def llm_planner_request(
    *,
    planner_kind: str | PlannerKind = PlannerKind.LLM_PIPELINE,
    model_path: str,
    output_dir: str | Path,
    model_format: str | ModelFormat = ModelFormat.HF_TRANSFORMERS,
    service: str = DEFAULT_LLM_SERVICE,
    stages: int = 2,
    shards: int = 2,
) -> PlannerRequest:
    return PlannerRequest(
        planner_kind=planner_kind,
        model_family=ModelFamily.LLM,
        model_format=model_format,
        model_path=model_path,
        output_dir=str(output_dir),
        layout=str(planner_kind.value if isinstance(planner_kind, PlannerKind) else planner_kind),
        options={
            "service": service,
            "stages": int(stages),
            "shards": int(shards),
        },
    )


def llm_splitter_output_from_result(
    result: PlannerResult,
    *,
    application: str = "llm-stub-demo",
    controller: str = DEFAULT_LLM_CONTROLLER,
    group: str = DEFAULT_LLM_GROUP,
    user: str = DEFAULT_LLM_USER,
    provider_prefix: str = DEFAULT_LLM_PROVIDER_PREFIX,
) -> SplitterOutput:
    split = result.split_plan
    service_name = str(split["service"])
    provider_count = max(1, len(split["roles"]))
    service = SplitServiceSpec(
        name=service_name,
        model_name=str(split["model"]),
        roles=list(split["roles"]),
        dependencies=list(split["dependencies"]),
        input_schema={
            "codec": "llm-token-reference",
            "implemented": False,
        },
        output_schema={
            "codec": "llm-token-stream-reference",
            "implemented": False,
        },
        metadata={
            "model_family": result.normalized_model_family(),
            "model_format": result.request.normalized_model_format(),
            "planner_kind": result.normalized_planner_kind(),
            "execution_plan_schema_version": 2,
            "planner": {
                "modelFamily": result.normalized_model_family(),
                "modelFormat": result.request.normalized_model_format(),
                "plannerKind": result.normalized_planner_kind(),
                "schemaVersion": 2,
                "scoreSummary": dict(result.score_summary),
                "selectedCandidate": dict(result.selected_candidate),
                "stub": True,
            },
            "execution_implemented": False,
            "description": str(split.get("description", "")),
        },
    )
    provider_identities = [
        provider_prefix if index == 0 else f"{provider_prefix}/{index}"
        for index in range(provider_count)
    ]
    return SplitterOutput(
        application=application,
        controller=controller,
        group=group,
        user=user,
        provider_prefix=provider_prefix,
        services=[service],
        provider_identities=provider_identities,
        trust_app_roots=["/example"],
        metadata=dict(service.metadata),
    )
