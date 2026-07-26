from __future__ import annotations

from datetime import datetime, timedelta, timezone
import json
import tempfile
import unittest

from ndnsf_distributed_inference.api import (
    ArtifactReference, DeploymentConstraints, DeploymentProgress,
    DeploymentRef, DeploymentStatus, InferenceApplication, InferenceClient,
    ModelIntent, OptimizationObjective, RequestContract,
)
from ndnsf_distributed_inference.app_sdk.application import ApplicationDefinitionSigner
from ndnsf_distributed_inference.app_sdk.client import APPClient
from ndnsf_distributed_inference.app_sdk.deployment import DeploymentCatalog
from ndnsf_distributed_inference.app_sdk.runtime_journal import RuntimeJournal
from ndnsf_distributed_inference.app_sdk.status import RequestState
from ndnsf.runtime_telemetry import (
    CollaborationSelectionStatus, ServiceOperationState,
    ServiceOperationStatus,
)


def definition(signer):
    return signer.define(
        deployment_id="demo", deployment_owner="/app/deployment-owner",
        service="/LLM/Test", model_intent=ModelIntent(("approved/model",)),
        artifacts=(ArtifactReference("repo:/model", "sha256:" + "1" * 64, 1),),
        request_contract=RequestContract("prompt/v1", "text/v1"),
        objective=OptimizationObjective("latency"),
        constraints=DeploymentConstraints(minimum_providers=2),
        optimization_profile="default")


class UserJourneyTest(unittest.TestCase):
    def test_application_binds_one_explicit_ensure_owner(self):
        with tempfile.TemporaryDirectory() as root:
            core = APPClient(RuntimeJournal.for_test(root, "requester"))
            catalog = DeploymentCatalog(
                owner_identity="/requester", journal=core.journal)
            client = InferenceClient(core, deployments=catalog)
            signer = ApplicationDefinitionSigner.generate("/app/creator")
            calls = []

            def ensure(value, revision):
                calls.append((value.deployment_id, revision))
                return DeploymentStatus("PREPARING", revision), None

            app = InferenceApplication(signer, client, deployment_manager=ensure)
            value = definition(signer)
            app.deploy(value)
            self.assertEqual(len(calls), 1)
            with self.assertRaises(RuntimeError):
                InferenceApplication(signer, client, deployment_manager=lambda *_: None)

    def test_cold_request_uses_same_ensure_as_prewarm_and_recovers(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "requester")
            core = APPClient(journal, executor=lambda payload: payload.upper())
            ensure_calls = []

            def ensure(value, revision):
                ensure_calls.append((value.digest(), revision))
                progress = tuple(DeploymentProgress(
                    request_id="ensure", attempt=1, revision=revision,
                    role=role, provider=f"/provider/{role}",
                    operation_id=f"prepare:{role}", phase="READY",
                    sequence=6, progress=1.0)
                    for role in ("prefill", "decode"))
                certificate = "sha256:" + "2" * 64
                status = DeploymentStatus(
                    "ACTIVE", revision, progress, certificate, 1)
                ref = DeploymentRef(
                    value.deployment_id, revision, value.service, value.digest(),
                    certificate, "/app/activation", "sha256:" + "3" * 64, 1,
                    application_identity=value.application_identity,
                    deployment_owner=value.deployment_owner,
                    coordinator_service=value.coordinator_service,
                    definition_record_name=(
                        value.application_identity + "/NDNSF/DI/DEFINITION/" +
                        value.deployment_id + "/" + value.digest()),
                    expires_at=(datetime.now(timezone.utc) +
                                timedelta(hours=1)).isoformat())
                return status, ref

            catalog = DeploymentCatalog(
                owner_identity="/requester", journal=journal,
                ensure_deployment=ensure)

            def submit(core, value, revision, input_value, deadline, options):
                payload = json.dumps(input_value, sort_keys=True).encode()
                return core.submit(value.deployment_id, revision, payload)

            client = InferenceClient(
                core, deployments=catalog, request_submitter=submit)
            signer = ApplicationDefinitionSigner.generate("/app/creator")
            app = InferenceApplication(signer, client)
            value = definition(signer)

            request = app.request(
                value, input={"prompt": "hello"},
                timeout=timedelta(seconds=5))
            self.assertEqual(request.status(), RequestState.COMPLETED)
            self.assertEqual(request.deployment_status().state, "ACTIVE")
            self.assertEqual(request.result().payload,
                             b'{"PROMPT": "HELLO"}')
            self.assertEqual(len(ensure_calls), 1)

            prewarm = app.deploy(value)
            self.assertEqual(prewarm.wait_until_active(
                timeout=timedelta(seconds=1)).status().state, "ACTIVE")
            self.assertEqual(len(ensure_calls), 2)

            restarted_core = APPClient(
                RuntimeJournal.for_test(root, "requester"))
            restarted_catalog = DeploymentCatalog(
                owner_identity="/requester", journal=restarted_core.journal)
            restarted = InferenceClient(
                restarted_core, deployments=restarted_catalog)
            rebound = restarted.requests.get(request.ref)
            self.assertEqual(rebound.status(), RequestState.COMPLETED)
            self.assertEqual(rebound.result().payload,
                             b'{"PROMPT": "HELLO"}')

    def test_request_rejects_unsigned_and_ambiguous_timing_before_ensure(self):
        with tempfile.TemporaryDirectory() as root:
            core = APPClient(RuntimeJournal.for_test(root, "requester"))
            client = InferenceClient(core)
            signer = ApplicationDefinitionSigner.generate("/app/creator")
            value = definition(signer)
            with self.assertRaises(ValueError):
                client.request(value, input=b"x")
            with self.assertRaises(ValueError):
                client.request(value, input=b"x", timeout=timedelta(seconds=1),
                               deadline=__import__("datetime").datetime.now(
                                   __import__("datetime").timezone.utc) +
                               timedelta(seconds=1))
            with self.assertRaises(ValueError):
                client.request(
                    __import__("dataclasses").replace(value, signature=""),
                    input=b"x", timeout=timedelta(seconds=1))

    def test_provider_signed_exact_snapshots_project_to_ready(self):
        with tempfile.TemporaryDirectory() as root:
            core = APPClient(
                RuntimeJournal.for_test(root, "requester"),
                executor=lambda payload: payload)
            catalog = DeploymentCatalog(
                owner_identity="/requester", journal=core.journal)

            def submit(core, value, revision, input_value, deadline, options):
                return core.submit(value.deployment_id, revision, bytes(input_value))

            client = InferenceClient(
                core, deployments=catalog, request_submitter=submit)
            signer = ApplicationDefinitionSigner.generate("/app/creator")
            app = InferenceApplication(signer, client)
            value = definition(signer)
            request = app.request(
                value, input=b"hello", timeout=timedelta(seconds=5))
            revision = request.ref.revision

            def snapshot(role, provider):
                details = json.dumps({
                    "schema": "ndnsf-di-preparation-progress-v1",
                    "phase": "READY", "deploymentRevision": revision,
                    "adapter": "onnxruntime", "artifactDigests": [
                        "sha256:" + "9" * 64],
                }, sort_keys=True, separators=(",", ":")).encode()
                member = ServiceOperationStatus(
                    operation_id=f"prepare:{role}", operation="prepare",
                    service_name=value.service, provider_name=provider,
                    request_id=request.ref.request_id, role=role,
                    state=ServiceOperationState.DONE, sequence=6,
                    progress_known=True, progress=1.0,
                    details_schema="ndnsf-di-preparation-progress-v1",
                    details_payload=details)
                return CollaborationSelectionStatus(
                    provider, value.service, request.ref.request_id,
                    f"selection-{role}", "Completed",
                    member_statuses=(member,))

            core.collaboration_status = lambda *args, **kwargs: (
                snapshot("prefill", "/provider/a"),
                snapshot("decode", "/provider/b"))
            status = request.deployment_status()
            self.assertEqual(status.state, "READY")
            self.assertEqual(len(status.roles), 2)
            self.assertTrue(status.readiness_certificate_digest.startswith("sha256:"))

    def test_owner_signed_coordinator_snapshot_projects_remote_progress(self):
        with tempfile.TemporaryDirectory() as root:
            core = APPClient(
                RuntimeJournal.for_test(root, "remote"),
                executor=lambda payload: payload)
            catalog = DeploymentCatalog(
                owner_identity="/remote", journal=core.journal)

            def submit(core, value, revision, input_value, deadline, options):
                return core.submit(value.deployment_id, revision, bytes(input_value))

            client = InferenceClient(
                core, deployments=catalog, request_submitter=submit)
            signer = ApplicationDefinitionSigner.generate("/app/creator")
            app = InferenceApplication(signer, client)
            value = definition(signer)
            request = app.request(
                value, input=b"hello", timeout=timedelta(seconds=5))
            revision = request.ref.revision
            progress = DeploymentProgress(
                request_id=request.ref.request_id, attempt=1,
                revision=revision, role="prefill", provider="/provider/a",
                operation_id="prepare:prefill", phase="LOADING",
                sequence=4, progress=0.7)
            details = json.dumps({
                "schema": "ndnsf-di-coordinator-progress-v1",
                "applicationIdentity": value.application_identity,
                "deploymentOwner": value.deployment_owner,
                "coordinatorService": value.coordinator_service,
                "definitionDigest": value.digest(),
                "deploymentRevision": revision,
                "state": "PREPARING", "phase": "PREPARING",
                "roles": [progress.to_dict()],
                "readinessCertificateDigest": "",
                "coordinatorEpoch": 2,
            }, sort_keys=True, separators=(",", ":")).encode()
            member = ServiceOperationStatus(
                operation_id="coordinate:" + request.ref.request_id,
                operation="ndnsf-di-coordinate",
                service_name=value.coordinator_service,
                provider_name=value.deployment_owner,
                request_id=request.ref.request_id, role="coordinator",
                state=ServiceOperationState.RUNNING, sequence=4,
                progress_known=True, progress=0.7,
                details_schema="ndnsf-di-coordinator-progress-v1",
                details_payload=details)
            snapshot = CollaborationSelectionStatus(
                value.deployment_owner, value.coordinator_service,
                request.ref.request_id, "selection-coordinator", "Running",
                member_statuses=(member,))
            core.collaboration_status = lambda *args, **kwargs: (snapshot,)
            status = request.deployment_status()
            self.assertEqual(status.state, "PREPARING")
            self.assertEqual(status.coordinator_epoch, 2)
            self.assertEqual(status.roles[0].phase.value, "LOADING")
            self.assertEqual(status.roles[0].progress, 0.7)

            forged = CollaborationSelectionStatus(
                "/attacker", value.coordinator_service,
                request.ref.request_id, "selection-forged", "Running",
                member_statuses=(__import__("dataclasses").replace(
                    member, provider_name="/attacker"),))
            core.collaboration_status = lambda *args, **kwargs: (forged,)
            self.assertEqual(request.deployment_status().roles[0].phase.value,
                             "LOADING")

            ready_roles = [DeploymentProgress(
                request_id=request.ref.request_id, attempt=1,
                revision=revision, role=role, provider=provider,
                operation_id=f"prepare:{role}", phase="READY",
                sequence=6, progress=1.0)
                for role, provider in (
                    ("prefill", "/provider/a"), ("decode", "/provider/b"))]
            ready_details = json.dumps({
                "schema": "ndnsf-di-coordinator-progress-v1",
                "applicationIdentity": value.application_identity,
                "deploymentOwner": value.deployment_owner,
                "coordinatorService": value.coordinator_service,
                "definitionDigest": value.digest(),
                "deploymentRevision": revision,
                "state": "READY", "phase": "READY",
                "roles": [item.to_dict() for item in ready_roles],
                "readinessCertificateDigest": "sha256:" + "7" * 64,
                "coordinatorEpoch": 2,
            }, sort_keys=True, separators=(",", ":")).encode()
            ready_member = __import__("dataclasses").replace(
                member, state=ServiceOperationState.DONE,
                sequence=5, progress=1.0, details_payload=ready_details)
            ready_snapshot = CollaborationSelectionStatus(
                value.deployment_owner, value.coordinator_service,
                request.ref.request_id, "selection-coordinator", "Completed",
                member_statuses=(ready_member,))
            core.collaboration_status = lambda *args, **kwargs: (ready_snapshot,)
            ready = request.deployment_status()
            self.assertEqual(ready.state, "READY")
            self.assertEqual(len(ready.roles), 2)
            self.assertTrue(ready.readiness_certificate_digest.startswith("sha256:"))


if __name__ == "__main__":
    unittest.main()
