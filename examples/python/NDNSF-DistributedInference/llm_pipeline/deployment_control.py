"""APP-layer deployment evidence over ordinary NDNSF service requests."""

from __future__ import annotations

import json

from ndnsf_distributed_inference.app_sdk import (
    APPProvider, ProviderActionReceipt, ProviderReadiness,
)
from ndnsf_distributed_inference.core.ports import ExecutionTargetProposal


CONTROL_SERVICE_PREFIX = "/APP/Deployment/Control"
CONTROL_SCHEMA = "ndnsf-di-app-deployment-control-v1"
RESPONSE_SCHEMA = "ndnsf-di-app-deployment-control-response-v1"


def control_service(stage_index: int) -> str:
    return f"{CONTROL_SERVICE_PREFIX}/Stage/{int(stage_index)}"


class EvidenceRunnerAdapter:
    """No-op adapter proving revision/artifact binding for the fake MiniNDN gate."""

    name = "deployment-evidence"
    version = "1"

    def supports(self, target):
        return target.device == "cpu"

    def create_runner(self, target, artifacts):
        return {
            "role": target.role,
            "provider": target.provider,
            "artifacts": tuple(artifacts),
        }


class ProviderDeploymentControl:
    def __init__(self, provider: APPProvider, *, role: str, revision: str,
                 artifact_digests: tuple[str, ...], boot_epoch: str):
        self.provider = provider
        self.role = role
        self.revision = revision
        self.artifact_digests = tuple(artifact_digests)
        self.provider.register_agent(
            boot_epoch=boot_epoch,
            capabilities=("cpu", "deployment-evidence-v1"),
            capacity_by_role={role: 1},
            permission_ready=True,
        )
        self.readiness: ProviderReadiness | None = None

    def handle(self, ctx) -> None:
        try:
            request = json.loads(bytes(ctx.request).decode("utf-8"))
            if request.get("schema") != CONTROL_SCHEMA:
                raise ValueError("unsupported deployment control schema")
            action = str(request.get("action", "")).upper()
            requested_revision = str(request.get("revision", ""))
            if action == "PREPARE" and requested_revision != self.revision:
                if self.provider.active_revision:
                    raise ValueError("new revision PREPARE requires inactive Provider")
                self.revision = requested_revision
                self.readiness = None
            if requested_revision != self.revision:
                raise ValueError("deployment control revision mismatch")
            if action == "PREPARE":
                artifacts = tuple(str(item) for item in request.get("artifactDigests", ()))
                if artifacts != self.artifact_digests:
                    raise ValueError("deployment control artifact mismatch")
                self.readiness = self.provider.stage(
                    self.revision,
                    ExecutionTargetProposal(
                        self.role, self.provider.provider,
                        EvidenceRunnerAdapter.name, "cpu"),
                    artifacts,
                )
                body = {"readiness": self.readiness.to_dict()}
            elif action == "ACTIVATE":
                if self.readiness is None:
                    raise ValueError("ACTIVATE requires PREPARE readiness")
                body = {
                    "actionReceipt": self.provider.activate(
                        self.readiness).to_dict()}
            elif action == "DRAIN":
                receipts = self.provider.drain()
                if len(receipts) != 1:
                    raise ValueError("DRAIN did not produce one role receipt")
                body = {"actionReceipt": receipts[0].to_dict()}
            elif action == "DELETE":
                receipts = self.provider.delete(self.revision)
                if len(receipts) != 1:
                    raise ValueError("DELETE did not produce one role receipt")
                body = {"actionReceipt": receipts[0].to_dict()}
            else:
                raise ValueError("unsupported deployment control action")
            response = {
                "schema": RESPONSE_SCHEMA,
                "status": True,
                "action": action,
                "provider": self.provider.provider,
                "role": self.role,
                "revision": self.revision,
                **body,
            }
            ctx.ndnsf.publish_final_response(json.dumps(
                response, sort_keys=True, separators=(",", ":")).encode("utf-8"))
            print(
                "LLM_PIPELINE_DEPLOYMENT_CONTROL "
                f"action={action} provider={self.provider.provider} "
                f"role={self.role} revision={self.revision}",
                flush=True,
            )
        except Exception as exc:
            ctx.ndnsf.fail(f"deployment control failed: {type(exc).__name__}:{exc}")


def decode_control_response(payload: bytes) -> dict:
    response = json.loads(bytes(payload).decode("utf-8"))
    if (response.get("schema") != RESPONSE_SCHEMA
            or response.get("status") is not True):
        raise ValueError("invalid deployment control response")
    return response


def readiness_from_response(payload: bytes) -> ProviderReadiness:
    return ProviderReadiness.from_dict(
        decode_control_response(payload)["readiness"])


def action_from_response(payload: bytes) -> ProviderActionReceipt:
    return ProviderActionReceipt.from_dict(
        decode_control_response(payload)["actionReceipt"])


__all__ = [
    "CONTROL_SCHEMA", "EvidenceRunnerAdapter", "ProviderDeploymentControl",
    "action_from_response", "control_service", "decode_control_response",
    "readiness_from_response",
]
