#!/usr/bin/env python3
"""Build a YOLO 2x2 distributed-inference plan with the APP-level API.

This example intentionally focuses on the application-facing API. It shows how
an AI developer describes a two-stage, two-shard split without importing NDNSF
Core objects. The same builder supports arbitrary role names and dependency
graphs; 2x2 is only one convenient layout.
"""

from __future__ import annotations

from pathlib import Path

from ndnsf_distributed_inference import APPDeployment, InferencePlanBuilder
from ndnsf_distributed_inference.backends.onnxruntime import runtime_spec


SERVICE = "/AI/YOLO/2x2Inference"
CONFIG_FILE = str(Path(__file__).with_name("yolo_policy.yaml"))


def fake_onnx_payload(stage: int, shard: int) -> bytes:
    """Small placeholder artifact for plan/API smoke tests.

    A real application would pass an ONNX shard path or bytes exported by its
    model splitter.
    """

    return f"placeholder-yolo-onnx-stage={stage}-shard={shard}\n".encode()


def build_yolo_2x2_plan():
    deployment = APPDeployment.from_config(
        CONFIG_FILE,
        generated_policy_dir="/tmp/ndnsf-di-yolo-2x2-policy",
    )
    builder = InferencePlanBuilder.for_service(
        deployment.deployment,
        SERVICE,
        runtime=runtime_spec(),
    ).metadata(layout="2-stage x 2-shard", framework="onnxruntime")

    for stage in range(2):
        for shard in range(2):
            builder.add_grid_part(
                stage=stage,
                shard=shard,
                model=fake_onnx_payload(stage, shard),
                filename=f"yolo-stage{stage}-shard{shard}.onnx",
                kind="onnx-model",
                metadata={
                    "tensor_parallel_shard": shard,
                    "pipeline_stage": stage,
                },
            )

    return builder.build(), deployment


def main() -> int:
    plan, deployment = build_yolo_2x2_plan()
    print("APP API plan smoke")
    print("service:", plan.service)
    print("policy trust schema:", deployment.trust_schema)
    print("roles:")
    for role in plan.roles:
        print(f"  {role.role} -> {role.artifact_name}")
    print("dependencies:")
    for dep in plan.dependencies:
        producers = ",".join(dep.producers)
        consumers = ",".join(dep.consumers)
        print(f"  {dep.key_scope}: {producers} -> {consumers} "
              f"topic={dep.topic_prefix}")
    print("provider role dependency views:")
    dependency_graph = deployment.deployment.dependency_graph_for_service(SERVICE)
    for role in [role.role for role in plan.roles]:
        view = dependency_graph.for_role(role)
        print(f"  {role}: "
              f"inputs={[edge.key_scope for edge in view.inputs]} "
              f"outputs={[edge.key_scope for edge in view.outputs]} "
              f"internal={[edge.key_scope for edge in view.internal]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
