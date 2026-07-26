from __future__ import annotations

from dataclasses import replace
from datetime import datetime, timedelta, timezone
import json
from types import SimpleNamespace
import tempfile
import unittest

from ndnsf import SignedAppDataResult
from ndnsf_distributed_inference.api import (
    DeploymentDefinitionRef, DeploymentHandleRef, DeploymentProgress,
    DeploymentRef, DeploymentStatus, InferenceOptions,
)
from ndnsf_distributed_inference.app_sdk.application import ApplicationDefinitionSigner
from ndnsf_distributed_inference.app_sdk.client import APPClient, InferenceClient
from ndnsf_distributed_inference.app_sdk.coordinator import (
    InferenceCoordinator, decode_coordinator_request,
    encode_coordinator_request, encode_coordinator_response,
)
from ndnsf_distributed_inference.client import InferenceResult
from ndnsf_distributed_inference.app_sdk.contracts import DeploymentRevision
from ndnsf_distributed_inference.app_sdk.deployment import (
    DeploymentCatalog, NetworkDeploymentCatalogTransport,
)
from ndnsf_distributed_inference.app_sdk.runtime_journal import RuntimeJournal
from ndnsf_distributed_inference.app_sdk.status import RequestState
from test_ndnsf_di_user_journey import definition


class _SignedDataBusUser:
    def __init__(self, identity, records, hints=()):
        self.identity = identity
        self.records = records
        self.hints = list(hints)

    def publish_signed_app_data(self, name, payload, *, freshness_ms):
        del freshness_ms
        prefix = self.identity.rstrip("/") + "/NDNSF/DI/"
        if not name.startswith(prefix):
            return SignedAppDataResult(False, error="wrong publisher prefix")
        self.records[name] = (self.identity, bytes(payload))
        return SignedAppDataResult(True, data_name=name)

    def fetch_signed_app_data(self, name, expected_signer, *, timeout_ms):
        del timeout_ms
        value = self.records.get(name)
        if value is None:
            return SignedAppDataResult(False, error="not found")
        signer, payload = value
        if signer != expected_signer:
            return SignedAppDataResult(False, error="signer mismatch")
        return SignedAppDataResult(
            True, data_name=name,
            signer_certificate=signer + "/KEY/test/issuer/v=1",
            payload=payload)

    def get_ndnsd_services(self):
        return [{"serviceMetaInfo": {"deployments": json.dumps(self.hints)}}]


class _CoordinatorLoopbackUser:
    def __init__(self):
        self.calls = []

    def request_collaboration_async(self, service, payload, *, roles,
                                    key_scopes, dependencies, on_response,
                                    on_timeout, ack_timeout_ms, timeout_ms,
                                    request_id):
        del on_timeout, ack_timeout_ms, timeout_ms, key_scopes, dependencies
        assert roles == [{
            "role": "coordinator", "service": service,
            "min_providers": 1, "max_providers": 1,
        }]
        definition_ref, input_payload, _, _, outer_id = (
            decode_coordinator_request(payload))
        assert request_id == outer_id
        self.calls.append((service, definition_ref, input_payload, outer_id))
        result = InferenceResult(
            True, input_payload.upper(), "", "inner-request")
        on_response(SimpleNamespace(
            status=True,
            payload=encode_coordinator_response(result),
            error=""))


class _CoordinatorClient:
    def __init__(self):
        self.calls = []

    def _request_as_coordinator(self, definition, **kwargs):
        self.calls.append((definition, kwargs))
        revision = definition.definition_digest
        progress = DeploymentProgress(
            request_id="inner", attempt=1, revision=revision,
            role="prefill", provider="/provider/a",
            operation_id="prepare:prefill", phase="READY",
            sequence=6, progress=1.0)
        return SimpleNamespace(
            ref=SimpleNamespace(
                request_id="inner", revision=revision, attempt_epoch=1),
            status=lambda: RequestState.COMPLETED,
            deployment_status=lambda: DeploymentStatus(
                "READY", revision, (progress,), "sha256:" + "8" * 64, 1),
            result=lambda **ignored: InferenceResult(
                True, bytes(kwargs["input"]).upper(), "", "inner"))


class DeploymentCatalogTest(unittest.TestCase):
    def test_on_demand_discovery_returns_typed_signed_reference(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "requester")
            catalog = DeploymentCatalog(owner_identity="/requester", journal=journal)
            signer = ApplicationDefinitionSigner.generate("/app/creator")
            catalog.authorize_application(
                signer.application_identity, signer.key_id, signer.public_key)
            value = definition(signer)
            ref = catalog.publish_definition(value)
            self.assertIsInstance(ref, DeploymentDefinitionRef)
            summary = catalog.discover(service=value.service)
            self.assertEqual(len(summary), 1)
            self.assertEqual(summary[0].state, "ON_DEMAND")
            self.assertEqual(summary[0].deployment, ref)
            resolved, revision = catalog.resolve_definition(ref)
            self.assertEqual(resolved, value)
            self.assertTrue(revision.startswith("sha256:"))

    def test_forged_hint_and_restart_fail_closed_or_rebind_exactly(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "requester")
            catalog = DeploymentCatalog(owner_identity="/requester", journal=journal)
            signer = ApplicationDefinitionSigner.generate("/app/creator")
            catalog.authorize_application(
                signer.application_identity, signer.key_id, signer.public_key)
            value = definition(signer)
            ref = catalog.publish_definition(value)
            with self.assertRaises(ValueError):
                catalog.resolve_definition(replace(ref, coordinator_service="/attacker"))
            with self.assertRaises(ValueError):
                catalog.resolve_definition(replace(
                    ref, record_name=ref.record_name + "/alias"))
            with self.assertRaises(ValueError):
                catalog.resolve_definition(replace(
                    ref, expires_at="2099-01-01T00:00:00+00:00"))

            restored = DeploymentCatalog(
                owner_identity="/requester",
                journal=RuntimeJournal.for_test(root, "requester"))
            restored.authorize_application(
                signer.application_identity, signer.key_id, signer.public_key)
            self.assertEqual(restored.resolve_definition(ref)[0], value)

    def test_self_signed_but_unauthorized_definition_is_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            catalog = DeploymentCatalog(
                owner_identity="/requester",
                journal=RuntimeJournal.for_test(root, "requester"))
            signer = ApplicationDefinitionSigner.generate("/attacker")
            with self.assertRaises(PermissionError):
                catalog.publish_definition(definition(signer))

    def test_remote_on_demand_hint_fetches_exact_signed_definition(self):
        with tempfile.TemporaryDirectory() as root:
            records = {}
            signer = ApplicationDefinitionSigner.generate("/app/creator")
            value = definition(signer)
            creator_transport = NetworkDeploymentCatalogTransport(
                _SignedDataBusUser("/app/creator", records))
            creator = DeploymentCatalog(
                owner_identity="/app/creator",
                journal=RuntimeJournal.for_test(root, "creator"),
                definition_publisher=creator_transport.publish_definition)
            creator.authorize_application(
                signer.application_identity, signer.key_id, signer.public_key)
            ref = creator.publish_definition(value)

            hint = {
                "applicationIdentity": value.application_identity,
                "deploymentOwner": value.deployment_owner,
                "coordinatorService": value.coordinator_service,
                "deploymentId": value.deployment_id,
                "serviceName": value.service,
                "definitionRecordName": ref.record_name,
                "definitionRecordDigest": ref.definition_digest,
                "expiresAt": ref.expires_at,
                "signerKeyId": ref.signer_key_id,
            }
            remote_user = _SignedDataBusUser("/remote/requester", records, [hint])
            transport = NetworkDeploymentCatalogTransport(remote_user)
            remote = DeploymentCatalog(
                owner_identity="/remote/requester",
                journal=RuntimeJournal.for_test(root, "remote"),
                definition_fetcher=transport.fetch_definition,
                activation_fetcher=transport.fetch_activation,
                discovery_hints=transport.discovery_hints)
            values = remote.discover(service=value.service)
            self.assertEqual(len(values), 1)
            self.assertEqual(values[0].state, "ON_DEMAND")
            self.assertEqual(remote.resolve_definition(values[0].deployment)[0], value)

            remote_user.hints = [{**hint, "coordinatorService": "/attacker"}]
            isolated = DeploymentCatalog(
                owner_identity="/remote/requester",
                definition_fetcher=transport.fetch_definition,
                activation_fetcher=transport.fetch_activation,
                discovery_hints=transport.discovery_hints)
            self.assertEqual(isolated.discover(service=value.service), ())

    def test_remote_active_requires_owner_signed_activation_and_exact_revision(self):
        with tempfile.TemporaryDirectory() as root:
            records = {}
            signer = ApplicationDefinitionSigner.generate("/app/creator")
            value = definition(signer)
            creator_transport = NetworkDeploymentCatalogTransport(
                _SignedDataBusUser("/app/creator", records))
            creator = DeploymentCatalog(
                owner_identity="/app/creator",
                definition_publisher=creator_transport.publish_definition)
            creator.authorize_application(
                signer.application_identity, signer.key_id, signer.public_key)
            ref = creator.publish_definition(value)
            revision = DeploymentRevision.resolve(value).revision
            owner_user = _SignedDataBusUser(value.deployment_owner, records)
            owner_transport = NetworkDeploymentCatalogTransport(owner_user)
            owner = DeploymentCatalog(
                owner_identity=value.deployment_owner,
                definition_fetcher=owner_transport.fetch_definition,
                activation_publisher=owner_transport.publish_activation)
            owner.resolve_definition(ref)
            active_ref = owner.publish_activation(
                value, DeploymentStatus(
                    "ACTIVE", revision, (), "sha256:" + "a" * 64, 1))
            activation_name = active_ref.activation_record_name
            activation_digest = active_ref.activation_record_digest
            hint = {
                "applicationIdentity": value.application_identity,
                "deploymentOwner": value.deployment_owner,
                "coordinatorService": value.coordinator_service,
                "deploymentId": value.deployment_id,
                "serviceName": value.service,
                "definitionRecordName": ref.record_name,
                "definitionRecordDigest": ref.definition_digest,
                "expiresAt": ref.expires_at,
                "signerKeyId": ref.signer_key_id,
                "activationRecordName": activation_name,
                "activationRecordDigest": activation_digest,
            }
            remote_user = _SignedDataBusUser("/remote/requester", records, [hint])
            transport = NetworkDeploymentCatalogTransport(remote_user)
            remote = DeploymentCatalog(
                owner_identity="/remote/requester",
                journal=RuntimeJournal.for_test(root, "remote-active"),
                definition_fetcher=transport.fetch_definition,
                activation_fetcher=transport.fetch_activation,
                discovery_hints=transport.discovery_hints)
            values = remote.discover(service=value.service)
            self.assertEqual(len(values), 1)
            self.assertEqual(values[0].state, "ACTIVE")
            self.assertIsInstance(values[0].deployment, DeploymentRef)
            handle = remote.get(values[0].deployment)
            self.assertEqual(handle.status().state, "ACTIVE")
            restored = DeploymentCatalog(
                owner_identity="/remote/requester",
                journal=RuntimeJournal.for_test(root, "remote-active"))
            rebound = restored.get(values[0].deployment)
            self.assertEqual(rebound.status().state, "ACTIVE")
            self.assertEqual(
                restored.resolve_definition(values[0].deployment)[0], value)

            expired = replace(
                active_ref,
                expires_at=(datetime.now(timezone.utc) -
                            timedelta(seconds=1)).isoformat())
            key = (value.deployment_id, revision)
            remote._active_refs[key] = expired
            with self.assertRaises(ValueError):
                remote.resolve_definition(expired)
            self.assertEqual(remote.discover(service=value.service)[0].state,
                             "ON_DEMAND")
            self.assertEqual(remote.status(DeploymentHandleRef(
                value.deployment_id, revision, 1, value.deployment_owner,
                "journal:test", "sha256:" + "f" * 64)).state, "INACTIVE")

            # Restore the live ACTIVE value before publishing its revocation.
            remote._active_refs[key] = active_ref
            remote._statuses[key] = DeploymentStatus(
                "ACTIVE", revision, (), "sha256:" + "a" * 64, 1)

            owner.revoke_activation(value)
            revoked = owner._activation_records[(value.deployment_id, revision)]
            remote_user.hints = [{
                **hint,
                "activationRecordName": revoked.record_name,
                "activationRecordDigest": revoked.digest(),
            }]
            revoked_values = remote.discover(service=value.service)
            self.assertEqual(revoked_values, ())
            with self.assertRaises(PermissionError):
                remote.resolve_definition(ref)

            # Replaying the older ACTIVE hint cannot cross the revocation fence.
            remote_user.hints = [hint]
            replayed = remote.discover(service=value.service)
            self.assertEqual(replayed, ())

            restarted = DeploymentCatalog(
                owner_identity="/remote/requester",
                journal=RuntimeJournal.for_test(root, "remote-active"))
            self.assertEqual(restarted.discover(service=value.service), ())
            with self.assertRaises(PermissionError):
                restarted.resolve_definition(ref)

    def test_signed_revision_rollover_fences_previous_active_revision(self):
        with tempfile.TemporaryDirectory() as root:
            records = {}
            signer = ApplicationDefinitionSigner.generate("/app/creator")
            first = definition(signer)
            creator_transport = NetworkDeploymentCatalogTransport(
                _SignedDataBusUser(first.application_identity, records))
            creator = DeploymentCatalog(
                owner_identity=first.application_identity,
                definition_publisher=creator_transport.publish_definition)
            creator.authorize_application(
                signer.application_identity, signer.key_id, signer.public_key)
            first_ref = creator.publish_definition(first)
            first_revision = DeploymentRevision.resolve(first).revision

            second = signer.define(
                deployment_id=first.deployment_id,
                deployment_owner=first.deployment_owner,
                service=first.service,
                model_intent=first.model_intent,
                artifacts=first.artifacts,
                request_contract=first.request_contract,
                objective=first.objective,
                constraints=first.constraints,
                optimization_profile=first.optimization_profile,
                metadata={"generation": 2},
                previous_revision=first_revision)
            second_ref = creator.publish_definition(second)
            second_revision = DeploymentRevision.resolve(second).revision

            owner_transport = NetworkDeploymentCatalogTransport(
                _SignedDataBusUser(first.deployment_owner, records))
            journal = RuntimeJournal.for_test(root, "owner")
            owner = DeploymentCatalog(
                owner_identity=first.deployment_owner, journal=journal,
                definition_fetcher=owner_transport.fetch_definition,
                activation_publisher=owner_transport.publish_activation)
            owner.resolve_definition(first_ref)
            active_first = owner.publish_activation(
                first, DeploymentStatus(
                    "ACTIVE", first_revision, (),
                    "sha256:" + "a" * 64, 1))
            owner.resolve_definition(second_ref)
            active_second = owner.publish_activation(
                second, DeploymentStatus(
                    "ACTIVE", second_revision, (),
                    "sha256:" + "b" * 64, 1))
            self.assertEqual(active_second.lifecycle_epoch, 2)
            rollover = owner._activation_records[
                (second.deployment_id, second_revision)]
            first_activation = owner._activation_records[
                (first.deployment_id, first_revision)]
            self.assertEqual(rollover.supersedes, first_activation.digest())
            with self.assertRaises(PermissionError):
                owner.resolve_definition(active_first)

            restarted = DeploymentCatalog(
                owner_identity=first.deployment_owner,
                journal=RuntimeJournal.for_test(root, "owner"))
            with self.assertRaises(PermissionError):
                restarted.resolve_definition(first_ref)
            self.assertEqual(
                restarted.resolve_definition(active_second)[1], second_revision)

    def test_catalog_builds_bounded_ndnsd_hints_without_granting_authority(self):
        with tempfile.TemporaryDirectory() as root:
            signer = ApplicationDefinitionSigner.generate("/app/creator")
            value = definition(signer)
            catalog = DeploymentCatalog(
                owner_identity="/app/creator",
                journal=RuntimeJournal.for_test(root, "creator"))
            catalog.authorize_application(
                signer.application_identity, signer.key_id, signer.public_key)
            ref = catalog.publish_definition(value)
            hint = catalog.discovery_hint(ref)
            self.assertEqual(hint["definitionRecordName"], ref.record_name)
            self.assertEqual(hint["definitionRecordDigest"], ref.definition_digest)
            self.assertEqual(hint["coordinatorService"], value.coordinator_service)
            self.assertNotIn("signature", hint)

            published = []
            provider = SimpleNamespace(publish_service_info=lambda *args: published.append(args))
            catalog.advertise(provider, ref, lifetime_seconds=17)
            self.assertEqual(published[0][0:2], (value.service, 17))
            payload = json.loads(published[0][2]["deployments"])
            self.assertEqual(payload, [hint])

    def test_remote_request_routes_to_bound_coordinator_not_local_policy(self):
        with tempfile.TemporaryDirectory() as root:
            signer = ApplicationDefinitionSigner.generate("/app/creator")
            value = definition(signer)
            user = _CoordinatorLoopbackUser()
            network = SimpleNamespace(
                _client=SimpleNamespace(user=user),
                encode_input=lambda *args, **kwargs: self.fail(
                    "remote input must not use requester-local deployment schema"))
            core = APPClient(
                RuntimeJournal.for_test(root, "remote-requester"),
                network_client=network,
                requester_identity="/remote/requester")
            catalog = DeploymentCatalog(
                owner_identity="/remote/requester", journal=core.journal)
            catalog.authorize_application(
                signer.application_identity, signer.key_id, signer.public_key)
            ref = catalog.publish_definition(value)
            client = InferenceClient(core, deployments=catalog)
            request = client.request(
                ref, input=b"hello", timeout=timedelta(seconds=2))
            self.assertEqual(request.result().payload, b"HELLO")
            self.assertEqual(len(user.calls), 1)
            service, observed_ref, observed_payload, outer_id = user.calls[0]
            self.assertEqual(service, value.coordinator_service)
            self.assertEqual(observed_ref, ref)
            self.assertEqual(observed_payload, b"hello")
            self.assertEqual(outer_id, request.ref.request_id)

    def test_coordinator_realizes_bound_reference_through_local_request_owner(self):
        signer = ApplicationDefinitionSigner.generate("/app/creator")
        value = definition(signer)
        ref = DeploymentCatalog(
            owner_identity="/app/creator",
            trusted_application_keys={
                (signer.application_identity, signer.key_id): signer.public_key,
            }).publish_definition(value)
        client = _CoordinatorClient()
        coordinator = InferenceCoordinator(client, value.coordinator_service)
        wire = encode_coordinator_request(
            ref, b"hello",
            deadline=datetime.now(timezone.utc) + timedelta(seconds=2),
            options=InferenceOptions(),
            outer_request_id="outer")
        response = coordinator.handle(wire)
        decoded = json.loads(response.decode())
        self.assertTrue(decoded["status"])
        self.assertEqual(client.calls[0][1]["coordinator_service"],
                         value.coordinator_service)

    def test_coordinator_reports_signed_projection_before_final_response(self):
        signer = ApplicationDefinitionSigner.generate("/app/creator")
        value = definition(signer)
        ref = DeploymentCatalog(
            owner_identity="/app/creator",
            trusted_application_keys={
                (signer.application_identity, signer.key_id): signer.public_key,
            }).publish_definition(value)
        client = _CoordinatorClient()
        coordinator = InferenceCoordinator(client, value.coordinator_service)
        wire = encode_coordinator_request(
            ref, b"hello",
            deadline=datetime.now(timezone.utc) + timedelta(seconds=2),
            options=InferenceOptions(), outer_request_id="outer")

        class Context:
            statuses = []
            response = b""
            role = "coordinator"
            local_provider = value.deployment_owner

            def report_operation_status(self, status):
                self.statuses.append(status)

            def publish_final_response(self, payload):
                self.response = bytes(payload)

        context = Context()
        coordinator.handle_collaboration(context, wire)
        self.assertEqual(len(context.statuses), 1)
        observed = context.statuses[0]
        self.assertEqual(observed.role, "coordinator")
        self.assertEqual(observed.request_id, "outer")
        details = json.loads(observed.details_payload.decode())
        self.assertEqual(details["state"], "READY")
        self.assertEqual(details["deploymentOwner"], value.deployment_owner)
        self.assertEqual(details["roles"][0]["phase"], "READY")
        self.assertEqual(details["roles"][0]["request_id"], "outer")
        self.assertEqual(json.loads(context.response)["status"], True)

        forged_context = Context()
        forged_context.local_provider = "/attacker"
        with self.assertRaises(PermissionError):
            coordinator.handle_collaboration(forged_context, wire)
        self.assertEqual(len(client.calls), 1)

        registrations = []
        provider = SimpleNamespace(
            add_collaboration_handler=lambda *args, **kwargs: registrations.append(
                (args, kwargs)))
        coordinator.register(provider)
        self.assertEqual(registrations[0][0][1], ["coordinator"])


if __name__ == "__main__":
    unittest.main()
