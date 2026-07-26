"""Single-writer APP deployment lifecycle with monotonic CAS fencing."""

from __future__ import annotations

import base64
import hashlib
import json
import os
from threading import RLock
import time
import warnings
from pathlib import Path
from dataclasses import asdict
from datetime import datetime, timedelta, timezone
from typing import Callable

from ..core.contracts import DeploymentLifecycleRecord
from .contracts import (
    ArtifactReference,
    DeploymentActivationRecord,
    DeploymentDefinition,
    DeploymentDefinitionRef,
    DeploymentHandleRef,
    DeploymentProgress,
    DeploymentRef,
    DeploymentStatus,
    DeploymentSummary,
    DeploymentOperation,
    DeploymentPlan,
)
from .runtime_journal import RuntimeJournal, RuntimeJournalUnsafeRootError
from .status import RevisionState


class APPDeploymentLifecycleStore:
    def __init__(self) -> None:
        self._records: dict[str, DeploymentLifecycleRecord] = {}
        self._actions: dict[str, DeploymentLifecycleRecord] = {}
        self._lock = RLock()

    def apply(self, record: DeploymentLifecycleRecord, *,
              complete_provider_receipts: bool = True) -> DeploymentLifecycleRecord:
        with self._lock:
            repeated = self._actions.get(record.action_digest)
            if repeated is not None:
                if repeated == record:
                    return repeated
                raise ValueError("action digest conflicts with another lifecycle record")
            current = self._records.get(record.deployment_id)
            if current is None:
                if record.lifecycle_epoch != 1 or record.expected_previous_epoch != 0:
                    raise ValueError("initial deployment lifecycle epoch must be one")
            elif not current.accepts_successor(record):
                raise ValueError("deployment owner or lifecycle CAS fence failed")
            if record.desired_action in {"DRAIN", "UNLOAD", "DELETE"} and not complete_provider_receipts:
                raise ValueError("destructive transition requires a complete action certificate")
            self._records[record.deployment_id] = record
            self._actions[record.action_digest] = record
            return record

    def get(self, deployment_id: str) -> DeploymentLifecycleRecord | None:
        with self._lock:
            return self._records.get(deployment_id)


class APPDeployment:
    """Restart-safe APP-owned deployment reconciliation façade."""

    def __init__(self, journal: RuntimeJournal | None, *, network_deployment=None,
                 readiness_verifier=None):
        self.journal = journal
        self._network_deployment = network_deployment
        self._readiness_verifier = readiness_verifier
        self._plans: dict[str, DeploymentPlan] = {}
        self._states: dict[str, RevisionState] = {}
        self._bindings: dict[str, tuple[tuple[str, str, str], ...]] = {}
        self._operations: dict[str, DeploymentOperation] = {}
        self.legacy_import_count = 0
        self._restore()

    @classmethod
    def from_config(
        cls,
        config: str | Path,
        *,
        state_root: str | Path | None = None,
        identity: str = "deployment-operator",
        test_only_allow_ephemeral_state_root: bool = False,
        readiness_verifier=None,
        **network_options,
    ) -> "APPDeployment":
        """Construct the canonical configuration view over the network adapter."""
        from .facades import APPDeployment as NetworkAPPDeployment

        state_root = state_root or os.environ.get("NDNSF_DI_STATE_ROOT")
        if state_root is None:
            raise RuntimeJournalUnsafeRootError(
                "APPDeployment requires an explicit operator persistent state root")
        journal = (
            RuntimeJournal.for_test(state_root, identity)
            if test_only_allow_ephemeral_state_root
            else RuntimeJournal(state_root, identity)
        )
        return cls(
            journal,
            readiness_verifier=readiness_verifier,
            network_deployment=NetworkAPPDeployment.from_config(
                config, **network_options),
        )

    @property
    def deployment(self):
        if self._network_deployment is None:
            raise AttributeError("deployment")
        return self._network_deployment.deployment

    def roles_for_service(self, service: str):
        if self._network_deployment is None:
            raise RuntimeError("network deployment configuration is not loaded")
        return self._network_deployment.roles_for_service(service)

    def dependency_graph(self, service: str):
        if self._network_deployment is None:
            raise RuntimeError("network deployment configuration is not loaded")
        return self._network_deployment.dependency_graph(service)

    def model_name_for_service(self, service: str):
        if self._network_deployment is None:
            raise RuntimeError("network deployment configuration is not loaded")
        return self._network_deployment.model_name_for_service(service)

    def __getattr__(self, name):
        network_deployment = self.__dict__.get("_network_deployment")
        if network_deployment is not None:
            return getattr(network_deployment, name)
        raise AttributeError(name)

    def _restore(self):
        if self.journal is None:
            return
        records = self.journal.records()
        canonical_plan_digests = {
            str(record["payload"].get("planDigest", ""))
            for record in records if record["kind"] == "deployment-plan-v2"
        }
        for record in records:
            payload = record["payload"]
            if record["kind"] in {"deployment-plan-v2", "deployment-state"}:
                legacy = record["kind"] == "deployment-state"
                if legacy:
                    warnings.warn(
                        "importing deprecated deployment-state journal record; "
                        "it will be rewritten as deployment-plan-v2",
                        DeprecationWarning, stacklevel=2)
                deployment_id = str(payload["deploymentId"])
                self._states[deployment_id] = RevisionState(payload["state"])
                definition_data = payload.get("definition")
                if definition_data is None:
                    raise ValueError("deployment journal record lacks immutable definition")
                if definition_data is not None:
                    definition = DeploymentDefinition(
                        deployment_id=str(definition_data["deployment_id"]),
                        model_id=str(definition_data["model_id"]),
                        artifacts=tuple(ArtifactReference(**item)
                                        for item in definition_data["artifacts"]),
                        roles=tuple(definition_data["roles"]),
                        configuration=dict(definition_data.get("configuration", {})),
                    )
                    lifecycle_epoch = int(payload.get(
                        "lifecycleEpoch", payload.get("epoch", 0)))
                    plan = DeploymentPlan.resolve(definition, epoch=lifecycle_epoch)
                    recorded_digest = str(payload.get(
                        "planDigest", payload.get("revision", "")))
                    if plan.plan_digest != recorded_digest:
                        raise ValueError("deployment journal plan digest mismatch")
                    self._plans[deployment_id] = plan
                readiness = tuple(payload.get("providerReadiness", ()))
                if readiness:
                    self._bindings[deployment_id] = tuple(sorted(
                        (str(item["role"]), str(item["provider"]),
                         str(item["bootEpoch"])) for item in readiness))
                if legacy and plan.plan_digest not in canonical_plan_digests:
                    self.legacy_import_count += 1
                    self._append_plan_record(
                        plan, self._states[deployment_id],
                        str(payload.get("action", "IMPORT")),
                        str(payload.get("reason", "legacy-v1-import")),
                        readiness=readiness,
                        action_receipts=tuple(payload.get(
                            "providerActionReceipts", ())))
                    canonical_plan_digests.add(plan.plan_digest)
            elif record["kind"] == "deployment-operation":
                operation = DeploymentOperation(**payload)
                self._operations[operation.operation_id] = operation

    def validate(self, definition: DeploymentDefinition) -> DeploymentDefinition:
        if not definition.artifacts: raise ValueError("deployment requires external artifacts")
        return definition

    def resolve(self, definition: DeploymentDefinition, *, epoch: int = 1) -> DeploymentPlan:
        self.validate(definition)
        plan = DeploymentPlan.resolve(definition, epoch)
        self._plans[definition.deployment_id] = plan
        self._states[definition.deployment_id] = RevisionState.RESOLVED
        self._record(plan, RevisionState.RESOLVED, "RESOLVE")
        return plan

    def plan(self, plan: DeploymentPlan) -> DeploymentOperation:
        return DeploymentOperation(
            f"plan:{plan.plan_digest}", plan.deployment_id,
            plan.plan_digest, "PLAN", "DRY_RUN", lifecycle_epoch=plan.lifecycle_epoch)

    def apply(self, revision: DeploymentPlan, *, readiness=(),
              activation_receipts=(), idempotency_key: str = "",
              _action: str = "APPLY") -> DeploymentOperation:
        current = self._states.get(revision.deployment_id)
        if current == RevisionState.ACTIVE:
            current_plan = self._plans.get(revision.deployment_id)
            if current_plan is None or current_plan.plan_digest != revision.plan_digest:
                raise ValueError("active DeploymentPlan differs from apply target")
            return self._operation(
                revision, _action, "ACTIVE", idempotency_key=idempotency_key)
        readiness = tuple(readiness)
        is_ready, reason = self._validate_readiness(revision, readiness)
        candidate_bindings = tuple(sorted(
            (item.role, item.provider, item.boot_epoch) for item in readiness))
        activations = ()
        if is_ready:
            activations = self._validate_action(
                revision, "ACTIVATE", "ACTIVE", activation_receipts,
                expected_bindings=candidate_bindings)
        state = RevisionState.ACTIVE if is_ready else RevisionState.FAILED
        self._plans[revision.deployment_id] = revision
        self._states[revision.deployment_id] = state
        if is_ready:
            self._bindings[revision.deployment_id] = candidate_bindings
        self._record(
            revision, state, _action, reason, readiness=readiness,
            action_receipts=activations)
        return self._operation(
            revision, _action, state.value, reason,
            idempotency_key=idempotency_key)

    def reconcile(self, revision: DeploymentPlan, *, readiness=(),
                  idempotency_key: str = "") -> DeploymentOperation:
        """Reconcile a journaled revision from authenticated Provider evidence.

        Evidence loss is an UNKNOWN observation and never fabricates INACTIVE.
        A valid observation may confirm the current desired phase, but does not
        replay ACTIVATE or create a new lifecycle epoch.
        """
        current_plan = self._plans.get(revision.deployment_id)
        if (current_plan is None or
                current_plan.plan_digest != revision.plan_digest or
                current_plan.lifecycle_epoch != revision.lifecycle_epoch):
            raise ValueError("reconcile target is not the journaled DeploymentPlan")
        current = self._states.get(revision.deployment_id, RevisionState.RESOLVED)
        if not readiness:
            self._record(
                revision, current, "RECONCILE", "EVIDENCE_UNAVAILABLE")
            return self._operation(
                revision, "RECONCILE", "UNKNOWN", "EVIDENCE_UNAVAILABLE",
                idempotency_key=idempotency_key, retryable=True)
        is_ready, reason = self._validate_readiness(revision, tuple(readiness))
        if not is_ready:
            self._record(revision, current, "RECONCILE", reason,
                         readiness=tuple(readiness))
            return self._operation(
                revision, "RECONCILE", "DEGRADED", reason,
                idempotency_key=idempotency_key, retryable=True)
        bindings = tuple(sorted(
            (item.role, item.provider, item.boot_epoch) for item in readiness))
        expected = self._bindings.get(revision.deployment_id)
        if expected is not None and bindings != expected:
            raise ValueError("reconcile Provider boot-epoch binding changed")
        self._bindings[revision.deployment_id] = bindings
        self._record(revision, current, "RECONCILE", readiness=tuple(readiness))
        return self._operation(
            revision, "RECONCILE", current.value,
            idempotency_key=idempotency_key)

    def open_operation(self, operation_id: str) -> DeploymentOperation:
        try:
            return self._operations[operation_id]
        except KeyError as exc:
            raise KeyError("deployment operation handle missing") from exc

    def _operation(self, revision, action, status, reason="", *,
                   idempotency_key="", retryable=False):
        identity = {
            "deploymentId": revision.deployment_id,
            "revision": revision.plan_digest,
            "lifecycleEpoch": revision.lifecycle_epoch,
            "action": action,
            "idempotencyKey": idempotency_key or f"{action}:{revision.plan_digest}",
        }
        digest = "sha256:" + hashlib.sha256(json.dumps(
            identity, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        operation_id = f"{action.lower()}:{digest[7:31]}"
        existing = self._operations.get(operation_id)
        if existing is not None:
            if (existing.revision != revision.plan_digest or
                    existing.action != action):
                raise ValueError("deployment operation idempotency conflict")
            return existing
        if self.journal is None:
            raise RuntimeError("deployment operation requires a RuntimeJournal")
        cursor = f"journal:{len(self.journal.records()) + 1}"
        operation = DeploymentOperation(
            operation_id, revision.deployment_id, revision.plan_digest,
            action, status, reason, revision.lifecycle_epoch, digest, cursor,
            bool(retryable))
        self.journal.append("deployment-operation", operation.__dict__)
        self._operations[operation_id] = operation
        return operation

    def _validate_readiness(self, revision: DeploymentPlan, readiness):
        if not readiness:
            raise ValueError("apply requires authenticated Provider readiness evidence")
        if self._readiness_verifier is None:
            raise ValueError("apply requires an authenticated Provider readiness verifier")
        required_roles = set(revision.definition.roles)
        observed_roles = set()
        readiness_failures = []
        expected_artifacts = tuple(item.digest for item in revision.definition.artifacts)
        now_ms = int(time.time() * 1000)
        for receipt in readiness:
            if (receipt.revision != revision.plan_digest
                    or not receipt.boot_epoch
                    or receipt.role not in required_roles
                    or receipt.role in observed_roles
                    or tuple(receipt.artifact_digests) != expected_artifacts
                    or not receipt.adapter_name
                    or receipt.observed_at_ms <= 0
                    or receipt.observed_at_ms > now_ms
                    or receipt.expires_at_ms <= now_ms
                    or receipt.evidence_digest != receipt.expected_digest()
                    or not receipt.signer_key_id or not receipt.signature
                    or not self._readiness_verifier(receipt)):
                raise ValueError("Provider readiness evidence is incomplete or unauthenticated")
            observed_roles.add(receipt.role)
            if (not receipt.ready or not receipt.permission_ready
                    or receipt.capacity <= 0
                    or not receipt.adapter_version):
                readiness_failures.append(receipt.reason or "READINESS_FAILED")
        if observed_roles != required_roles:
            raise ValueError("Provider readiness does not cover every deployment role")
        if readiness_failures:
            return False, ";".join(sorted(set(readiness_failures)))
        return True, ""

    def status(self, deployment_id: str) -> RevisionState | None:
        return self._states.get(deployment_id)

    def wait(self, deployment_id: str, target=RevisionState.ACTIVE,
             timeout_ms: int = 1000) -> RevisionState | None:
        deadline = time.monotonic() + timeout_ms / 1000
        while time.monotonic() < deadline:
            if self.status(deployment_id) == target: return target
            if self.status(deployment_id) == RevisionState.FAILED: return RevisionState.FAILED
            time.sleep(0.01)
        return self.status(deployment_id)

    def rollback(self, definition: DeploymentDefinition, *, readiness=(),
                 activation_receipts=(), idempotency_key: str = "") -> DeploymentOperation:
        current = self._plans.get(definition.deployment_id)
        epoch = 1 if current is None else current.lifecycle_epoch + 1
        revision = self.resolve(definition, epoch=epoch)
        return self.apply(
            revision, readiness=readiness,
            activation_receipts=activation_receipts,
            idempotency_key=idempotency_key, _action="ROLLBACK")

    def _validate_action(self, revision, action, state, action_receipts,
                         *, expected_bindings=None):
        receipts = tuple(action_receipts)
        if not receipts:
            raise ValueError(
                f"{action.lower()} requires authenticated Provider {action} action evidence")
        if self._readiness_verifier is None:
            raise ValueError(f"{action} requires a Provider action evidence verifier")
        expected = set(
            expected_bindings if expected_bindings is not None
            else self._bindings.get(revision.deployment_id, ()))
        observed = set()
        now_ms = int(time.time() * 1000)
        for receipt in receipts:
            membership = (receipt.role, receipt.provider, receipt.boot_epoch)
            if (membership not in expected or membership in observed
                    or receipt.revision != revision.plan_digest
                    or receipt.action != action or receipt.state != state
                    or receipt.observed_at_ms <= 0
                    or receipt.observed_at_ms > now_ms
                    or receipt.expires_at_ms <= now_ms
                    or receipt.evidence_digest != receipt.expected_digest()
                    or not receipt.signer_key_id or not receipt.signature
                    or not self._readiness_verifier(receipt)):
                raise ValueError(
                    f"Provider {action} action evidence is incomplete or unauthenticated")
            observed.add(membership)
        if observed != expected:
            raise ValueError(f"Provider {action} action evidence is incomplete")
        return receipts

    def drain(self, deployment_id: str, *, action_receipts=(),
              idempotency_key: str = "") -> DeploymentOperation:
        revision = self._plans[deployment_id]
        receipts = self._validate_action(
            revision, "DRAIN", "INACTIVE", action_receipts)
        self._states[deployment_id] = RevisionState.INACTIVE
        self._record(revision, RevisionState.INACTIVE, "DRAIN",
                     action_receipts=receipts)
        return self._operation(
            revision, "DRAIN", RevisionState.INACTIVE.value,
            idempotency_key=idempotency_key)

    def delete(self, deployment_id: str, *, action_receipts=(),
               idempotency_key: str = "") -> DeploymentOperation:
        revision = self._plans[deployment_id]
        if self._states.get(deployment_id) != RevisionState.INACTIVE:
            raise ValueError("delete requires an inactive deployment")
        receipts = self._validate_action(
            revision, "DELETE", "DELETED", action_receipts)
        self._states[deployment_id] = RevisionState.DELETED
        self._record(revision, RevisionState.DELETED, "DELETE",
                     action_receipts=receipts)
        return self._operation(
            revision, "DELETE", RevisionState.DELETED.value,
            idempotency_key=idempotency_key)

    def _record(self, revision, state, action, reason="", readiness=(),
                action_receipts=()):
        if self.journal is None:
            raise RuntimeError("deployment lifecycle requires a RuntimeJournal")
        self._append_plan_record(revision, state, action, reason,
                                 readiness=readiness,
                                 action_receipts=action_receipts)

    def _append_plan_record(self, revision, state, action, reason="", readiness=(),
                            action_receipts=()):
        def field(item, snake, camel=None, default=""):
            if isinstance(item, dict):
                return item.get(camel or snake, item.get(snake, default))
            return getattr(item, snake, default)

        self.journal.append("deployment-plan-v2", {
            "schemaVersion": 2,
            "deploymentId": revision.deployment_id,
            "planDigest": revision.plan_digest,
            "lifecycleEpoch": revision.lifecycle_epoch, "state": state.value,
            "action": action, "reason": reason,
            "providerReadiness": [{
                "provider": field(item, "provider"),
                "role": field(item, "role"),
                "bootEpoch": field(item, "boot_epoch", "bootEpoch"),
                "evidenceDigest": field(item, "evidence_digest", "evidenceDigest"),
                "signerKeyId": field(item, "signer_key_id", "signerKeyId"),
                "expiresAtMs": field(item, "expires_at_ms", "expiresAtMs", 0),
            } for item in readiness],
            "providerActionReceipts": [{
                "provider": field(item, "provider"),
                "role": field(item, "role"),
                "bootEpoch": field(item, "boot_epoch", "bootEpoch"),
                "action": field(item, "action"),
                "state": field(item, "state"),
                "evidenceDigest": field(item, "evidence_digest", "evidenceDigest"),
                "signerKeyId": field(item, "signer_key_id", "signerKeyId"),
            } for item in action_receipts],
            "definition": revision.definition.to_dict()})


class DeploymentHandle:
    """Bound view of one immutable deployment revision.

    The handle owns no lifecycle state.  Every method re-enters the catalog's
    existing coordinator/store, so a deserialized handle cannot manufacture an
    ACTIVE deployment or silently follow a newer revision.
    """

    def __init__(self, catalog: "DeploymentCatalog", handle_ref: DeploymentHandleRef):
        self._catalog = catalog
        self._handle_ref = handle_ref

    @property
    def handle_ref(self) -> DeploymentHandleRef:
        return self._handle_ref

    @property
    def ref(self) -> DeploymentRef:
        return self._catalog.active_ref(self._handle_ref)

    def status(self) -> DeploymentStatus:
        return self._catalog.status(self._handle_ref)

    def refresh(self) -> "DeploymentHandle":
        return self._catalog.get(self._handle_ref)

    def wait_until_active(self, *, timeout: timedelta) -> "DeploymentHandle":
        if not isinstance(timeout, timedelta) or timeout.total_seconds() <= 0:
            raise TypeError("timeout must be a positive datetime.timedelta")
        deadline = time.monotonic() + timeout.total_seconds()
        terminal = {"FAILED", "CANCELLED", "EXPIRED", "INACTIVE", "DELETED"}
        while time.monotonic() < deadline:
            state = self.status().state
            if state == "ACTIVE":
                return self
            if state in terminal:
                raise RuntimeError(f"deployment became {state}")
            time.sleep(0.01)
        raise TimeoutError("deployment did not become ACTIVE before wait timeout")


class _VerifiedDefinitionRecord:
    """Transport-attested definition Data; never exported as public authority."""

    def __init__(self, definition, signer_identity: str, signer_certificate: str):
        self.definition = definition
        self.signer_identity = signer_identity
        self.signer_certificate = signer_certificate


class _VerifiedActivationRecord:
    """Transport-attested activation Data signed by the lifecycle owner."""

    def __init__(self, activation, signer_identity: str, signer_certificate: str):
        self.activation = activation
        self.signer_identity = signer_identity
        self.signer_certificate = signer_certificate


class NetworkDeploymentCatalogTransport:
    """Exact-name signed NDN Data transport plus untrusted NDNSD hints.

    Authority remains in :class:`DeploymentCatalog`: this adapter verifies the
    NDN Data trust path and signer identity, while the catalog verifies the
    embedded Application signature, digests, lifecycle owner and fences.
    """

    DEFINITION_SCHEMA = "ndnsf-di-definition-record-v1"
    ACTIVATION_SCHEMA = "ndnsf-di-activation-record-v1"

    def __init__(self, service_user, *, timeout_ms: int = 5000,
                 freshness_ms: int = 24 * 60 * 60 * 1000):
        self._user = service_user
        self.timeout_ms = max(1, int(timeout_ms))
        self.freshness_ms = max(1, int(freshness_ms))

    @staticmethod
    def _record_signer(record_name: str) -> str:
        marker = "/NDNSF/DI/"
        if marker not in record_name:
            raise ValueError("APP record name is outside /NDNSF/DI")
        signer = record_name.split(marker, 1)[0]
        if not signer.startswith("/"):
            raise ValueError("APP record signer identity is invalid")
        return signer

    @staticmethod
    def _encode(schema: str, record_name: str, digest: str,
                field: str, value: dict) -> bytes:
        return json.dumps({
            "schema": schema,
            "recordName": record_name,
            "recordDigest": digest,
            field: value,
        }, sort_keys=True, separators=(",", ":")).encode("utf-8")

    @staticmethod
    def _decode(payload: bytes, *, schema: str, record_name: str,
                digest: str, field: str) -> dict:
        value = json.loads(bytes(payload).decode("utf-8"))
        if (not isinstance(value, dict) or value.get("schema") != schema or
                value.get("recordName") != record_name or
                value.get("recordDigest") != digest or
                not isinstance(value.get(field), dict)):
            raise ValueError("signed APP record envelope binding mismatch")
        return dict(value[field])

    def publish_definition(self, record_name: str,
                           definition: DeploymentDefinition):
        digest = definition.digest()
        result = self._user.publish_signed_app_data(
            record_name,
            self._encode(self.DEFINITION_SCHEMA, record_name, digest,
                         "definition", definition.to_dict()),
            freshness_ms=self.freshness_ms)
        if not result.success:
            raise RuntimeError(f"definition Data publication failed: {result.error}")
        if result.data_name != record_name:
            raise ValueError("definition publisher changed the exact record name")
        return None

    def publish_activation(self, record_name: str,
                           activation: DeploymentActivationRecord):
        digest = activation.digest()
        result = self._user.publish_signed_app_data(
            record_name,
            self._encode(self.ACTIVATION_SCHEMA, record_name, digest,
                         "activation", activation.to_dict()),
            freshness_ms=self.freshness_ms)
        if not result.success:
            raise RuntimeError(f"activation Data publication failed: {result.error}")
        if result.data_name != record_name:
            raise ValueError("activation publisher changed the exact record name")
        return None

    def fetch_definition(self, record_name: str, digest: str):
        expected_signer = self._record_signer(record_name)
        result = self._user.fetch_signed_app_data(
            record_name, expected_signer, timeout_ms=self.timeout_ms)
        if not result.success:
            raise LookupError(f"definition Data unavailable: {result.error}")
        if result.data_name != record_name:
            raise ValueError("definition Data exact-name mismatch")
        definition = DeploymentDefinition.from_dict(self._decode(
            result.payload, schema=self.DEFINITION_SCHEMA,
            record_name=record_name, digest=digest, field="definition"))
        if (definition.digest() != digest or
                definition.application_identity != expected_signer):
            raise ValueError("definition Data digest or Application binding mismatch")
        return _VerifiedDefinitionRecord(
            definition, expected_signer, result.signer_certificate)

    def fetch_activation(self, record_name: str, digest: str):
        expected_signer = self._record_signer(record_name)
        result = self._user.fetch_signed_app_data(
            record_name, expected_signer, timeout_ms=self.timeout_ms)
        if not result.success:
            raise LookupError(f"activation Data unavailable: {result.error}")
        if result.data_name != record_name:
            raise ValueError("activation Data exact-name mismatch")
        activation = DeploymentActivationRecord.from_dict(self._decode(
            result.payload, schema=self.ACTIVATION_SCHEMA,
            record_name=record_name, digest=digest, field="activation"))
        if activation.digest() != digest:
            raise ValueError("activation record digest mismatch")
        return _VerifiedActivationRecord(
            activation, expected_signer, result.signer_certificate)

    def discovery_hints(self):
        hints = []
        for entry in self._user.get_ndnsd_services():
            metadata = entry.get("serviceMetaInfo", {})
            if not isinstance(metadata, dict):
                continue
            records = metadata.get("deployments", ())
            if isinstance(records, str):
                try:
                    records = json.loads(records)
                except json.JSONDecodeError:
                    continue
            if isinstance(records, list):
                hints.extend(item for item in records if isinstance(item, dict))
        return tuple(hints)


class DeploymentCatalog:
    """Typed catalog over signed definition and activation records.

    Lookup hints are admitted only after the exact stored definition has been
    digest- and signature-validated.  Transport adapters can replace the
    fetch/publish callables without changing this authority boundary.
    """

    def __init__(self, *, owner_identity: str, journal=None,
                 definition_publisher: Callable | None = None,
                 definition_fetcher: Callable | None = None,
                 activation_publisher: Callable | None = None,
                 activation_fetcher: Callable | None = None,
                 discovery_hints: Callable | None = None,
                 ensure_deployment: Callable | None = None,
                 trusted_application_keys=None):
        self.owner_identity = owner_identity
        self.journal = journal
        self._definition_publisher = definition_publisher
        self._definition_fetcher = definition_fetcher
        self._activation_publisher = activation_publisher
        self._activation_fetcher = activation_fetcher
        self._discovery_hints = discovery_hints
        self._ensure_deployment = ensure_deployment
        self._definitions: dict[str, DeploymentDefinition] = {}
        self._definition_refs: dict[str, DeploymentDefinitionRef] = {}
        self._statuses: dict[tuple[str, str], DeploymentStatus] = {}
        self._active_refs: dict[tuple[str, str], DeploymentRef] = {}
        self._activation_records: dict[tuple[str, str], DeploymentActivationRecord] = {}
        # A lifecycle-owner revocation fences the immutable revision itself.
        # Keeping only ``_active_refs.pop`` would make the same definition
        # immediately invocable again as ON_DEMAND and bypass the fence.
        self._revoked_revisions: set[tuple[str, str]] = set()
        self._trusted_application_keys = dict(trusted_application_keys or {})
        self._lock = RLock()
        self._restore()

    def authorize_application(self, application_identity: str,
                              signer_key_id: str,
                              signer_public_key: str) -> None:
        """Install one operator-authorized Application definition key."""
        if not application_identity.startswith("/"):
            raise ValueError("Application identity must be an NDN name")
        raw = base64.b64decode(signer_public_key, validate=True)
        if "sha256:" + hashlib.sha256(raw).hexdigest() != signer_key_id:
            raise ValueError("Application definition key binding is invalid")
        key = (application_identity, signer_key_id)
        previous = self._trusted_application_keys.get(key)
        if previous is not None and previous != signer_public_key:
            raise ValueError(
                "Application definition key replacement requires rollover")
        if previous == signer_public_key:
            return
        self._trusted_application_keys[key] = signer_public_key
        if self.journal is not None:
            self.journal.append("deployment-application-key", {
                "applicationIdentity": application_identity,
                "signerKeyId": signer_key_id,
                "signerPublicKey": signer_public_key,
            })

    def _verify_definition(self, definition: DeploymentDefinition) -> DeploymentDefinition:
        from .application import ApplicationDefinitionSigner

        ApplicationDefinitionSigner.verify(definition)
        expected = self._trusted_application_keys.get(
            (definition.application_identity, definition.signer_key_id))
        if expected != definition.signer_public_key:
            raise PermissionError("deployment definition signer is not authorized")
        return definition

    def _admit_fetched_definition(self, fetched) -> DeploymentDefinition:
        if isinstance(fetched, _VerifiedDefinitionRecord):
            definition = fetched.definition
            if fetched.signer_identity != definition.application_identity:
                raise PermissionError("definition Data signer is not its Application")
            # The trust-schema-validated NDN Application identity authorizes
            # the exact embedded definition key. A naked/self-signed object
            # never reaches this branch.
            self.authorize_application(
                definition.application_identity,
                definition.signer_key_id,
                definition.signer_public_key)
            return definition
        return fetched

    def _restore(self) -> None:
        if self.journal is None:
            return
        for record in self.journal.records():
            payload = record["payload"]
            if record["kind"] == "deployment-application-key":
                key = (str(payload["applicationIdentity"]),
                       str(payload["signerKeyId"]))
                public_key = str(payload["signerPublicKey"])
                raw = base64.b64decode(public_key, validate=True)
                if "sha256:" + hashlib.sha256(raw).hexdigest() != key[1]:
                    raise ValueError("journaled Application key binding is invalid")
                previous = self._trusted_application_keys.get(key)
                if previous is not None and previous != public_key:
                    raise ValueError("journaled Application key rollover conflict")
                self._trusted_application_keys[key] = public_key
            elif record["kind"] == "deployment-definition":
                definition = DeploymentDefinition.from_dict(payload["definition"])
                ref = DeploymentDefinitionRef(**payload["reference"])
                if definition.digest() != ref.definition_digest:
                    raise ValueError("journaled definition digest mismatch")
                self._definitions[ref.definition_digest] = definition
                self._definition_refs[ref.definition_digest] = ref
            elif record["kind"] == "deployment-catalog-status":
                status_data = payload["status"]
                roles = tuple(DeploymentProgress(**item)
                              for item in status_data.get("roles", ()))
                status = DeploymentStatus(**{**status_data, "roles": roles})
                key = (payload["deploymentId"], payload["revision"])
                self._statuses[key] = status
                if payload.get("activeRef"):
                    self._active_refs[key] = DeploymentRef(**payload["activeRef"])
            elif record["kind"] == "deployment-activation":
                activation = DeploymentActivationRecord.from_dict(
                    payload["activation"])
                key = (activation.deployment_id, activation.revision)
                self._activation_records[key] = activation
                if activation.revoked:
                    self._revoked_revisions.add(key)
                    self._active_refs.pop(key, None)
            elif record["kind"] == "deployment-rollover-fence":
                key = (str(payload["deploymentId"]), str(payload["revision"]))
                self._revoked_revisions.add(key)
                self._active_refs.pop(key, None)

    def bind_ensure_deployment(self, ensure_deployment: Callable) -> None:
        """Bind the single preparation owner used by request and prewarm.

        Binding is intentionally one-shot. Replacing the coordinator after a
        catalog has been composed would create two lifecycle authorities.
        """
        if not callable(ensure_deployment):
            raise TypeError("ensure_deployment must be callable")
        with self._lock:
            if (self._ensure_deployment is not None and
                    self._ensure_deployment is not ensure_deployment):
                raise RuntimeError("deployment catalog already has an ensure owner")
            self._ensure_deployment = ensure_deployment

    def _require_not_revoked(self, deployment_id: str, revision: str) -> None:
        with self._lock:
            revoked = (deployment_id, revision) in self._revoked_revisions
        if revoked:
            raise PermissionError("deployment revision has been revoked")

    @staticmethod
    def _future_timestamp(value: str) -> bool:
        try:
            parsed = datetime.fromisoformat(value)
        except (TypeError, ValueError):
            return False
        return (parsed.tzinfo is not None and
                parsed > datetime.now(timezone.utc))

    def _current_active_ref(self, key: tuple[str, str]) -> DeploymentRef | None:
        """Return fresh ACTIVE evidence and retire expired cached evidence."""
        with self._lock:
            active = self._active_refs.get(key)
            if active is None or self._future_timestamp(active.expires_at):
                return active
            self._active_refs.pop(key, None)
            current = self._statuses.get(key)
            epoch = max(1, current.coordinator_epoch if current else 1)
            self._statuses[key] = DeploymentStatus(
                "INACTIVE", key[1], (), active.activation_certificate_digest,
                epoch, "ACTIVATION_EXPIRED")
            return None

    @staticmethod
    def _revision(definition: DeploymentDefinition) -> str:
        return DeploymentPlan.resolve(definition).plan_digest

    def publish_definition(self, definition: DeploymentDefinition) -> DeploymentDefinitionRef:
        self._verify_definition(definition)
        if definition.previous_revision:
            with self._lock:
                predecessor = next((
                    item for item in self._definitions.values()
                    if (item.application_identity == definition.application_identity and
                        item.deployment_id == definition.deployment_id and
                        self._revision(item) == definition.previous_revision)
                ), None)
            if predecessor is None:
                raise ValueError("previous deployment revision is unavailable")
            if (predecessor.deployment_owner != definition.deployment_owner or
                    predecessor.service != definition.service):
                raise ValueError("deployment rollover changed immutable authority")
        digest = definition.digest()
        record_name = (
            definition.application_identity.rstrip("/") + "/NDNSF/DI/DEFINITION/" +
            definition.deployment_id + "/" + digest)
        ref = DeploymentDefinitionRef(
            application_identity=definition.application_identity,
            deployment_owner=definition.deployment_owner,
            coordinator_service=definition.coordinator_service,
            deployment_id=definition.deployment_id,
            service=definition.service,
            record_name=record_name,
            definition_digest=digest,
            expires_at=definition.expires_at,
            signer_key_id=definition.signer_key_id,
        )
        if self._definition_publisher is not None:
            published = self._definition_publisher(record_name, definition)
            if published is not None and published != ref:
                raise ValueError("definition publisher changed the canonical reference")
        with self._lock:
            previous = self._definitions.get(digest)
            if previous is not None and previous != definition:
                raise ValueError("definition digest collision")
            self._definitions[digest] = definition
            self._definition_refs[digest] = ref
        if self.journal is not None:
            self.journal.append("deployment-definition", {
                "reference": asdict(ref), "definition": definition.to_dict()})
        return ref

    def definition_ref(self, definition: DeploymentDefinition) -> DeploymentDefinitionRef:
        """Return the exact canonical reference already validated by this catalog."""
        self._verify_definition(definition)
        digest = definition.digest()
        with self._lock:
            existing = self._definition_refs.get(digest)
        if existing is not None:
            return existing
        return DeploymentDefinitionRef(
            application_identity=definition.application_identity,
            deployment_owner=definition.deployment_owner,
            coordinator_service=definition.coordinator_service,
            deployment_id=definition.deployment_id,
            service=definition.service,
            record_name=(definition.application_identity.rstrip("/") +
                         "/NDNSF/DI/DEFINITION/" + definition.deployment_id +
                         "/" + digest),
            definition_digest=digest,
            expires_at=definition.expires_at,
            signer_key_id=definition.signer_key_id,
        )

    def publish_activation(
        self, definition: DeploymentDefinition, status: DeploymentStatus, *,
        validity: timedelta = timedelta(hours=1),
    ) -> DeploymentRef:
        """Publish lifecycle-owner-signed ACTIVE evidence after exact readiness."""
        self._verify_definition(definition)
        if self.owner_identity != definition.deployment_owner:
            raise PermissionError("only the deployment owner may activate a revision")
        if status.state not in {"READY", "ACTIVE"}:
            raise ValueError("activation requires READY status")
        revision = self._revision(definition)
        self._require_not_revoked(definition.deployment_id, revision)
        if status.revision != revision:
            raise ValueError("activation status revision mismatch")
        if validity.total_seconds() <= 0:
            raise ValueError("activation validity must be positive")
        now = datetime.now(timezone.utc)
        with self._lock:
            prior_candidates = [
                (key, active, self._activation_records.get(key))
                for key, active in self._active_refs.items()
                if key[0] == definition.deployment_id and key[1] != revision
            ]
        prior_key = None
        prior_active = None
        prior_activation = None
        if prior_candidates:
            prior_key, prior_active, prior_activation = max(
                prior_candidates, key=lambda item: item[1].lifecycle_epoch)
            if definition.previous_revision != prior_active.revision:
                raise ValueError("deployment rollover does not name the ACTIVE predecessor")
            if prior_activation is None:
                raise ValueError("ACTIVE predecessor evidence is unavailable")
        lifecycle_epoch = max(
            status.coordinator_epoch,
            (prior_active.lifecycle_epoch + 1) if prior_active is not None else 1)
        record_name = (
            definition.deployment_owner.rstrip("/") +
            "/NDNSF/DI/DEPLOYMENT/" + definition.deployment_id + "/" +
            revision + "/ACTIVATION")
        activation = DeploymentActivationRecord(
            application_identity=definition.application_identity,
            deployment_owner=definition.deployment_owner,
            deployment_id=definition.deployment_id,
            revision=revision,
            service=definition.service,
            definition_digest=definition.digest(),
            revision_digest=revision,
            activation_certificate_digest=status.readiness_certificate_digest,
            lifecycle_epoch=lifecycle_epoch,
            activated_at=now.isoformat(),
            expires_at=(now + validity).isoformat(),
            signer_key_id="ndn-data-key-locator",
            signature="ndn-data-signature",
            record_name=record_name,
            supersedes=(prior_activation.digest()
                        if prior_activation is not None else ""),
        )
        if self._activation_publisher is None:
            raise RuntimeError("deployment owner has no activation Data publisher")
        published = self._activation_publisher(record_name, activation)
        ref = self.definition_ref(definition)
        active = DeploymentRef(
            deployment_id=definition.deployment_id,
            revision=revision,
            service=definition.service,
            definition_digest=definition.digest(),
            activation_certificate_digest=status.readiness_certificate_digest,
            activation_record_name=record_name,
            activation_record_digest=activation.digest(),
            lifecycle_epoch=lifecycle_epoch,
            application_identity=definition.application_identity,
            deployment_owner=definition.deployment_owner,
            coordinator_service=definition.coordinator_service,
            definition_record_name=ref.record_name,
            expires_at=activation.expires_at,
        )
        if published is not None and published != active:
            raise ValueError("activation publisher changed the canonical reference")
        key = (definition.deployment_id, revision)
        with self._lock:
            if prior_key is not None:
                self._revoked_revisions.add(prior_key)
                self._active_refs.pop(prior_key, None)
                self._statuses[prior_key] = DeploymentStatus(
                    "INACTIVE", prior_key[1], (),
                    prior_active.activation_certificate_digest,
                    lifecycle_epoch, "ROLLED_OVER")
            self._activation_records[key] = activation
            self._active_refs[key] = active
            self._statuses[key] = DeploymentStatus(
                "ACTIVE", revision, status.roles,
                status.readiness_certificate_digest, lifecycle_epoch)
        if self.journal is not None:
            if prior_key is not None:
                self.journal.append("deployment-rollover-fence", {
                    "deploymentId": prior_key[0],
                    "revision": prior_key[1],
                    "supersededBy": revision,
                    "lifecycleEpoch": lifecycle_epoch,
                })
                self.journal.append("deployment-catalog-status", {
                    "deploymentId": prior_key[0],
                    "revision": prior_key[1],
                    "status": asdict(self._statuses[prior_key]),
                    "activeRef": None,
                })
            self.journal.append("deployment-activation", {
                "activation": activation.to_dict()})
            self.journal.append("deployment-catalog-status", {
                "deploymentId": definition.deployment_id,
                "revision": revision,
                "status": asdict(self._statuses[key]),
                "activeRef": asdict(active),
            })
        return active

    def revoke_activation(
        self, definition: DeploymentDefinition, *,
        validity: timedelta = timedelta(hours=1),
    ) -> None:
        """Publish a monotonic lifecycle-owner revocation fence."""
        self._verify_definition(definition)
        if self.owner_identity != definition.deployment_owner:
            raise PermissionError("only the deployment owner may revoke activation")
        revision = self._revision(definition)
        key = (definition.deployment_id, revision)
        with self._lock:
            current = self._activation_records.get(key)
        if current is None or current.revoked:
            raise RuntimeError("there is no ACTIVE record to revoke")
        now = datetime.now(timezone.utc)
        record_name = (
            definition.deployment_owner.rstrip("/") +
            "/NDNSF/DI/DEPLOYMENT/" + definition.deployment_id + "/" +
            revision + "/REVOCATION/" + str(current.lifecycle_epoch + 1))
        revoked = DeploymentActivationRecord(
            application_identity=definition.application_identity,
            deployment_owner=definition.deployment_owner,
            deployment_id=definition.deployment_id,
            revision=revision,
            service=definition.service,
            definition_digest=definition.digest(),
            revision_digest=revision,
            activation_certificate_digest=current.activation_certificate_digest,
            lifecycle_epoch=current.lifecycle_epoch + 1,
            activated_at=now.isoformat(),
            expires_at=(now + validity).isoformat(),
            signer_key_id="ndn-data-key-locator",
            signature="ndn-data-signature",
            state="REVOKED",
            record_name=record_name,
            supersedes=current.digest(),
            revoked=True,
        )
        if self._activation_publisher is None:
            raise RuntimeError("deployment owner has no activation Data publisher")
        self._activation_publisher(record_name, revoked)
        with self._lock:
            self._activation_records[key] = revoked
            self._revoked_revisions.add(key)
            self._active_refs.pop(key, None)
            self._statuses[key] = DeploymentStatus(
                "INACTIVE", revision, (),
                current.activation_certificate_digest,
                revoked.lifecycle_epoch, "REVOKED")
        if self.journal is not None:
            self.journal.append("deployment-activation", {
                "activation": revoked.to_dict()})
            self.journal.append("deployment-catalog-status", {
                "deploymentId": definition.deployment_id,
                "revision": revision,
                "status": asdict(self._statuses[key]),
                "activeRef": None,
            })

    def resolve_definition(self, deployment) -> tuple[DeploymentDefinition, str]:
        if isinstance(deployment, DeploymentHandle):
            deployment = deployment.handle_ref
        if isinstance(deployment, DeploymentDefinition):
            definition = self._verify_definition(deployment)
            revision = self._revision(definition)
            self._require_not_revoked(definition.deployment_id, revision)
            if (self._definition_publisher is None or
                    definition.application_identity == self.owner_identity):
                self.publish_definition(definition)
            else:
                ref = self.definition_ref(definition)
                with self._lock:
                    self._definitions[definition.digest()] = definition
                    self._definition_refs[definition.digest()] = ref
            return definition, revision
        if isinstance(deployment, DeploymentRef):
            if not self._future_timestamp(deployment.expires_at):
                raise ValueError("ACTIVE deployment evidence expired")
            with self._lock:
                definition = self._definitions.get(deployment.definition_digest)
            if definition is None and self._definition_fetcher is not None:
                definition = self._admit_fetched_definition(
                    self._definition_fetcher(deployment.definition_record_name,
                                             deployment.definition_digest))
            if definition is None:
                raise LookupError("ACTIVE definition is unavailable")
            self._verify_definition(definition)
            self._require_not_revoked(
                deployment.deployment_id, deployment.revision)
            canonical_definition_name = (
                definition.application_identity.rstrip("/") +
                "/NDNSF/DI/DEFINITION/" + definition.deployment_id + "/" +
                definition.digest())
            if (definition.digest() != deployment.definition_digest or
                    definition.deployment_id != deployment.deployment_id or
                    definition.service != deployment.service or
                    deployment.definition_record_name != canonical_definition_name or
                    (deployment.application_identity and
                     definition.application_identity != deployment.application_identity) or
                    (deployment.deployment_owner and
                     definition.deployment_owner != deployment.deployment_owner) or
                    (deployment.coordinator_service and
                     definition.coordinator_service != deployment.coordinator_service)):
                raise ValueError("ACTIVE deployment reference binding mismatch")
            active = self._current_active_ref(
                (deployment.deployment_id, deployment.revision))
            if active != deployment:
                raise ValueError("ACTIVE deployment evidence is not locally validated")
            return definition, deployment.revision
        if isinstance(deployment, DeploymentHandleRef):
            with self._lock:
                for digest, definition in self._definitions.items():
                    if (definition.deployment_id == deployment.deployment_id and
                            self._revision(definition) == deployment.revision):
                        self._require_not_revoked(
                            deployment.deployment_id, deployment.revision)
                        self._verify_definition(definition)
                        return definition, deployment.revision
            raise LookupError("deployment handle definition is unavailable")
        if isinstance(deployment, DeploymentDefinitionRef):
            with self._lock:
                definition = self._definitions.get(deployment.definition_digest)
            if definition is None and self._definition_fetcher is not None:
                definition = self._admit_fetched_definition(
                    self._definition_fetcher(
                        deployment.record_name, deployment.definition_digest))
            if definition is None:
                raise LookupError("ON_DEMAND definition is unavailable")
            self._verify_definition(definition)
            revision = self._revision(definition)
            self._require_not_revoked(definition.deployment_id, revision)
            canonical_record_name = (
                definition.application_identity.rstrip("/") +
                "/NDNSF/DI/DEFINITION/" + definition.deployment_id + "/" +
                definition.digest())
            if (definition.digest() != deployment.definition_digest or
                    definition.application_identity != deployment.application_identity or
                    definition.deployment_owner != deployment.deployment_owner or
                    definition.coordinator_service != deployment.coordinator_service or
                    definition.deployment_id != deployment.deployment_id or
                    definition.service != deployment.service or
                    deployment.record_name != canonical_record_name or
                    deployment.expires_at != definition.expires_at or
                    definition.signer_key_id != deployment.signer_key_id):
                raise ValueError("ON_DEMAND definition reference binding mismatch")
            with self._lock:
                newly_cached = deployment.definition_digest not in self._definitions
                self._definitions[deployment.definition_digest] = definition
                self._definition_refs[deployment.definition_digest] = deployment
            if newly_cached and self.journal is not None:
                self.journal.append("deployment-definition", {
                    "reference": asdict(deployment),
                    "definition": definition.to_dict(),
                })
            return definition, revision
        raise TypeError("deployment must be a signed requestable deployment")

    def ensure(self, deployment) -> DeploymentHandle:
        definition, revision = self.resolve_definition(deployment)
        locator = f"journal:deployment/{definition.deployment_id}/{revision}"
        handle_ref = DeploymentHandleRef(
            deployment_id=definition.deployment_id,
            revision=revision,
            lifecycle_epoch=1,
            owner_identity=self.owner_identity,
            journal_locator=locator,
            journal_digest="sha256:" + hashlib.sha256(locator.encode()).hexdigest(),
        )
        key = (definition.deployment_id, revision)
        with self._lock:
            self._statuses.setdefault(key, DeploymentStatus("PREPARING", revision))
        if self._ensure_deployment is not None:
            status, active_ref = self._ensure_deployment(definition, revision)
            if not isinstance(status, DeploymentStatus):
                raise TypeError("ensure_deployment must return DeploymentStatus")
            if status.revision != revision:
                raise ValueError("ensure_deployment changed immutable revision")
            with self._lock:
                self._statuses[key] = status
                if active_ref is not None:
                    if not isinstance(active_ref, DeploymentRef):
                        raise TypeError("ensure_deployment returned invalid active reference")
                    self._active_refs[key] = active_ref
        if self.journal is not None:
            status = self._statuses[key]
            self.journal.append("deployment-catalog-status", {
                "deploymentId": definition.deployment_id,
                "revision": revision,
                "status": asdict(status),
                "activeRef": asdict(self._active_refs[key])
                if key in self._active_refs else None,
            })
        return DeploymentHandle(self, handle_ref)

    def status(self, ref: DeploymentHandleRef) -> DeploymentStatus:
        key = (ref.deployment_id, ref.revision)
        self._current_active_ref(key)
        with self._lock:
            try:
                return self._statuses[key]
            except KeyError as exc:
                raise LookupError("deployment status is unavailable") from exc

    def record_status(self, ref: DeploymentHandleRef,
                      status: DeploymentStatus) -> None:
        """Persist one monotonic, exact revision-bound aggregate status."""
        if status.revision != ref.revision:
            raise ValueError("deployment status revision mismatch")
        key = (ref.deployment_id, ref.revision)
        with self._lock:
            current = self._statuses.get(key)
            if current is not None and status.coordinator_epoch < current.coordinator_epoch:
                raise ValueError("stale deployment coordinator epoch")
            if current is not None:
                old_versions = {(item.role, item.provider): (
                    item.attempt, item.sequence) for item in current.roles}
                for item in status.roles:
                    previous = old_versions.get((item.role, item.provider))
                    if previous is not None and (item.attempt, item.sequence) < previous:
                        raise ValueError("stale deployment progress")
            self._statuses[key] = status
        if self.journal is not None:
            self.journal.append("deployment-catalog-status", {
                "deploymentId": ref.deployment_id,
                "revision": ref.revision,
                "status": asdict(status),
                "activeRef": asdict(self._active_refs[key])
                if key in self._active_refs else None,
            })

    def active_ref(self, ref: DeploymentHandleRef) -> DeploymentRef:
        status = self.status(ref)
        if status.state != "ACTIVE":
            raise RuntimeError("deployment is not ACTIVE")
        try:
            active = self._current_active_ref(
                (ref.deployment_id, ref.revision))
            if active is None:
                raise KeyError
            return active
        except KeyError as exc:
            raise RuntimeError("ACTIVE deployment evidence is unavailable") from exc

    def get(self, ref) -> DeploymentHandle:
        if isinstance(ref, DeploymentHandle):
            ref = ref.handle_ref
        if isinstance(ref, DeploymentHandleRef):
            self.status(ref)
            return DeploymentHandle(self, ref)
        definition, revision = self.resolve_definition(ref)
        return self.ensure(definition)

    def discover(self, *, service: str, model=None, constraints=None) -> tuple[DeploymentSummary, ...]:
        if not service.startswith("/"):
            raise ValueError("service must be an NDN name")
        values = []
        if self._discovery_hints is not None:
            for hint in self._discovery_hints():
                try:
                    self._admit_discovery_hint(dict(hint), service)
                except (KeyError, TypeError, ValueError, LookupError,
                        PermissionError, json.JSONDecodeError):
                    # NDNSD is explicitly an untrusted lookup channel. Bad or
                    # unavailable hints are ignored; they never become refs.
                    continue
        with self._lock:
            definitions = tuple(self._definitions.items())
        for digest, definition in definitions:
            if definition.service != service:
                continue
            revision = self._revision(definition)
            if (definition.deployment_id, revision) in self._revoked_revisions:
                continue
            active = self._current_active_ref(
                (definition.deployment_id, revision))
            ref = active or self._definition_refs[digest]
            state = "ACTIVE" if active is not None else "ON_DEMAND"
            values.append(DeploymentSummary(ref, state, service, definition.expires_at))
        return tuple(sorted(values, key=lambda item: (
            item.state != "ACTIVE", item.deployment.deployment_id,
            getattr(item.deployment, "revision", ""))))

    def discovery_hint(self, deployment) -> dict[str, str]:
        """Build a bounded NDNSD locator hint for a validated reference.

        The returned dictionary deliberately contains no signature or
        readiness claim. A consumer must fetch and validate the exact signed
        APP records before the hint can become authority.
        """
        if isinstance(deployment, DeploymentDefinition):
            ref = self.definition_ref(deployment)
            definition = deployment
        elif isinstance(deployment, DeploymentDefinitionRef):
            definition, _ = self.resolve_definition(deployment)
            ref = deployment
        elif isinstance(deployment, DeploymentRef):
            definition, _ = self.resolve_definition(deployment)
            ref = self.definition_ref(definition)
        elif isinstance(deployment, DeploymentHandle):
            status = deployment.status()
            if status.state == "ACTIVE":
                return self.discovery_hint(deployment.ref)
            definition, _ = self.resolve_definition(deployment.handle_ref)
            ref = self.definition_ref(definition)
        else:
            raise TypeError("deployment must be a validated deployment reference")
        hint = {
            "applicationIdentity": ref.application_identity,
            "deploymentOwner": ref.deployment_owner,
            "coordinatorService": ref.coordinator_service,
            "deploymentId": ref.deployment_id,
            "serviceName": ref.service,
            "definitionRecordName": ref.record_name,
            "definitionRecordDigest": ref.definition_digest,
            "expiresAt": ref.expires_at,
            "signerKeyId": ref.signer_key_id,
        }
        if isinstance(deployment, DeploymentRef):
            hint.update({
                "activationRecordName": deployment.activation_record_name,
                "activationRecordDigest": deployment.activation_record_digest,
            })
        return hint

    def advertise(self, service_provider, deployment, *,
                  lifetime_seconds: int = 30) -> dict[str, str]:
        """Publish one untrusted NDNSD locator for exact signed APP records."""
        if not isinstance(lifetime_seconds, int) or lifetime_seconds <= 0:
            raise ValueError("lifetime_seconds must be a positive integer")
        hint = self.discovery_hint(deployment)
        service_provider.publish_service_info(
            hint["serviceName"], lifetime_seconds,
            {"deployments": json.dumps(
                [hint], sort_keys=True, separators=(",", ":"))})
        return hint

    @staticmethod
    def _hint(hint: dict, camel: str, snake: str = ""):
        return hint.get(camel, hint.get(snake or camel, ""))

    def _admit_discovery_hint(self, hint: dict, requested_service: str) -> None:
        service = str(self._hint(hint, "serviceName", "service_name"))
        if service != requested_service:
            return
        ref = DeploymentDefinitionRef(
            application_identity=str(self._hint(
                hint, "applicationIdentity", "application_identity")),
            deployment_owner=str(self._hint(
                hint, "deploymentOwner", "deployment_owner")),
            coordinator_service=str(self._hint(
                hint, "coordinatorService", "coordinator_service")),
            deployment_id=str(self._hint(hint, "deploymentId", "deployment_id")),
            service=service,
            record_name=str(self._hint(
                hint, "definitionRecordName", "definition_record_name")),
            definition_digest=str(self._hint(
                hint, "definitionRecordDigest", "definition_record_digest")),
            expires_at=str(self._hint(hint, "expiresAt", "expires_at")),
            signer_key_id=str(self._hint(hint, "signerKeyId", "signer_key_id")),
        )
        definition, revision = self.resolve_definition(ref)
        if (definition.deployment_owner != ref.deployment_owner or
                definition.coordinator_service != ref.coordinator_service):
            raise ValueError("NDNSD definition authority hint mismatch")
        digest = definition.digest()
        with self._lock:
            self._definitions[digest] = definition
            self._definition_refs[digest] = ref

        activation_name = str(self._hint(
            hint, "activationRecordName", "activation_record_name"))
        activation_digest = str(self._hint(
            hint, "activationRecordDigest", "activation_record_digest"))
        if not activation_name and not activation_digest:
            return
        if (not activation_name or not activation_digest.startswith("sha256:") or
                self._activation_fetcher is None):
            raise ValueError("incomplete ACTIVE deployment hint")
        fetched = self._activation_fetcher(activation_name, activation_digest)
        if not isinstance(fetched, _VerifiedActivationRecord):
            raise PermissionError("activation transport did not attest its signer")
        activation = fetched.activation
        if fetched.signer_identity != definition.deployment_owner:
            raise PermissionError("activation Data signer is not deployment owner")
        expires = datetime.fromisoformat(activation.expires_at)
        if expires.tzinfo is None or expires <= datetime.now(timezone.utc):
            raise ValueError("activation record expired")
        if (activation.record_name != activation_name or
                activation.digest() != activation_digest or
                activation.application_identity != definition.application_identity or
                activation.deployment_owner != definition.deployment_owner or
                activation.deployment_id != definition.deployment_id or
                activation.service != definition.service or
                activation.definition_digest != digest or
                activation.revision != revision or
                activation.revision_digest != revision or
                activation.lifecycle_epoch <= 0):
            raise ValueError("activation record authority or revision binding mismatch")
        active = DeploymentRef(
            deployment_id=definition.deployment_id,
            revision=revision,
            service=definition.service,
            definition_digest=digest,
            activation_certificate_digest=activation.activation_certificate_digest,
            activation_record_name=activation_name,
            activation_record_digest=activation_digest,
            lifecycle_epoch=activation.lifecycle_epoch,
            application_identity=definition.application_identity,
            deployment_owner=definition.deployment_owner,
            coordinator_service=definition.coordinator_service,
            definition_record_name=ref.record_name,
            expires_at=activation.expires_at,
        )
        key = (definition.deployment_id, revision)
        with self._lock:
            if key in self._revoked_revisions and not activation.revoked:
                raise ValueError("revoked deployment revision cannot be reactivated")
            current = self._active_refs.get(key)
            if current is not None and current.lifecycle_epoch >= active.lifecycle_epoch:
                if (current.lifecycle_epoch == active.lifecycle_epoch and
                        current.activation_record_digest == activation.digest()):
                    return
                raise ValueError("stale activation lifecycle epoch")
            prior = self._activation_records.get(key)
            if (prior is not None and prior.digest() != activation.digest() and
                    activation.supersedes != prior.digest()):
                raise ValueError("activation rollover does not supersede current record")
            if activation.revoked:
                if prior is None or activation.supersedes != prior.digest():
                    raise ValueError("revocation does not fence an ACTIVE record")
                self._activation_records[key] = activation
                self._revoked_revisions.add(key)
                self._active_refs.pop(key, None)
                self._statuses[key] = DeploymentStatus(
                    "INACTIVE", revision, (),
                    activation.activation_certificate_digest,
                    activation.lifecycle_epoch, "REVOKED")
            else:
                self._active_refs[key] = active
                self._activation_records[key] = activation
                self._statuses[key] = DeploymentStatus(
                    "ACTIVE", revision, (), activation.activation_certificate_digest,
                    activation.lifecycle_epoch)
        if self.journal is not None:
            self.journal.append("deployment-definition", {
                "reference": asdict(ref), "definition": definition.to_dict()})
            self.journal.append("deployment-activation", {
                "activation": activation.to_dict()})
            self.journal.append("deployment-catalog-status", {
                "deploymentId": definition.deployment_id,
                "revision": revision,
                "status": asdict(self._statuses[key]),
                "activeRef": asdict(active) if not activation.revoked else None,
            })


__all__ = [
    "APPDeployment", "APPDeploymentLifecycleStore", "DeploymentCatalog",
    "DeploymentHandle", "NetworkDeploymentCatalogTransport",
]
