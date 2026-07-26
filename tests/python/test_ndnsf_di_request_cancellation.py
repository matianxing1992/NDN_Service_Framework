from __future__ import annotations

import tempfile
import time
import hashlib
import unittest
from unittest import mock
from concurrent.futures import Future
from dataclasses import replace
from types import SimpleNamespace

from ndnsf_distributed_inference.app_sdk import APPClient, RequestState
from ndnsf_distributed_inference.app_sdk.client import RequestRecoveryError
from ndnsf_distributed_inference.app_sdk.execution_control import (
    ExecutionControlEvidence, ExistingServiceExecutionControlTransport,
)
from ndnsf_distributed_inference.app_sdk.runtime_journal import RuntimeJournal
from ndnsf_distributed_inference.core import (
    ExecutionActivateMessage,
    ReadySetMember,
)


def _activation(handle) -> ExecutionActivateMessage:
    members = (
        ReadySetMember("/provider/a", "prefill", "boot-a", "sha256:ready-a"),
        ReadySetMember("/provider/b", "decode", "boot-b", "sha256:ready-b"),
    )
    return ExecutionActivateMessage(
        handle.requester_identity,
        handle.request_id,
        handle.attempt_epoch,
        "sha256:selection",
        "sha256:plan",
        members,
        handle.expires_at_ms,
        1,
        "requester-signature",
    )


class _ControlTransport:
    def __init__(self, *, fail_provider="", mutate=None):
        self.calls = []
        self.fail_provider = fail_provider
        self.mutate = mutate

    def cancel(self, provider, service_name, payload, *, timeout_ms):
        self.calls.append((provider, service_name, dict(payload), timeout_ms))
        if provider == self.fail_provider:
            raise TimeoutError("missing cancellation evidence")
        evidence = ExecutionControlEvidence(
            operation="CANCEL",
            provider=provider,
            service_name=service_name,
            requester_identity=payload["requesterIdentity"],
            request_id=payload["requestId"],
            attempt_epoch=payload["attemptEpoch"],
            activation_digest=payload["activationDigest"],
            cancellation_id=payload["cancellationId"],
            accepted=True,
            reason="CANCELLED",
            data_name=f"{provider}/NDNSF/RESPONSE/cancel",
            signer_certificate=f"{provider}/KEY/cert",
            wire_digest=f"sha256:{provider.strip('/').replace('/', '-')}",
        )
        return self.mutate(evidence) if self.mutate is not None else evidence


class DistributedCancellationTest(unittest.TestCase):
    def _pending_client(self, root, transport):
        pending = Future()
        network = SimpleNamespace(
            encode_input=lambda service, value: bytes(value),
            async_distributed_inference=lambda service, payload, **kwargs: pending,
        )
        client = APPClient(
            RuntimeJournal.for_test(root, "alice"),
            network_client=network,
            requester_identity="/alice",
            execution_control_transport=transport,
        )
        handle = client.submit(
            service="/LLM/Qwen",
            input=b"prompt",
            deployment_revision="sha256:revision",
            deadline=time.time() + 30,
        )
        return client, pending, client.bind_execution_activation(
            handle, _activation(handle))

    def test_post_activation_cancel_reaches_exact_members_before_terminal_state(self):
        with tempfile.TemporaryDirectory() as root:
            pending = Future()
            network = SimpleNamespace(
                encode_input=lambda service, value: bytes(value),
                async_distributed_inference=lambda service, payload, **kwargs: pending,
            )
            transport = _ControlTransport()
            client = APPClient(
                RuntimeJournal.for_test(root, "alice"),
                network_client=network,
                requester_identity="/alice",
                execution_control_transport=transport,
            )
            handle = client.submit(
                service="/LLM/Qwen",
                input=b"prompt",
                deployment_revision="sha256:revision",
                deadline=time.time() + 30,
            )
            activation = _activation(handle)
            bound = client.bind_execution_activation(handle, activation)

            self.assertEqual(bound.activation_digest, activation.digest())
            self.assertEqual(client.status(bound), RequestState.CERTIFIED)
            client.cancel(bound, reason="operator", attempt_epoch=1)

            self.assertEqual(
                [call[0] for call in transport.calls],
                ["/provider/a", "/provider/b"],
            )
            self.assertTrue(all(
                call[2]["requestId"] == handle.request_id
                and call[2]["attemptEpoch"] == 1
                and call[2]["activationDigest"] == activation.digest()
                and call[2]["operation"] == "CANCEL"
                for call in transport.calls
            ))
            self.assertEqual(client.status(bound), RequestState.CANCELLED)
            self.assertTrue(pending.cancelled())
            evidence = [
                record["payload"] for record in client.journal.records()
                if record["kind"] == "execution-control-evidence"
            ]
            self.assertEqual(
                {item["provider"] for item in evidence},
                {"/provider/a", "/provider/b"},
            )

    def test_missing_provider_evidence_does_not_claim_distributed_cancellation(self):
        with tempfile.TemporaryDirectory() as root:
            client, pending, handle = self._pending_client(
                root, _ControlTransport(fail_provider="/provider/b"))
            with self.assertRaisesRegex(RequestRecoveryError, "CANCELLATION_INCOMPLETE"):
                client.cancel(handle, reason="operator")
            self.assertEqual(client.status(handle), RequestState.CERTIFIED)
            self.assertFalse(pending.cancelled())
            self.assertFalse(any(
                record["kind"] == "execution-control-evidence"
                for record in client.journal.records()
            ))

    def test_stale_attempt_evidence_is_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            client, pending, handle = self._pending_client(
                root, _ControlTransport(
                    mutate=lambda item: replace(item, attempt_epoch=2)))
            with self.assertRaisesRegex(RequestRecoveryError, "CANCELLATION_INCOMPLETE"):
                client.cancel(handle)
            self.assertEqual(client.status(handle), RequestState.CERTIFIED)
            self.assertFalse(pending.cancelled())

    def test_wrong_provider_evidence_is_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            client, pending, handle = self._pending_client(
                root, _ControlTransport(
                    mutate=lambda item: replace(item, provider="/provider/other")))
            with self.assertRaisesRegex(RequestRecoveryError, "CANCELLATION_INCOMPLETE"):
                client.cancel(handle)
            self.assertEqual(client.status(handle), RequestState.CERTIFIED)
            self.assertFalse(pending.cancelled())

    def test_wrong_provider_signer_evidence_is_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            client, pending, handle = self._pending_client(
                root, _ControlTransport(
                    mutate=lambda item: replace(
                        item,
                        signer_certificate="/provider/other/KEY/cert")))
            with self.assertRaisesRegex(
                    (RequestRecoveryError, ValueError),
                    "CANCELLATION_INCOMPLETE|certified Provider"):
                client.cancel(handle)
            self.assertEqual(client.status(handle), RequestState.CERTIFIED)
            self.assertFalse(pending.cancelled())

    def test_replayed_cancellation_evidence_is_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            client, pending, handle = self._pending_client(
                root, _ControlTransport(
                    mutate=lambda item: replace(
                        item, cancellation_id="sha256:previous-cancellation")))
            with self.assertRaisesRegex(RequestRecoveryError, "CANCELLATION_INCOMPLETE"):
                client.cancel(handle)
            self.assertEqual(client.status(handle), RequestState.CERTIFIED)
            self.assertFalse(pending.cancelled())

    def test_provider_rejection_does_not_claim_distributed_cancellation(self):
        with tempfile.TemporaryDirectory() as root:
            client, pending, handle = self._pending_client(
                root, _ControlTransport(
                    mutate=lambda item: replace(
                        item, accepted=False, reason="CANCEL_REJECTED")))
            with self.assertRaisesRegex(
                    RequestRecoveryError, "CANCELLATION_INCOMPLETE"):
                client.cancel(handle)
            self.assertEqual(client.status(handle), RequestState.CERTIFIED)
            self.assertFalse(pending.cancelled())

    def test_accepted_terminal_result_is_not_revoked_by_late_cancel(self):
        with tempfile.TemporaryDirectory() as root:
            submitted = {}
            pending = Future()
            network = SimpleNamespace(encode_input=lambda service, value: bytes(value))

            def submit_network(service, payload, **kwargs):
                submitted.update(kwargs)
                return pending

            network.async_distributed_inference = submit_network
            transport = _ControlTransport()
            client = APPClient(
                RuntimeJournal.for_test(root, "alice"),
                network_client=network,
                requester_identity="/alice",
                execution_control_transport=transport,
            )
            original = client.submit(
                service="/LLM/Qwen",
                input=b"prompt",
                deployment_revision="sha256:revision",
                deadline=time.time() + 30,
            )
            bound = client.bind_execution_activation(original, _activation(original))
            result = SimpleNamespace(
                status=True,
                payload=b"accepted-result",
                error="",
                request_id=bound.request_id,
                data_name="/provider/b/result",
                signer_certificate="/provider/b/KEY/cert",
                wire_digest="sha256:terminal",
            )
            submitted["on_result"](result)
            pending.set_result(result)

            self.assertEqual(client.status(bound), RequestState.COMPLETED)
            client.cancel(bound, reason="too-late")
            self.assertEqual(client.status(bound), RequestState.COMPLETED)
            self.assertEqual(client.result(bound), b"accepted-result")
            self.assertEqual(transport.calls, [])

    def test_completed_cancellation_is_idempotent_across_requester_restart(self):
        with tempfile.TemporaryDirectory() as root:
            transport = _ControlTransport()
            client, _pending, handle = self._pending_client(root, transport)
            client.cancel(handle, reason="operator")
            self.assertEqual(len(transport.calls), 2)

            restarted_transport = _ControlTransport()
            restarted = APPClient(
                RuntimeJournal.for_test(root, "alice"),
                requester_identity="/alice",
                execution_control_transport=restarted_transport,
            )
            reopened = restarted.open_request(handle.request_id, attempt_epoch=1)
            restarted.cancel(reopened, reason="operator")
            self.assertEqual(restarted.status(reopened), RequestState.CANCELLED)
            self.assertEqual(restarted_transport.calls, [])

    def test_existing_service_transport_retains_authenticated_response_evidence(self):
        payload = {
            "operation": "CANCEL",
            "requestId": "request",
            "attemptEpoch": 3,
            "providerName": "/provider/a",
            "providerRole": "prefill",
            "requesterIdentity": "/alice",
            "activationDigest": "sha256:activation",
            "cancellationId": "sha256:cancellation",
        }
        from ndnsf_distributed_inference.app_sdk.execution_control import (
            encode_execution_control,
        )
        component = "execution-control-" + hashlib.sha256(
            encode_execution_control(payload)).hexdigest()[:24]
        response = SimpleNamespace(
            status=True,
            payload=b"schema=ndnsf-di-execution-control-v2;status=1;reason=CANCELLED;",
            error="",
            data_name=f"/provider/a/NDNSF/RESPONSE/{component}",
            signer_certificate="/provider/a/KEY/cert",
            wire_digest="sha256:wire",
        )
        network = SimpleNamespace(
            request_execution_control=mock.Mock(return_value=response))
        evidence = ExistingServiceExecutionControlTransport(network).cancel(
            "/provider/a", "/LLM/Qwen", payload, timeout_ms=700)
        self.assertTrue(evidence.accepted)
        self.assertEqual(evidence.attempt_epoch, 3)
        self.assertEqual(evidence.signer_certificate, "/provider/a/KEY/cert")
        call = network.request_execution_control.call_args
        self.assertEqual(
            call.args[:3], ("/provider/a", "prefill", "/LLM/Qwen"))
        self.assertIn(b"operation=CANCEL;", call.args[3])
        self.assertEqual(call.kwargs["timeout_ms"], 700)


if __name__ == "__main__":
    unittest.main()
