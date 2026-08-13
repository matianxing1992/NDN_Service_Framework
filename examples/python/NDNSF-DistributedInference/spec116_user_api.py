#!/usr/bin/env python3
"""Canonical Spec 116 creator flow; configuration supplies the NDN runtime."""

from datetime import timedelta

from ndnsf_distributed_inference.api import (
    ArtifactReference, DeploymentConstraints, InferenceApplication,
    ModelIntent, OptimizationObjective, RequestContract,
)


def build_definition(application: InferenceApplication):
    return application.define(
        deployment_id="qwen-demo",
        deployment_owner="/ndnsf/apps/qwen/deployment-owner",
        service="/LLM/Qwen/Generate",
        model_intent=ModelIntent(("Qwen/approved-v1",)),
        artifacts=(ArtifactReference(
            "repo:/models/qwen", "sha256:" + "a" * 64, 0),),
        request_contract=RequestContract("prompt/v1", "generated-text/v1"),
        objective=OptimizationObjective("latency"),
        constraints=DeploymentConstraints(
            minimum_providers=2,
            allowed_partition_kinds=("layer-range",)),
        optimization_profile="default")


def run(config: str, state_root: str, envelope_key_file: str):
    application = InferenceApplication.from_config(
        config, state_root=state_root,
        envelope_key_file=envelope_key_file)
    request = application.request_preplanned(
        build_definition(application),
        input={"prompt": "Explain NDN in one sentence."},
        timeout=timedelta(minutes=6))
    return request.result()


__all__ = ["build_definition", "run"]
