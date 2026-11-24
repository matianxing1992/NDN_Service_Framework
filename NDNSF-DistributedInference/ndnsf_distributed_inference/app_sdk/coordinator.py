"""Authorized ON_DEMAND request coordinator over NDNSF Collaboration."""

from __future__ import annotations

import base64
from concurrent.futures import Future, InvalidStateError
from dataclasses import asdict
from datetime import datetime, timedelta, timezone
import json

from ndnsf import AckDecision, ProviderCapabilityHint, _ndnsf, encode_provider_capability_ack

from ndnsf.runtime_telemetry import ServiceOperationState, ServiceOperationStatus

from ..client import InferenceResult
from .contracts import DeploymentDefinitionRef, InferenceOptions
from .status import RequestState


COORDINATOR_REQUEST_SCHEMA = "ndnsf-di-coordinator-request-v1"
COORDINATOR_RESPONSE_SCHEMA = "ndnsf-di-coordinator-response-v1"
COORDINATOR_PROGRESS_SCHEMA = "ndnsf-di-coordinator-progress-v1"


def _stable_json(value: dict) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def encode_coordinator_request(
    definition: DeploymentDefinitionRef,
    payload: bytes,
    *,
    deadline: datetime,
    options: InferenceOptions,
    outer_request_id: str,
) -> bytes:
    if deadline.tzinfo is None or deadline <= datetime.now(timezone.utc):
        raise ValueError("coordinator request deadline must be aware and future")
    return _stable_json({
        "schema": COORDINATOR_REQUEST_SCHEMA,
        "definition": asdict(definition),
        "input": base64.b64encode(bytes(payload)).decode("ascii"),
        "deadline": deadline.astimezone(timezone.utc).isoformat(),
        "options": {
            "metadata": dict(options.metadata),
            "outputEncoding": options.output_encoding,
        },
        "outerRequestId": outer_request_id,
    })


def decode_coordinator_request(payload: bytes):
    value = json.loads(bytes(payload).decode("utf-8"))
    if not isinstance(value, dict) or value.get("schema") != COORDINATOR_REQUEST_SCHEMA:
        raise ValueError("invalid coordinator request schema")
    definition = DeploymentDefinitionRef(**dict(value["definition"]))
    deadline = datetime.fromisoformat(str(value["deadline"]))
    if deadline.tzinfo is None or deadline <= datetime.now(timezone.utc):
        raise TimeoutError("coordinator request deadline expired")
    options_payload = dict(value.get("options", {}))
    options = InferenceOptions(
        metadata=dict(options_payload.get("metadata", {})),
        output_encoding=str(options_payload.get(
            "outputEncoding", "application/octet-stream")),
    )
    return (
        definition,
        base64.b64decode(str(value["input"]), validate=True),
        deadline,
        options,
        str(value.get("outerRequestId", "")),
    )


def encode_coordinator_response(result: InferenceResult) -> bytes:
    return _stable_json({
        "schema": COORDINATOR_RESPONSE_SCHEMA,
        "status": bool(result.status),
        "payload": base64.b64encode(bytes(result.payload)).decode("ascii"),
        "error": str(result.error),
        "innerRequestId": str(result.request_id),
        "dataName": str(result.data_name),
        "signerCertificate": str(result.signer_certificate),
        "wireDigest": str(result.wire_digest),
    })


def decode_coordinator_response(payload: bytes, outer_request_id: str) -> InferenceResult:
    value = json.loads(bytes(payload).decode("utf-8"))
    if not isinstance(value, dict) or value.get("schema") != COORDINATOR_RESPONSE_SCHEMA:
        raise ValueError("invalid coordinator response schema")
    return InferenceResult(
        bool(value.get("status", False)),
        base64.b64decode(str(value.get("payload", "")), validate=True),
        str(value.get("error", "")),
        outer_request_id,
        str(value.get("dataName", "")),
        str(value.get("signerCertificate", "")),
        str(value.get("wireDigest", "")),
    )


class InferenceCoordinator:
    """Deployment-owner process that realizes authorized remote definitions.

    The coordinator is registered as a one-role NDNSF Collaboration service. It
    owns policy execution and the shared request/ensure path; remote requesters
    send only a frozen signed definition reference and input bytes. The existing
    signed SELECTION-STATUS channel projects preparation progress to the remote
    request handle without treating generic progress as DI readiness.
    """

    def __init__(self, client, coordinator_service: str):
        if not coordinator_service.startswith("/"):
            raise ValueError("coordinator_service must be an NDN name")
        self.client = client
        self.coordinator_service = coordinator_service

    def handle(self, payload: bytes) -> bytes:
        definition, input_payload, deadline, options, _ = (
            decode_coordinator_request(payload))
        if definition.coordinator_service != self.coordinator_service:
            raise PermissionError("definition is bound to another coordinator")
        request = self.client._request_as_coordinator(
            definition, input=input_payload, deadline=deadline, options=options,
            coordinator_service=self.coordinator_service)
        remaining = max(
            0.001, (deadline - datetime.now(timezone.utc)).total_seconds())
        result = request.result(wait_timeout=timedelta(seconds=remaining))
        return encode_coordinator_response(result)

    @staticmethod
    def _operation_state(request_state: RequestState, deployment_state: str):
        if request_state == RequestState.COMPLETED:
            return ServiceOperationState.DONE
        if request_state == RequestState.FAILED or deployment_state == "FAILED":
            return ServiceOperationState.FAILED
        if request_state == RequestState.CANCELLED or deployment_state == "CANCELLED":
            return ServiceOperationState.CANCELED
        if request_state == RequestState.EXPIRED or deployment_state == "EXPIRED":
            return ServiceOperationState.EXPIRED
        return ServiceOperationState.RUNNING

    def _report_progress(
        self, context, definition: DeploymentDefinitionRef, request,
        outer_request_id: str, sequence: int, previous_details: bytes,
    ) -> tuple[int, bytes]:
        deployment_status = request.deployment_status()
        request_state = request.status()
        roles = [{**item.to_dict(), "request_id": outer_request_id}
                 for item in deployment_status.roles]
        phase = deployment_status.state
        if roles:
            known = [float(item.get("progress", 0.0)) for item in roles]
            progress = sum(known) / len(known)
        else:
            progress = 1.0 if phase in {"READY", "ACTIVE"} else 0.0
        details = _stable_json({
            "schema": COORDINATOR_PROGRESS_SCHEMA,
            "applicationIdentity": definition.application_identity,
            "deploymentOwner": definition.deployment_owner,
            "coordinatorService": self.coordinator_service,
            "definitionDigest": definition.definition_digest,
            "deploymentRevision": request.ref.revision,
            "state": deployment_status.state,
            "phase": phase,
            "requestState": request_state.value,
            "roles": roles,
            "readinessCertificateDigest": (
                deployment_status.readiness_certificate_digest),
            "coordinatorEpoch": deployment_status.coordinator_epoch,
        })
        if details == previous_details:
            return sequence, details
        context.report_operation_status(ServiceOperationStatus(
            operation_id=f"coordinate:{outer_request_id}",
            operation="ndnsf-di-coordinate",
            service_name=self.coordinator_service,
            provider_name=definition.deployment_owner,
            request_id=outer_request_id,
            role="coordinator",
            attempt=request.ref.attempt_epoch,
            epoch=deployment_status.coordinator_epoch,
            sequence=sequence,
            state=self._operation_state(request_state, deployment_status.state),
            reason_code=deployment_status.reason,
            progress_known=True,
            progress=max(0.0, min(1.0, progress)),
            details_schema=COORDINATOR_PROGRESS_SCHEMA,
            details_payload=details,
        ))
        return sequence + 1, details

    def handle_collaboration(self, context, payload: bytes) -> None:
        definition, input_payload, deadline, options, outer_request_id = (
            decode_coordinator_request(payload))
        if definition.coordinator_service != self.coordinator_service:
            raise PermissionError("definition is bound to another coordinator")
        if not outer_request_id:
            raise ValueError("outer request identity is required")

        def start(budget_ms):
            return self.client._request_as_coordinator(
                definition, input=input_payload,
                deadline=min(deadline, datetime.now(timezone.utc) + timedelta(milliseconds=budget_ms)),
                options=options, coordinator_service=self.coordinator_service)

        sequence = 1
        last_details = b""

        def report(request):
            nonlocal sequence, last_details
            sequence, last_details = self._report_progress(
                context, definition, request, outer_request_id, sequence,
                last_details)

        request = _ndnsf._drive_di_coordinator_request(
            context, outer_request_id, self.coordinator_service, definition.deployment_owner, payload,
            int((deadline - datetime.now(timezone.utc)).total_seconds() * 1000), start, report)
        if request is None:
            return
        remaining = max(
            0.001, (deadline - datetime.now(timezone.utc)).total_seconds())
        try:
            result = request.result(wait_timeout=timedelta(seconds=remaining))
        except Exception as exc:
            result = InferenceResult(
                False, b"", str(exc)[:160] or request.status().value,
                outer_request_id)
        response = encode_coordinator_response(result)
        if context._coordinator_work_budget_ms() > 0 and datetime.now(timezone.utc) < deadline:
            context.publish_final_response(response)

    def register(self, service_provider) -> None:
        role_ack = encode_provider_capability_ack(ProviderCapabilityHint(
            provider_name=service_provider.provider_identity,
            service_name=self.coordinator_service,
            service_payload_schema="ndnsf-di-capability-v1",
            service_payload={"roles": ["coordinator"]},
        ))
        service_provider.add_collaboration_handler(
            self.coordinator_service, ["coordinator"],
            self.handle_collaboration,
            ack_handler=lambda _: AckDecision(payload=role_ack))


def submit_via_coordinator(
    service_user,
    coordinator_service: str,
    definition: DeploymentDefinitionRef,
    payload: bytes,
    *,
    deadline: datetime,
    options: InferenceOptions,
    outer_request_id: str,
    ack_timeout_ms: int = 500,
) -> Future:
    """Submit without executing Application policy in requester space."""

    future: Future = Future()
    timeout_ms = max(
        1, int((deadline - datetime.now(timezone.utc)).total_seconds() * 1000))
    wire = encode_coordinator_request(
        definition, payload, deadline=deadline, options=options,
        outer_request_id=outer_request_id)

    def on_response(response) -> None:
        if future.done():
            return
        try:
            if not response.status:
                future.set_result(InferenceResult(
                    False, b"", response.error or "COORDINATOR_REJECTED",
                    outer_request_id))
                return
            future.set_result(decode_coordinator_response(
                response.payload, outer_request_id))
        except InvalidStateError:
            pass
        except Exception as exc:
            try:
                future.set_exception(exc)
            except InvalidStateError:
                pass

    def on_timeout(_: str) -> None:
        try:
            future.set_result(InferenceResult(
                False, b"", "COORDINATOR_TIMEOUT", outer_request_id))
        except InvalidStateError:
            pass

    if timeout_ms <= 1:
        on_timeout(outer_request_id)
        return future
    service_user._request_di_coordinator_async(
        coordinator_service, definition.deployment_owner, outer_request_id, wire,
        min(max(1, ack_timeout_ms), timeout_ms - 1), timeout_ms, on_response, on_timeout)

    def forward_cancel(done):
        nonlocal wire
        request_wire, wire = wire, b""
        if done.cancelled():
            service_user._cancel_di_coordinator(
                definition.deployment_owner, outer_request_id, request_wire)

    future.add_done_callback(forward_cancel)
    return future


__all__ = [
    "COORDINATOR_PROGRESS_SCHEMA", "InferenceCoordinator", "decode_coordinator_request",
    "decode_coordinator_response", "encode_coordinator_request",
    "encode_coordinator_response", "submit_via_coordinator",
]
