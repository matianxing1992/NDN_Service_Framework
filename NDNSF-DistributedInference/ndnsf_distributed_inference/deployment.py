"""Fail-closed distributed execution lease transactions for NDNSF-DI."""

from __future__ import annotations

import base64
from dataclasses import dataclass, field
from enum import Enum
import json
import os
import time
from typing import Any, Callable, Iterable, Mapping, Protocol

from ndnsf import (
    AckCandidate,
    ExecutionLeaseBinding,
    GenericExecutionLease,
    ProviderCapabilityHint,
    ProviderExecutionLeaseTable,
    ServiceDiscoveryRecord,
    ServiceOperationState,
    ServiceOperationStatus,
    parse_ack_metadata,
    to_plain,
)


LEASE_SERVICE_NAME = "/Inference/Control/Lease"
LEASE_CODEC_SCHEMA = "ndnsf-di-execution-lease-operation-v1"


def deployment_roles_from_ack_candidate(candidate: AckCandidate) -> list[str]:
    """Return DI roles represented by a ready or provisioning ACK."""
    fields = parse_ack_metadata(bytes(candidate.payload))

    def roles_from(value: Any) -> list[str]:
        if value is None:
            return []
        if isinstance(value, str):
            return [item.strip() for item in value.split(",") if item.strip()]
        if isinstance(value, (list, tuple)):
            return [str(item).strip() for item in value if str(item).strip()]
        return [str(value).strip()] if str(value).strip() else []

    if candidate.status:
        capability_payload = fields.get("providerCapabilityHint")
        if isinstance(capability_payload, dict):
            try:
                hint = ProviderCapabilityHint.from_dict(capability_payload)
                record = ServiceDiscoveryRecord.from_provider_capability_hint(hint)
                if not record.ready_for_new_request():
                    return []
            except Exception:
                return []
        return roles_from(fields.get("roles"))
    reason = str(fields.get("negativeAckReason", candidate.message)).strip()
    if reason.replace("_", "").replace("-", "").upper() != "MODELUNAVAILABLE":
        return []
    roles = roles_from(fields.get("provisioningRole"))
    roles.extend(role for role in roles_from(fields.get("roles")) if role not in roles)
    return roles


_DEPLOYMENT_STATUS_PRIORITY = {
    "ACTIVE": 0, "IDLE": 1, "DEGRADED": 2, "DISK_RESIDENT": 3,
    "PROVISIONING": 4, "EVICTED": 5, "REJECTED": 6, "NOT_FOUND": 7,
}


def deployment_operation_status(
    deployment: dict[str, Any], *, operation: str = "DEPLOYMENT"
) -> dict[str, Any]:
    existing = deployment.get("operationStatus", deployment.get("operation_status"))
    if isinstance(existing, dict):
        try:
            return to_plain(ServiceOperationStatus.from_dict(existing))
        except Exception:
            pass
    status = str(deployment.get("status", "")).upper()
    state = {
        "PROVISIONING": ServiceOperationState.RUNNING,
        "REJECTED": ServiceOperationState.FAILED,
        "NOT_FOUND": ServiceOperationState.FAILED,
        "EVICTED": ServiceOperationState.CANCELED,
        "DEGRADED": ServiceOperationState.WAITING_INPUT,
    }.get(status, ServiceOperationState.DONE)
    progress = {
        "PROVISIONING": 0.5, "REJECTED": 0.0, "NOT_FOUND": 0.0,
        "DEGRADED": 0.75,
    }.get(status, 1.0)
    deployment_id = str(deployment.get("deploymentId", deployment.get("deployment_id", "")))
    result = ServiceOperationStatus(
        operation_id=deployment_id or operation.lower(),
        operation=operation,
        service_name=str(deployment.get("serviceName", deployment.get("service_name", ""))),
        state=state,
        reason_code=status if status in {"REJECTED", "NOT_FOUND"} else "",
        message=str(deployment.get("reason", "")) or status.lower(),
        progress=progress,
        updated_at_ms=int(deployment.get("updatedAtMs", deployment.get("updated_at_ms", 0)) or 0),
        metadata={
            "deploymentStatus": status,
            "planId": deployment.get("planId", deployment.get("plan_id", "")),
            "refCount": deployment.get("refCount", deployment.get("ref_count", 0)),
        },
    )
    return to_plain(result)


def with_deployment_operation_status(
    deployment: dict[str, Any], *, operation: str = "DEPLOYMENT"
) -> dict[str, Any]:
    result = dict(deployment)
    result["operationStatus"] = deployment_operation_status(result, operation=operation)
    return result


def deployment_sort_key(deployment: dict[str, Any]) -> tuple[int, str]:
    status = ""
    payload = deployment.get("operationStatus", deployment.get("operation_status"))
    if isinstance(payload, dict):
        try:
            operation_status = ServiceOperationStatus.from_dict(payload)
            status = str(operation_status.metadata.get("deploymentStatus", "")).upper()
            if not status:
                priority = {
                    ServiceOperationState.DONE: 0,
                    ServiceOperationState.WAITING_INPUT: 2,
                    ServiceOperationState.RUNNING: 4,
                    ServiceOperationState.CANCELED: 5,
                    ServiceOperationState.FAILED: 6,
                    ServiceOperationState.EXPIRED: 6,
                }
                return (priority.get(operation_status.state, 99),
                        str(deployment.get("deploymentId", "")))
        except Exception:
            status = ""
    if not status:
        status = str(deployment.get("status", "")).upper()
    return (_DEPLOYMENT_STATUS_PRIORITY.get(status, 99),
            str(deployment.get("deploymentId", "")))


@dataclass(frozen=True)
class DeploymentRecord:
    """Descriptive deployment metadata; never execution or eviction authority."""

    plan_id: str
    plan_digest: str
    creator: str
    service_name: str
    role_providers: Mapping[str, str] = field(default_factory=dict)
    artifact_references: Mapping[str, str] = field(default_factory=dict)
    readiness: Mapping[str, str] = field(default_factory=dict)


class LeaseOperation(str, Enum):
    PREPARE = "PREPARE"
    COMMIT = "COMMIT"
    ABORT = "ABORT"
    RENEW = "RENEW"
    RELEASE = "RELEASE"


@dataclass(frozen=True)
class LeaseOperationRequest:
    operation: LeaseOperation
    request_id: str
    plan_digest: str
    idempotency_key: str
    target_service_name: str
    lease_id: str = ""
    provider_epoch: str = ""
    resource_binding_schema: str = "ndnsf-di-binding-v1"
    resource_binding_proof: bytes = b""
    roles: tuple[str, ...] = ()
    expires_at_ms: int = 0
    schema: str = LEASE_CODEC_SCHEMA

    def to_bytes(self) -> bytes:
        payload = {
            "schema": self.schema,
            "targetServiceName": self.target_service_name,
            "operation": self.operation.value,
            "requestId": self.request_id,
            "planDigest": self.plan_digest,
            "idempotencyKey": self.idempotency_key,
            "leaseId": self.lease_id,
            "providerEpoch": self.provider_epoch,
            "resourceBindingSchema": self.resource_binding_schema,
            "resourceBindingProof": base64.b64encode(
                self.resource_binding_proof
            ).decode("ascii"),
            "roles": list(self.roles),
            "expiresAtMs": self.expires_at_ms,
        }
        return json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()

    @classmethod
    def from_bytes(cls, wire: bytes) -> "LeaseOperationRequest":
        payload = _decode_payload(wire)
        _require_schema(payload)
        try:
            operation = LeaseOperation(str(payload["operation"]))
            proof = base64.b64decode(
                str(payload.get("resourceBindingProof", "")), validate=True
            )
        except (KeyError, ValueError) as exc:
            raise ValueError("invalid lease operation request") from exc
        request = cls(
            operation=operation,
            request_id=str(payload.get("requestId", "")),
            plan_digest=str(payload.get("planDigest", "")),
            idempotency_key=str(payload.get("idempotencyKey", "")),
            target_service_name=str(payload.get("targetServiceName", "")),
            lease_id=str(payload.get("leaseId", "")),
            provider_epoch=str(payload.get("providerEpoch", "")),
            resource_binding_schema=str(
                payload.get("resourceBindingSchema", "ndnsf-di-binding-v1")
            ),
            resource_binding_proof=proof,
            roles=tuple(str(role) for role in payload.get("roles", [])),
            expires_at_ms=int(payload.get("expiresAtMs", 0) or 0),
        )
        if (
            not request.request_id
            or not request.plan_digest
            or not request.idempotency_key
            or not request.target_service_name
        ):
            raise ValueError("lease request is missing required binding fields")
        return request


@dataclass(frozen=True)
class LeaseOperationResponse:
    status: bool
    operation: LeaseOperation
    reason_code: str
    lease_id: str = ""
    provider_epoch: str = ""
    state: str = ""
    expires_at_ms: int = 0
    execution_deadline_ms: int = 0
    conflict_keys: tuple[str, ...] = ()
    retry_after_ms: int = 0
    schema: str = LEASE_CODEC_SCHEMA

    def to_bytes(self) -> bytes:
        return json.dumps(
            {
                "schema": self.schema,
                "status": self.status,
                "operation": self.operation.value,
                "reasonCode": self.reason_code,
                "leaseId": self.lease_id,
                "providerEpoch": self.provider_epoch,
                "state": self.state,
                "expiresAtMs": self.expires_at_ms,
                "executionDeadlineMs": self.execution_deadline_ms,
                "conflictKeys": list(self.conflict_keys),
                "retryAfterMs": self.retry_after_ms,
            },
            sort_keys=True,
            separators=(",", ":"),
        ).encode()

    @classmethod
    def from_bytes(cls, wire: bytes) -> "LeaseOperationResponse":
        payload = _decode_payload(wire)
        _require_schema(payload)
        try:
            operation = LeaseOperation(str(payload["operation"]))
        except (KeyError, ValueError) as exc:
            raise ValueError("invalid lease operation response") from exc
        return cls(
            status=bool(payload.get("status", False)),
            operation=operation,
            reason_code=str(payload.get("reasonCode", "")),
            lease_id=str(payload.get("leaseId", "")),
            provider_epoch=str(payload.get("providerEpoch", "")),
            state=str(payload.get("state", "")),
            expires_at_ms=int(payload.get("expiresAtMs", 0) or 0),
            execution_deadline_ms=int(payload.get("executionDeadlineMs", 0) or 0),
            conflict_keys=tuple(str(key) for key in payload.get("conflictKeys", [])),
            retry_after_ms=int(payload.get("retryAfterMs", 0) or 0),
        )


def _decode_payload(wire: bytes) -> dict[str, Any]:
    try:
        payload = json.loads(bytes(wire).decode())
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("malformed lease operation payload") from exc
    if not isinstance(payload, dict):
        raise ValueError("lease operation payload must be an object")
    return payload


def _require_schema(payload: Mapping[str, Any]) -> None:
    if payload.get("schema") != LEASE_CODEC_SCHEMA:
        raise ValueError("unsupported lease operation schema")


class LeaseTransport(Protocol):
    def request(self, provider: str, payload: bytes) -> bytes:
        ...


class NdnsfLeaseTransport:
    """Carry lease operations over NDNSF's authenticated Targeted service path."""

    def __init__(
        self, service_user: Any, *, timeout_ms: int = 5000, retries: int = 2
    ) -> None:
        self._service_user = service_user
        self._timeout_ms = int(timeout_ms)
        self._retries = max(0, int(retries))

    def request(self, provider: str, payload: bytes) -> bytes:
        error = "execution lease service failed"
        for attempt in range(self._retries + 1):
            try:
                response = self._service_user.request_service_targeted(
                    provider,
                    LEASE_SERVICE_NAME,
                    payload,
                    timeout_ms=self._timeout_ms,
                )
                if response.status:
                    return bytes(response.payload)
                error = response.error or error
            except Exception as exc:
                error = str(exc) or error
            if attempt < self._retries:
                time.sleep(0.05 * (attempt + 1))
        raise RuntimeError(error)


@dataclass(frozen=True)
class ProviderLeaseAssignment:
    provider: str
    roles: tuple[str, ...]
    resource_binding_proof: bytes
    resource_binding_schema: str = "ndnsf-di-binding-v1"


@dataclass(frozen=True)
class CommittedProviderLease:
    assignment: ProviderLeaseAssignment
    lease_id: str
    provider_epoch: str
    expires_at_ms: int
    conflict_keys: tuple[str, ...]


@dataclass(frozen=True)
class DistributedLeaseSet:
    request_id: str
    plan_digest: str
    service_name: str
    leases: tuple[CommittedProviderLease, ...]


class LeaseTransactionError(RuntimeError):
    def __init__(self, phase: str, provider: str, response: LeaseOperationResponse):
        super().__init__(f"{phase} failed for {provider}: {response.reason_code}")
        self.phase = phase
        self.provider = provider
        self.response = response


class DistributedLeaseTransaction:
    """User-owned prepare-all/commit-all transaction over secured NDNSF calls."""

    def __init__(self, transport: LeaseTransport):
        self._transport = transport

    def acquire(
        self,
        *,
        request_id: str,
        plan_digest: str,
        service_name: str,
        assignments: Iterable[ProviderLeaseAssignment],
        expires_at_ms: int,
        capacity_wait_ms: int = 0,
        capacity_poll_ms: int = 100,
        reservation_ttl_ms: int = 0,
    ) -> DistributedLeaseSet:
        assignments = tuple(assignments)
        deadline = time.monotonic() + max(0, capacity_wait_ms) / 1000.0
        attempt = 0
        while True:
            attempt += 1
            attempt_expires_at_ms = (
                int(time.time() * 1000) + max(1, int(reservation_ttl_ms))
                if reservation_ttl_ms > 0
                else expires_at_ms
            )
            try:
                return self._acquire_once(
                    request_id=request_id,
                    plan_digest=plan_digest,
                    service_name=service_name,
                    assignments=assignments,
                    expires_at_ms=attempt_expires_at_ms,
                    attempt=attempt,
                )
            except LeaseTransactionError as error:
                if (
                    error.response.reason_code != "LEASE_CAPACITY_REJECTED"
                    or capacity_wait_ms <= 0
                    or time.monotonic() >= deadline
                ):
                    raise
                delay_ms = error.response.retry_after_ms or max(1, capacity_poll_ms)
                remaining_s = deadline - time.monotonic()
                if remaining_s <= 0:
                    raise
                time.sleep(min(delay_ms / 1000.0, remaining_s))

    def _acquire_once(
        self,
        *,
        request_id: str,
        plan_digest: str,
        service_name: str,
        assignments: tuple[ProviderLeaseAssignment, ...],
        expires_at_ms: int,
        attempt: int,
    ) -> DistributedLeaseSet:
        prepared: list[CommittedProviderLease] = []
        committed: list[CommittedProviderLease] = []
        try:
            for assignment in assignments:
                response = self._call(
                    assignment.provider,
                    LeaseOperationRequest(
                        operation=LeaseOperation.PREPARE,
                        request_id=request_id,
                        plan_digest=plan_digest,
                        idempotency_key=self._key(
                            "prepare", request_id, assignment.provider, str(attempt)
                        ),
                        target_service_name=service_name,
                        resource_binding_schema=assignment.resource_binding_schema,
                        resource_binding_proof=assignment.resource_binding_proof,
                        roles=assignment.roles,
                        expires_at_ms=expires_at_ms,
                    ),
                )
                if not response.status:
                    raise LeaseTransactionError("prepare", assignment.provider, response)
                prepared.append(
                    CommittedProviderLease(
                        assignment=assignment,
                        lease_id=response.lease_id,
                        provider_epoch=response.provider_epoch,
                        expires_at_ms=response.expires_at_ms,
                        conflict_keys=response.conflict_keys,
                    )
                )

            for lease in prepared:
                response = self._call(
                    lease.assignment.provider,
                    LeaseOperationRequest(
                        operation=LeaseOperation.COMMIT,
                        request_id=request_id,
                        plan_digest=plan_digest,
                        idempotency_key=self._key(
                            "commit", request_id, lease.assignment.provider, lease.lease_id
                        ),
                        target_service_name=service_name,
                        lease_id=lease.lease_id,
                        provider_epoch=lease.provider_epoch,
                    ),
                )
                if not response.status:
                    raise LeaseTransactionError(
                        "commit", lease.assignment.provider, response
                    )
                committed.append(lease)
        except Exception:
            self._cleanup(request_id, plan_digest, service_name, prepared, committed)
            raise

        return DistributedLeaseSet(
            request_id, plan_digest, service_name, tuple(committed)
        )

    def run(self, *, execute: Callable[[DistributedLeaseSet], Any], **kwargs: Any) -> Any:
        lease_set = self.acquire(**kwargs)
        try:
            return execute(lease_set)
        finally:
            self.release(lease_set)

    def release(self, lease_set: DistributedLeaseSet) -> None:
        for lease in lease_set.leases:
            self._best_effort(
                lease.assignment.provider,
                LeaseOperationRequest(
                    operation=LeaseOperation.RELEASE,
                    request_id=lease_set.request_id,
                    plan_digest=lease_set.plan_digest,
                    idempotency_key=self._key(
                        "release",
                        lease_set.request_id,
                        lease.assignment.provider,
                        lease.lease_id,
                    ),
                    target_service_name=lease_set.service_name,
                    lease_id=lease.lease_id,
                    provider_epoch=lease.provider_epoch,
                ),
            )

    def _cleanup(
        self,
        request_id: str,
        plan_digest: str,
        service_name: str,
        prepared: Iterable[CommittedProviderLease],
        committed: Iterable[CommittedProviderLease],
    ) -> None:
        committed_ids = {lease.lease_id for lease in committed}
        for lease in prepared:
            operation = (
                LeaseOperation.RELEASE
                if lease.lease_id in committed_ids
                else LeaseOperation.ABORT
            )
            self._best_effort(
                lease.assignment.provider,
                LeaseOperationRequest(
                    operation=operation,
                    request_id=request_id,
                    plan_digest=plan_digest,
                    idempotency_key=self._key(
                        operation.value.lower(),
                        request_id,
                        lease.assignment.provider,
                        lease.lease_id,
                    ),
                    target_service_name=service_name,
                    lease_id=lease.lease_id,
                    provider_epoch=lease.provider_epoch,
                ),
            )

    def _call(
        self, provider: str, request: LeaseOperationRequest
    ) -> LeaseOperationResponse:
        response = LeaseOperationResponse.from_bytes(
            self._transport.request(provider, request.to_bytes())
        )
        if response.operation is not request.operation:
            raise ValueError(
                "lease response operation does not match the current request"
            )
        return response

    def _best_effort(self, provider: str, request: LeaseOperationRequest) -> None:
        try:
            self._call(provider, request)
        except Exception:
            pass

    @staticmethod
    def _key(
        operation: str, request_id: str, provider: str, discriminator: str = ""
    ) -> str:
        suffix = f":{discriminator}" if discriminator else ""
        return f"{operation}:{request_id}:{provider}{suffix}"


class PythonExecutionLeaseProviderAdapter:
    """Python provider adapter over the canonical C++ lease table."""

    def __init__(
        self,
        provider_name: str,
        target_service_name: str,
        conflict_key_resolver: Callable[
            [LeaseOperationRequest, Mapping[str, str]], Iterable[str]
        ],
        *,
        provider_epoch: str = "",
    ) -> None:
        self.provider_name = provider_name
        self.target_service_name = target_service_name
        self._table = ProviderExecutionLeaseTable(provider_epoch)
        self._conflict_key_resolver = conflict_key_resolver

    @property
    def provider_epoch(self) -> str:
        return self._table.provider_epoch

    @property
    def table(self) -> ProviderExecutionLeaseTable:
        return self._table

    def handle(self, context: Mapping[str, str], payload: bytes, now_ms: int) -> bytes:
        request = LeaseOperationRequest.from_bytes(payload)
        requester = str(context.get("requesterIdentity", ""))
        provider = str(context.get("providerName", ""))
        service = str(context.get("serviceName", ""))
        wire_request_id = str(context.get("requestId", ""))
        if (
            not requester
            or provider != self.provider_name
            or service != LEASE_SERVICE_NAME
            or not wire_request_id
            or request.target_service_name != self.target_service_name
        ):
            return LeaseOperationResponse(
                False, request.operation, "LEASE_BINDING_MISMATCH"
            ).to_bytes()

        if request.operation is LeaseOperation.PREPARE:
            lease = GenericExecutionLease()
            lease.provider_name = self.provider_name
            lease.requester_name = requester
            lease.request_id = request.request_id
            lease.service_name = self.target_service_name
            lease.plan_digest = request.plan_digest
            lease.resource_binding_schema = request.resource_binding_schema
            lease.resource_binding_proof = request.resource_binding_proof
            lease.conflict_keys = list(self._conflict_key_resolver(request, context))
            if not lease.conflict_keys:
                return LeaseOperationResponse(
                    False, request.operation, "LEASE_CAPACITY_REJECTED"
                ).to_bytes()
            lease.expires_at_ms = request.expires_at_ms
            lease.idempotency_key = request.idempotency_key
            result = self._table.prepare(lease, now_ms)
        elif request.operation is LeaseOperation.COMMIT:
            result = self._table.commit(
                request.lease_id,
                request.provider_epoch,
                requester,
                request.idempotency_key,
                now_ms,
            )
        elif request.operation is LeaseOperation.ABORT:
            result = self._table.abort(
                request.lease_id,
                request.provider_epoch,
                requester,
                request.idempotency_key,
                now_ms,
            )
        elif request.operation is LeaseOperation.RENEW:
            result = self._table.renew(
                request.lease_id,
                request.provider_epoch,
                requester,
                request.idempotency_key,
                now_ms,
                request.expires_at_ms,
            )
        else:
            result = self._table.release(
                request.lease_id,
                request.provider_epoch,
                requester,
                request.idempotency_key,
                now_ms,
            )
        return _response_from_native(request.operation, result).to_bytes()


def register_python_execution_lease_service(
    service_provider: Any,
    adapter: PythonExecutionLeaseProviderAdapter,
    *,
    clock_ms: Callable[[], int] | None = None,
) -> None:
    """Register the Python adapter on the ordinary authenticated NDNSF service."""

    clock = clock_ms or (lambda: int(time.time() * 1000))
    service_provider.add_context_handler(
        LEASE_SERVICE_NAME,
        lambda context, payload: adapter.handle(context, payload, clock()),
    )


def _response_from_native(operation: LeaseOperation, result: Any) -> LeaseOperationResponse:
    lease = result.lease
    state = getattr(lease.state, "name", str(lease.state)).split(".")[-1]
    return LeaseOperationResponse(
        status=bool(result.status),
        operation=operation,
        reason_code=str(result.reason_code),
        lease_id=str(lease.lease_id),
        provider_epoch=str(lease.provider_epoch),
        state=state,
        expires_at_ms=int(lease.expires_at_ms),
        execution_deadline_ms=int(lease.execution_deadline_ms),
        conflict_keys=tuple(lease.conflict_keys),
        retry_after_ms=int(result.retry_after_ms),
    )


def discover_deployments(service_user: Any, service_name: str = "") -> list[dict[str, Any]]:
    """Read descriptive deployment records from NDNSD without granting authority."""

    try:
        service_user._native.pump(50)
    except Exception:
        pass
    deployments: list[dict[str, Any]] = []
    seen: set[str] = set()
    for entry in service_user.get_ndnsd_services():
        metadata = entry.get("serviceMetaInfo", {})
        if not isinstance(metadata, dict) or not metadata.get("deployments"):
            continue
        try:
            records = json.loads(str(metadata["deployments"]))
        except (TypeError, json.JSONDecodeError):
            continue
        for record in records if isinstance(records, list) else ():
            if not isinstance(record, dict):
                continue
            record_service = str(
                record.get("serviceName", record.get("service_name", ""))
            )
            deployment_id = str(
                record.get("deploymentId", record.get("deployment_id", ""))
            )
            if (
                deployment_id
                and deployment_id not in seen
                and (not service_name or record_service == service_name)
            ):
                seen.add(deployment_id)
                deployments.append(dict(record))
    rank = {"ACTIVE": 0, "IDLE": 1, "DISK_RESIDENT": 2, "EVICTED": 3}
    deployments.sort(
        key=lambda item: (
            rank.get(str(item.get("status", "")).upper(), 9),
            str(item.get("deploymentId", item.get("deployment_id", ""))),
        )
    )
    return deployments


def get_deployment(service_user: Any, deployment_id: str) -> dict[str, Any] | None:
    return next(
        (
            item
            for item in discover_deployments(service_user)
            if str(item.get("deploymentId", item.get("deployment_id", "")))
            == deployment_id
        ),
        None,
    )


def wait_deployment(
    service_user: Any,
    deployment_id: str,
    *,
    timeout_ms: int = 60000,
    target_status: str = "ACTIVE",
) -> dict[str, Any] | None:
    deadline = time.monotonic() + max(0, timeout_ms) / 1000.0
    while time.monotonic() < deadline:
        deployment = get_deployment(service_user, deployment_id)
        if deployment and str(deployment.get("status", "")) == target_status:
            return deployment
        time.sleep(0.1)
    return get_deployment(service_user, deployment_id)


def deployment_role_provider_preference(
    service_user: Any, deployment_id: str
) -> str:
    deployment = get_deployment(service_user, deployment_id)
    if not deployment or str(deployment.get("status", "")) not in {"ACTIVE", "DEGRADED"}:
        return ""
    preferences: list[str] = []
    fragment_map = deployment.get("fragmentMap", deployment.get("fragment_map", {}))
    for role, providers in dict(fragment_map).items():
        if not providers:
            continue
        first = providers[0]
        provider = str(first.get("provider", "")) if isinstance(first, dict) else str(first)
        if provider:
            preferences.append(f"{role}=>{provider}")
    return ";".join(preferences) + (";" if preferences else "")


def request_collaboration_with_deployment(
    service_user: Any,
    service_name: str,
    payload: bytes,
    *,
    deployment_id: str = "",
    **kwargs: Any,
) -> Any:
    """Apply descriptive deployment placement, then call generic collaboration."""

    preference = (
        deployment_role_provider_preference(service_user, deployment_id)
        if deployment_id
        else ""
    )
    previous = os.environ.get("NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE", "")
    if preference:
        os.environ["NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE"] = preference
    try:
        return service_user.request_collaboration(
            service_name, payload, **kwargs
        )
    finally:
        if preference:
            if previous:
                os.environ["NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE"] = previous
            else:
                os.environ.pop("NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE", None)
