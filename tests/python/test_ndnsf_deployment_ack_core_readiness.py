#!/usr/bin/env python3
"""Deployment ACK role capture uses core readiness metadata."""

from __future__ import annotations

import unittest
from unittest.mock import patch

from ndnsf import (
    AckCandidate,
    GenericProviderRuntimeHint,
    ProviderCapabilityHint,
    encode_provider_capability_ack,
)
from ndnsf_distributed_inference.deployment import deployment_roles_from_ack_candidate


def _candidate(payload: bytes, *, status: bool = True, message: str = "ack") -> AckCandidate:
    return AckCandidate(
        provider_name="/provider/A",
        service_name="/Inference/NativeTracer",
        request_id="req-1",
        status=status,
        message=message,
        payload=payload,
    )


def _typed_payload(*, ready: bool = True, drain_state: str = "ACTIVE",
                   reason: str = "", **service_payload) -> bytes:
    return encode_provider_capability_ack(ProviderCapabilityHint(
        provider_name="/provider/A",
        service_name="/Inference/NativeTracer",
        ready=ready,
        drain_state=drain_state,
        reason_code=reason,
        runtime_hint=GenericProviderRuntimeHint(provider_name="/provider/A"),
        service_payload_schema="ndnsf-di-capability-v1",
        service_payload=service_payload,
    ))


class DeploymentAckCoreReadinessTest(unittest.TestCase):
    def test_typed_ready_provider_records_roles(self) -> None:
        payload = _typed_payload(roles="/Backbone,/Head")

        self.assertEqual(
            deployment_roles_from_ack_candidate(_candidate(payload)),
            ["/Backbone", "/Head"],
        )

    def test_typed_draining_provider_does_not_record_ready_roles(self) -> None:
        payload = _typed_payload(roles="/Backbone", drain_state="DRAINING")

        self.assertEqual(deployment_roles_from_ack_candidate(_candidate(payload)), [])

    def test_legacy_ready_ack_keeps_existing_role_capture(self) -> None:
        payload = b"roles=/Backbone,/Merge;runtimeStatus=ready;"

        with patch.dict("os.environ", {"NDNSF_ACK_COMPATIBILITY_MODE": "mixed"}):
            self.assertEqual(
                deployment_roles_from_ack_candidate(_candidate(payload)),
                ["/Backbone", "/Merge"],
            )

    def test_explicit_model_unavailable_negative_ack_records_provisioning_role(self) -> None:
        payload = _typed_payload(
            ready=False,
            reason="ModelUnavailable",
            roles="/Backbone",
            runtimeStatus="installing",
            provisioningRole="/Backbone",
        )

        self.assertEqual(
            deployment_roles_from_ack_candidate(_candidate(payload, status=False)),
            ["/Backbone"],
        )

    def test_non_provisioning_negative_ack_does_not_record_roles(self) -> None:
        payload = _typed_payload(
            ready=False,
            reason="QUEUE_FULL",
            roles="/Backbone",
            negativeAckReason="QUEUE_FULL",
        )

        self.assertEqual(
            deployment_roles_from_ack_candidate(
                _candidate(payload, status=False, message="QUEUE_FULL")),
            [],
        )


if __name__ == "__main__":
    unittest.main()
