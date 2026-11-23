"""Authenticated APP-to-Provider control over an existing DI service."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import json
import re
from typing import Any, Mapping, Protocol


CONTROL_SCHEMA = "ndnsf-di-execution-control-v2"
LEGACY_CONTROL_SCHEMA = "ndnsf-di-execution-control-v1"


def _execution_control_request_prefix(wire: bytes) -> str:
    return "execution-control-" + hashlib.sha256(wire).hexdigest()[:24] + "-"


def _digest(payload: Mapping[str, Any]) -> str:
    wire = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    return "sha256:" + hashlib.sha256(wire).hexdigest()


@dataclass(frozen=True)
class ExecutionControlEvidence:
    operation: str
    provider: str
    service_name: str
    requester_identity: str
    request_id: str
    attempt_epoch: int
    activation_digest: str
    cancellation_id: str
    accepted: bool
    reason: str
    data_name: str
    signer_certificate: str
    wire_digest: str

    def __post_init__(self) -> None:
        required = (
            self.operation, self.provider, self.service_name,
            self.requester_identity, self.request_id,
            self.activation_digest, self.cancellation_id,
            self.data_name, self.signer_certificate, self.wire_digest,
        )
        if not all(required) or self.attempt_epoch <= 0:
            raise ValueError("execution-control evidence lacks authenticated binding")
        provider = self.provider.rstrip("/")
        if (
            not self.data_name.startswith(provider + "/")
            or not self.signer_certificate.startswith(provider + "/KEY/")
            or not self.wire_digest.startswith("sha256:")
        ):
            raise ValueError(
                "execution-control evidence is not bound to the certified Provider"
            )

    def to_record(self) -> dict[str, Any]:
        return {"schema": CONTROL_SCHEMA, **asdict(self), "evidence_digest": self.digest()}

    def digest(self) -> str:
        return _digest({"schema": CONTROL_SCHEMA, **asdict(self)})

    @classmethod
    def from_record(cls, payload: Mapping[str, Any]) -> "ExecutionControlEvidence":
        fields = dict(payload)
        schema = fields.pop("schema", None)
        digest = fields.pop("evidence_digest", None)
        evidence = cls(**fields)
        if schema != CONTROL_SCHEMA or digest != evidence.digest():
            raise ValueError("invalid execution-control evidence record")
        return evidence


class ExecutionControlTransport(Protocol):
    def cancel(
        self,
        provider: str,
        service_name: str,
        payload: Mapping[str, Any],
        *,
        timeout_ms: int,
    ) -> ExecutionControlEvidence:
        """Request one attempt-fenced Provider cancellation."""


def encode_execution_control(payload: Mapping[str, Any]) -> bytes:
    fields = {
        "schema": CONTROL_SCHEMA,
        "operation": str(payload["operation"]),
        "requestId": str(payload["requestId"]),
        "attemptEpoch": str(int(payload["attemptEpoch"])),
        "providerName": str(payload["providerName"]),
        "providerRole": str(payload["providerRole"]),
        "requesterIdentity": str(payload["requesterIdentity"]),
        "activationDigest": str(payload["activationDigest"]),
        "cancellationId": str(payload["cancellationId"]),
    }
    return "".join(f"{key}={value};" for key, value in fields.items()).encode()


def _parse_fields(payload: bytes) -> dict[str, str]:
    fields: dict[str, str] = {}
    for item in bytes(payload).decode("utf-8", errors="strict").split(";"):
        if not item:
            continue
        key, separator, value = item.partition("=")
        if not separator or not key or key in fields:
            raise ValueError("malformed execution-control response")
        fields[key] = value
    return fields


class ExistingServiceExecutionControlTransport:
    """Send control through the authenticated existing collaboration service."""

    def __init__(self, network_client) -> None:
        self._network_client = network_client

    def cancel(
        self,
        provider: str,
        service_name: str,
        payload: Mapping[str, Any],
        *,
        timeout_ms: int,
    ) -> ExecutionControlEvidence:
        wire = encode_execution_control(payload)
        response = self._network_client.request_execution_control(
            provider,
            str(payload["providerRole"]),
            service_name,
            wire,
            timeout_ms=timeout_ms,
        )
        fields = _parse_fields(bytes(getattr(response, "payload", b"")))
        if fields.get("schema") != CONTROL_SCHEMA or fields.get("status") not in {"0", "1"}:
            raise ValueError("invalid execution-control response schema or status")
        accepted = bool(getattr(response, "status", False)) and fields.get("status") == "1"
        data_name = str(getattr(response, "data_name", ""))
        request_component = str(getattr(response, "request_id", ""))
        if request_component.startswith("/"):
            request_component = request_component[1:]
        if (not re.fullmatch(_execution_control_request_prefix(wire) + "[0-9a-f]{32}",
                            request_component)
                or data_name.rsplit("/", 1)[-1] != request_component):
            raise ValueError(
                "execution-control response Data name does not bind the request payload"
            )
        return ExecutionControlEvidence(
            operation="CANCEL",
            provider=provider,
            service_name=service_name,
            requester_identity=str(payload["requesterIdentity"]),
            request_id=str(payload["requestId"]),
            attempt_epoch=int(payload["attemptEpoch"]),
            activation_digest=str(payload["activationDigest"]),
            cancellation_id=str(payload["cancellationId"]),
            accepted=accepted,
            reason=fields.get("reason") or str(getattr(response, "error", "")) or "REJECTED",
            data_name=data_name,
            signer_certificate=str(getattr(response, "signer_certificate", "")),
            wire_digest=str(getattr(response, "wire_digest", "")),
        )


__all__ = [
    "ExecutionControlEvidence",
    "ExecutionControlTransport",
    "ExistingServiceExecutionControlTransport",
    "encode_execution_control",
]
