#!/usr/bin/env python3
"""Validate LLM pipeline plan semantics with an in-memory fake runtime.

This smoke does not execute a real LLM. It proves that the LLM stub planner,
policy compiler, and native execution-plan schema can describe ordered
multi-stage execution in a form a provider runtime can consume.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.llm_stub_planner import (  # noqa: E402
    llm_planner_registry,
    llm_planner_request,
    llm_splitter_output_from_result,
)
from ndnsf_distributed_inference.plan import PlannerKind  # noqa: E402
from ndnsf_distributed_inference.policy import write_policy_bundle  # noqa: E402


@dataclass(frozen=True)
class PlannedDependency:
    producer: str
    consumer: str
    key_scope: str
    tensors: tuple[str, ...]


class InMemoryDependencyStore:
    def __init__(self) -> None:
        self._objects: dict[str, tuple[bytes, PlannedDependency]] = {}
        self.published_objects = 0
        self.fetched_objects = 0

    def publish(self, name: str, payload: bytes, edge: PlannedDependency) -> None:
        if name in self._objects:
            raise RuntimeError(f"duplicate dependency object: {name}")
        self._objects[name] = (bytes(payload), edge)
        self.published_objects += 1

    def fetch(self, name: str) -> bytes:
        try:
            payload, edge = self._objects[name]
        except KeyError as exc:
            raise RuntimeError(f"missing dependency object: {name}") from exc
        if not edge.key_scope or not edge.tensors:
            raise RuntimeError(f"dependency object metadata is incomplete: {name}")
        self.fetched_objects += 1
        return payload


def dependency_name(session_id: str, edge: PlannedDependency) -> str:
    producer = edge.producer.strip("/").replace("/", "_")
    return f"/NDNSF/DI/LLM/{session_id}/{edge.key_scope}/{producer}/bundle/0"


def load_service(native_plan: Path, service_name: str) -> dict[str, Any]:
    doc = json.loads(native_plan.read_text(encoding="utf-8"))
    matches = [
        service for service in doc.get("services", [])
        if service.get("service") == service_name
    ]
    if len(matches) != 1:
        raise RuntimeError(f"expected exactly one service {service_name} in {native_plan}")
    return matches[0]


def planned_dependencies(service: dict[str, Any]) -> list[PlannedDependency]:
    dependencies: list[PlannedDependency] = []
    for raw in service.get("dependencies", []):
        producers = list(raw.get("producers", []))
        consumers = list(raw.get("consumers", []))
        if len(producers) != 1 or len(consumers) != 1:
            raise RuntimeError("LLM pipeline smoke expects single-producer/single-consumer edges")
        dependencies.append(PlannedDependency(
            producer=str(producers[0]),
            consumer=str(consumers[0]),
            key_scope=str(raw.get("keyScope", "")),
            tensors=tuple(str(tensor) for tensor in raw.get("tensors", [])),
        ))
    return dependencies


def validate_pipeline_metadata(service: dict[str, Any], stages: int, layers: int) -> None:
    if service.get("modelFamily") != "llm":
        raise RuntimeError("native plan is not an LLM plan")
    if service.get("plannerKind") != PlannerKind.LLM_PIPELINE.value:
        raise RuntimeError("native plan is not an LLM pipeline plan")
    if service.get("executionMode") != "pipeline-parallel":
        raise RuntimeError("LLM pipeline plan missing executionMode=pipeline-parallel")

    pipeline = service.get("llmPipeline", {})
    if pipeline.get("stageCount") != stages:
        raise RuntimeError("LLM pipeline stageCount metadata mismatch")
    if layers and pipeline.get("layerCount") != layers:
        raise RuntimeError("LLM pipeline layerCount metadata mismatch")

    role_metadata = service.get("roleMetadata", {})
    for index in range(stages):
        role = f"/LLM/Pipeline/Stage/{index}"
        metadata = role_metadata.get(role)
        if not isinstance(metadata, dict):
            raise RuntimeError(f"missing role metadata for {role}")
        if metadata.get("stageIndex") != index:
            raise RuntimeError(f"stageIndex mismatch for {role}")
        if metadata.get("stageCount") != stages:
            raise RuntimeError(f"stageCount mismatch for {role}")
        if metadata.get("roleKind") != "llm-pipeline-stage":
            raise RuntimeError(f"roleKind mismatch for {role}")
        layer_range = metadata.get("layerRange", {})
        if layers:
            expected_start = (index * layers) // stages
            expected_end = ((index + 1) * layers) // stages
            if layer_range.get("start") != expected_start:
                raise RuntimeError(f"layerRange.start mismatch for {role}")
            if layer_range.get("endExclusive") != expected_end:
                raise RuntimeError(f"layerRange.endExclusive mismatch for {role}")


def execute_fake_pipeline(
    service: dict[str, Any],
    *,
    session_id: str,
    input_prompt: bytes,
) -> tuple[bytes, list[str], InMemoryDependencyStore]:
    roles = list(service.get("roles", []))
    edges = planned_dependencies(service)
    outgoing = {edge.producer: edge for edge in edges}
    incoming = {edge.consumer: edge for edge in edges}
    store = InMemoryDependencyStore()
    execution_order: list[str] = []

    current_payload = input_prompt
    for index, role in enumerate(roles):
        if index > 0:
            edge = incoming.get(role)
            if edge is None:
                raise RuntimeError(f"role {role} has no incoming dependency")
            current_payload = store.fetch(dependency_name(session_id, edge))

        execution_order.append(role)
        stage_payload = json.dumps({
            "role": role,
            "stageIndex": index,
            "inputBytes": len(current_payload),
            "inputPreview": current_payload.decode("utf-8", errors="replace")[:80],
        }, sort_keys=True).encode("utf-8")

        edge = outgoing.get(role)
        if edge is not None:
            if edge.tensors != ("hidden-state",):
                raise RuntimeError(f"unexpected LLM pipeline edge tensors: {edge.tensors}")
            store.publish(dependency_name(session_id, edge), stage_payload, edge)
        else:
            return json.dumps({
                "finalRole": role,
                "executionOrder": execution_order,
                "logitsReference": stage_payload.decode("utf-8"),
            }, sort_keys=True).encode("utf-8"), execution_order, store

    raise RuntimeError("pipeline did not produce a final output")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--service", default="/AI/LLM/StubInference")
    parser.add_argument("--model", default="/Model/Qwen2.5-0.5B-GGUF/Stub")
    parser.add_argument("--model-format", default="gguf")
    parser.add_argument("--runtime-backend", default="llama.cpp")
    parser.add_argument("--stages", type=int, default=3)
    parser.add_argument("--layers", type=int, default=24)
    parser.add_argument("--session-id", default="llm-pipeline-smoke-session")
    parser.add_argument("--prompt", default="hello from NDNSF-DI LLM pipeline smoke")
    parser.add_argument("--out-dir", default="/tmp/ndnsf-di-llm-pipeline-smoke")
    args = parser.parse_args()

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    policy = out / "llm_pipeline_policy.yaml"
    generated = out / "generated-policy"

    request = llm_planner_request(
        planner_kind=PlannerKind.LLM_PIPELINE,
        model_path=args.model,
        output_dir=out,
        model_format=args.model_format,
        runtime_backend=args.runtime_backend,
        service=args.service,
        stages=args.stages,
        layers=args.layers,
    )
    result = llm_planner_registry().plan(request)
    llm_splitter_output_from_result(result).write_policy_config(policy)
    deployment = write_policy_bundle(policy, generated)

    native_plan = Path(deployment.native_execution_plan_file)
    service = load_service(native_plan, args.service)
    validate_pipeline_metadata(service, args.stages, args.layers)
    final_payload, execution_order, store = execute_fake_pipeline(
        service,
        session_id=args.session_id,
        input_prompt=args.prompt.encode("utf-8"),
    )
    final_doc = json.loads(final_payload.decode("utf-8"))
    if final_doc.get("finalRole") != f"/LLM/Pipeline/Stage/{args.stages - 1}":
        raise RuntimeError("final output did not come from last pipeline stage")
    if len(execution_order) != args.stages:
        raise RuntimeError("pipeline did not execute every stage exactly once")

    print(
        "LLM_PIPELINE_SMOKE_OK",
        f"stages={args.stages}",
        f"layers={args.layers}",
        f"dependencies={len(service.get('dependencies', []))}",
        f"dependency_objects={store.published_objects}",
        f"final_bytes={len(final_payload)}",
        f"native_plan={native_plan}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
