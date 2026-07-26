#!/usr/bin/env python3
"""Canonical local deployment-to-durable-request workflow."""

from pathlib import Path

from ndnsf_distributed_inference.app_sdk import (
    APPClient, APPDeployment, APPProvider, ProviderEvidenceSigner,
    ProviderEvidenceVerifier, RuntimeJournal,
)
from ndnsf_distributed_inference.core.ports import ExecutionTargetProposal
from ndnsf_distributed_inference.ops.cli import definition_from_json
from ndnsf_distributed_inference.sdk.adapters import RunnerAdapterRegistry


class ExampleRunner:
    name = "example"
    version = "1"

    def supports(self, target): return target.device == "cpu"
    def create_runner(self, target, artifacts): return object()


root = Path("/tmp/ndnsf-di-workflow")
journal = RuntimeJournal.for_test(root, "example")
definition = definition_from_json(Path(__file__).with_name("deployment.json"))
signer = ProviderEvidenceSigner.generate()
verifier = ProviderEvidenceVerifier({signer.key_id: signer.public_pem()})
registry = RunnerAdapterRegistry(); registry.register(ExampleRunner())
provider = APPProvider(
    "/example/provider", registry, signer=signer, signer_key_id=signer.key_id)
provider.register_agent(
    boot_epoch="example-boot-1",
    capacity_by_role={role: 1 for role in definition.roles},
    permission_ready=True)
deployment = APPDeployment(journal, readiness_verifier=verifier)
revision = deployment.resolve(definition)
print(deployment.plan(revision))
artifacts = tuple(item.digest for item in definition.artifacts)
readiness = tuple(provider.stage(
    revision.revision,
    ExecutionTargetProposal(role, "/example/provider", "example", "cpu"),
    artifacts) for role in definition.roles)
activations = tuple(provider.activate(item) for item in readiness)
deployment.apply(
    revision, readiness=readiness, activation_receipts=activations)
client = APPClient(journal, executor=lambda payload: payload)
handle = client.submit(definition.deployment_id, revision.revision, b"model-input")
print(client.wait(handle), client.result(handle))
deployment.drain(
    definition.deployment_id, action_receipts=provider.drain())
deployment.delete(
    definition.deployment_id, action_receipts=provider.delete(revision.revision))
