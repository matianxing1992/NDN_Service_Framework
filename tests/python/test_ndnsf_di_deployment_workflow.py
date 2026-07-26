from __future__ import annotations

import tempfile
import unittest
import json

from ndnsf_distributed_inference.app_sdk import (
    APPDeployment, APPProvider, ArtifactReference, DeploymentDefinition,
    DeploymentPlan, DeploymentRevision, ProviderEvidenceSigner, ProviderEvidenceVerifier,
    ProviderReadiness,
)
from ndnsf_distributed_inference.app_sdk.runtime_journal import (
    RuntimeJournal as RuntimeJournalImpl,
)
from ndnsf_distributed_inference.core.ports import ExecutionTargetProposal
from ndnsf_distributed_inference.sdk.adapters import RunnerAdapterRegistry
from ndnsf_distributed_inference.app_sdk.status import RevisionState


def RuntimeJournal(state_root, identity, *, quota_bytes=64 << 20):
    return RuntimeJournalImpl.for_test(
        state_root, identity, quota_bytes=quota_bytes)


def definition(model="qwen-7b"):
    return DeploymentDefinition(
        "demo", model, (ArtifactReference("file:///project/model", "sha256:"+"a"*64, 10, "/models/qwen"),),
        ("prefill","decode"), {"precision":"fp16"})


class Adapter:
    name = "runner"
    version = "1"

    def supports(self, target):
        return target.device == "cpu"

    def create_runner(self, target, artifacts):
        return object()


def signed_readiness(revision, *, fail_role="", signer=None):
    signer = signer or ProviderEvidenceSigner.generate()
    verifier = ProviderEvidenceVerifier({signer.key_id: signer.public_pem()})
    registry = RunnerAdapterRegistry()
    registry.register(Adapter())
    provider = APPProvider(
        "/provider", registry, signer=signer, signer_key_id=signer.key_id)
    provider.register_agent(
        boot_epoch="boot-1", capabilities=("cpu",),
        capacity_by_role={"prefill": 1, "decode": 1}, permission_ready=True)
    artifacts = tuple(item.digest for item in revision.definition.artifacts)
    receipts = tuple(provider.stage(
        revision.revision,
        ExecutionTargetProposal(
            role, "/provider", "missing" if role == fail_role else "runner", "cpu"),
        artifacts,
    ) for role in revision.definition.roles)
    return receipts, verifier, provider, signer


class DeploymentWorkflowTest(unittest.TestCase):
    def test_canonical_plan_writer_and_bounded_legacy_import(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal(root, "operator")
            plan = APPDeployment(journal).resolve(definition())
            self.assertIsInstance(plan, DeploymentPlan)
            records = journal.records()
            self.assertEqual(records[-1]["kind"], "deployment-plan-v2")
            self.assertEqual(records[-1]["payload"]["planDigest"],
                             plan.plan_digest)
            self.assertFalse(any(item["kind"] == "deployment-state"
                                 for item in records))

        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal(root, "operator")
            plan = DeploymentPlan.resolve(definition())
            journal.append("deployment-state", {
                "deploymentId": plan.deployment_id,
                "revision": plan.plan_digest,
                "epoch": plan.lifecycle_epoch,
                "state": RevisionState.RESOLVED.value,
                "action": "RESOLVE",
                "definition": plan.definition.to_dict(),
                "providerReadiness": [],
                "providerActionReceipts": [],
            })
            imported = APPDeployment(journal)
            self.assertEqual(imported.legacy_import_count, 1)
            self.assertEqual(journal.records()[-1]["kind"],
                             "deployment-plan-v2")
            reopened = APPDeployment(journal)
            self.assertEqual(reopened.legacy_import_count, 0)

    def test_round_trip_digest_stability_and_secret_rejection(self):
        item=definition(); self.assertEqual(item.digest(), item.digest())
        revision=DeploymentRevision.resolve(item); self.assertEqual(revision.definition_digest,item.digest())
        with self.assertRaisesRegex(ValueError,"secret"):
            DeploymentDefinition("d","m",item.artifacts,("r",),{"token":"bad"})

    def test_validate_resolve_plan_apply_restart_rollback_drain_delete(self):
        with tempfile.TemporaryDirectory() as root:
            probe=APPDeployment(RuntimeJournal(root,"operator")); revision=probe.resolve(definition())
            readiness, verifier, provider, signer = signed_readiness(revision)
            activations = tuple(provider.activate(item) for item in readiness)
            app=APPDeployment(RuntimeJournal(root,"operator"), readiness_verifier=verifier)
            self.assertEqual(app.plan(revision).status,"DRY_RUN")
            self.assertEqual(app.apply(
                revision, readiness=readiness,
                activation_receipts=activations).status,"ACTIVE")
            self.assertEqual(app.apply(revision).status,"ACTIVE")
            restarted=APPDeployment(RuntimeJournal(root,"operator"), readiness_verifier=verifier)
            self.assertEqual(restarted.status("demo"),RevisionState.ACTIVE)
            preview = DeploymentRevision.resolve(definition("qwen-7b"), epoch=2)
            rollback_readiness, _, rollback_provider, _ = signed_readiness(
                preview, signer=signer)
            rollback_activations = tuple(
                rollback_provider.activate(receipt)
                for receipt in rollback_readiness)
            newer=restarted.rollback(
                definition("qwen-7b"), readiness=rollback_readiness,
                activation_receipts=rollback_activations)
            self.assertEqual(newer.lifecycle_epoch,2)
            self.assertEqual(newer.revision, preview.revision)
            self.assertEqual(restarted.drain(
                "demo", action_receipts=rollback_provider.drain()).status,
                RevisionState.INACTIVE.value)
            restarted_again=APPDeployment(
                RuntimeJournal(root,"operator"), readiness_verifier=verifier)
            self.assertEqual(restarted_again.status("demo"),RevisionState.INACTIVE)
            self.assertEqual(restarted_again.delete(
                "demo", action_receipts=rollback_provider.delete(preview.revision)).status,
                RevisionState.DELETED.value)

    def test_apply_handle_reopens_and_restart_reconciles_authenticated_evidence(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal(root, "operator")
            probe = APPDeployment(journal)
            revision = probe.resolve(definition())
            readiness, verifier, provider, _ = signed_readiness(revision)
            activations = tuple(provider.activate(item) for item in readiness)
            app = APPDeployment(journal, readiness_verifier=verifier)

            operation = app.apply(
                revision, readiness=readiness,
                activation_receipts=activations,
                idempotency_key="apply-demo-v1",
            )
            self.assertEqual(operation.lifecycle_epoch, 1)
            self.assertTrue(operation.event_cursor)
            self.assertEqual(
                app.open_operation(operation.operation_id), operation)

            restarted = APPDeployment(
                RuntimeJournal(root, "operator"), readiness_verifier=verifier)
            reconciled = restarted.reconcile(
                revision, readiness=readiness,
                idempotency_key="reconcile-demo-v1",
            )
            self.assertEqual(reconciled.status, "ACTIVE")
            self.assertEqual(reconciled.lifecycle_epoch, 1)
            self.assertEqual(
                restarted.open_operation(reconciled.operation_id), reconciled)

    def test_reconcile_without_observed_evidence_is_unknown_not_inactive(self):
        with tempfile.TemporaryDirectory() as root:
            app = APPDeployment(RuntimeJournal(root, "operator"))
            revision = app.resolve(definition())
            operation = app.reconcile(revision, readiness=())
            self.assertEqual(operation.status, "UNKNOWN")
            self.assertEqual(app.status("demo"), RevisionState.RESOLVED)

    def test_readiness_failure_is_terminal_for_wait(self):
        with tempfile.TemporaryDirectory() as root:
            probe=APPDeployment(RuntimeJournal(root,"operator")); rev=probe.resolve(definition())
            readiness, verifier, _, _ = signed_readiness(rev, fail_role="decode")
            app=APPDeployment(RuntimeJournal(root,"operator"), readiness_verifier=verifier)
            self.assertEqual(app.apply(rev,readiness=readiness).status,"FAILED")
            self.assertEqual(app.wait("demo",timeout_ms=10),RevisionState.FAILED)

    def test_apply_without_authenticated_provider_readiness_fails_closed(self):
        with tempfile.TemporaryDirectory() as root:
            app = APPDeployment(RuntimeJournal(root, "operator"))
            revision = app.resolve(definition())
            with self.assertRaisesRegex(ValueError, "authenticated Provider readiness"):
                app.apply(revision)

    def test_apply_accepts_complete_authenticated_revision_readiness(self):
        with tempfile.TemporaryDirectory() as root:
            probe = APPDeployment(RuntimeJournal(root, "operator"))
            revision = probe.resolve(definition())
            receipts, verifier, provider, _ = signed_readiness(revision)
            activations = tuple(provider.activate(item) for item in receipts)
            app = APPDeployment(
                RuntimeJournal(root, "operator"), readiness_verifier=verifier)
            self.assertEqual(
                app.apply(
                    revision, readiness=receipts,
                    activation_receipts=activations).status, "ACTIVE")
            self.assertEqual(
                ProviderReadiness.from_dict(json.loads(json.dumps(
                    receipts[0].to_dict()))),
                receipts[0],
            )

    def test_drain_without_authenticated_provider_action_fails_closed(self):
        with tempfile.TemporaryDirectory() as root:
            probe = APPDeployment(RuntimeJournal(root, "operator"))
            revision = probe.resolve(definition())
            receipts, verifier, provider, _ = signed_readiness(revision)
            activations = tuple(provider.activate(item) for item in receipts)
            app = APPDeployment(
                RuntimeJournal(root, "operator"), readiness_verifier=verifier)
            app.apply(
                revision, readiness=receipts,
                activation_receipts=activations)
            with self.assertRaisesRegex(ValueError, "authenticated Provider DRAIN"):
                app.drain(revision.deployment_id)

    def test_apply_readiness_without_authenticated_activation_fails_closed(self):
        with tempfile.TemporaryDirectory() as root:
            probe = APPDeployment(RuntimeJournal(root, "operator"))
            revision = probe.resolve(definition())
            receipts, verifier, _, _ = signed_readiness(revision)
            app = APPDeployment(
                RuntimeJournal(root, "operator"), readiness_verifier=verifier)
            with self.assertRaisesRegex(ValueError, "authenticated Provider ACTIVATE"):
                app.apply(revision, readiness=receipts)


if __name__=="__main__": unittest.main()
