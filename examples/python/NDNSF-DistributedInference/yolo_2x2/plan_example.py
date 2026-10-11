#!/usr/bin/env python3
"""Inspect the YOLO 2x2 distributed-inference service policy."""

from __future__ import annotations

from pathlib import Path

from ndnsf_distributed_inference.app_sdk import APPDeployment


SERVICE = "/AI/YOLO/2x2Inference"
CONFIG_FILE = str(Path(__file__).with_name("yolo_policy.yaml"))


def inspect_yolo_2x2_policy():
    deployment = APPDeployment.from_config(
        CONFIG_FILE,
        generated_policy_dir="/tmp/ndnsf-di-yolo-2x2-policy",
    )
    service = deployment.deployment.service_policy(SERVICE)
    return service, deployment


def main() -> int:
    service, deployment = inspect_yolo_2x2_policy()
    print("APP API service policy smoke")
    print("service:", service.name)
    print("policy trust schema:", deployment.deployment.trust_schema)
    print("roles:")
    for role in service.roles:
        print(" ", role)
    print("dependencies:")
    for dep in service.dependencies:
        producers = ",".join(dep.producers)
        consumers = ",".join(dep.consumers)
        print(f"  {dep.key_scope}: {producers} -> {consumers} "
              f"topic={dep.topic_prefix}")
    print("provider role dependency views:")
    dependency_graph = deployment.deployment.dependency_graph_for_service(SERVICE)
    for role in service.roles:
        view = dependency_graph.for_role(role)
        print(f"  {role}: "
              f"inputs={[edge.key_scope for edge in view.inputs]} "
              f"outputs={[edge.key_scope for edge in view.outputs]} "
              f"internal={[edge.key_scope for edge in view.internal]}")
    print("DISTRIBUTED_INFERENCE_YOLO_2X2_POLICY_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
