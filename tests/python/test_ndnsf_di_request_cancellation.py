from __future__ import annotations

import tempfile
import threading
import time
import hashlib
import unittest
from unittest import mock
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import replace
from types import SimpleNamespace

from ndnsf_distributed_inference.app_sdk import APPClient, RequestState
from ndnsf_distributed_inference.app_sdk.client import RequestRecoveryError
from ndnsf_distributed_inference.app_sdk.execution_control import (
    ExecutionControlEvidence, ExistingServiceExecutionControlTransport,
)
from ndnsf_distributed_inference.app_sdk.runtime_journal import RuntimeJournal, RuntimeJournalError
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
            first_receipt, = (
                record["payload"] for record in client.journal.records()
                if record["kind"] == "request-cancellation-progress"
            )
            self.assertEqual(first_receipt["provider"], "/provider/a")
            retry_transport = _ControlTransport()
            restarted = APPClient(
                RuntimeJournal.for_test(root, "alice"), requester_identity="/alice",
                execution_control_transport=retry_transport,
            )
            reopened = restarted.open_request(handle.request_id)
            restarted.cancel(reopened, reason="operator retried")
            self.assertEqual([call[0] for call in retry_transport.calls], ["/provider/b"])
            self.assertEqual(retry_transport.calls[0][2]["cancellationId"],
                             first_receipt["cancellation_id"])
            terminal = restarted.open_request(handle.request_id)
            self.assertEqual(restarted.status(terminal), RequestState.CANCELLED)
            self.assertEqual(terminal.cancellation_reason, "operator")
            records = restarted.journal.records()
            self.assertEqual(sum(row["kind"] == "request-cancellation-request"
                                 for row in records), 1)
            self.assertEqual({row["payload"]["provider"] for row in records
                              if row["kind"] == "execution-control-evidence"},
                             {"/provider/a", "/provider/b"})

    def test_corrupted_cancellation_progress_is_rejected_before_retry(self):
        with tempfile.TemporaryDirectory() as root:
            client, _, handle = self._pending_client(
                root, _ControlTransport(fail_provider="/provider/b"))
            with self.assertRaisesRegex(RequestRecoveryError, "CANCELLATION_INCOMPLETE"):
                client.cancel(handle)
            receipt, = (row["payload"] for row in client.journal.records()
                        if row["kind"] == "request-cancellation-progress")
            # The journal checksum is valid, but the stored evidence digest no longer matches.
            client.journal.append("request-cancellation-progress", {
                **receipt, "wire_digest": "sha256:other-response",
            })
            transport = _ControlTransport()
            reopened = APPClient(
                RuntimeJournal.for_test(root, "alice"), requester_identity="/alice",
                execution_control_transport=transport,
            )
            with self.assertRaisesRegex(RequestRecoveryError, "CANCELLATION_EVIDENCE_INVALID"):
                reopened.cancel(reopened.open_request(handle.request_id))
            self.assertEqual(transport.calls, [])

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
            stale_transport = _ControlTransport()
            stale = APPClient(
                RuntimeJournal.for_test(root, "alice"),
                requester_identity="/alice",
                execution_control_transport=stale_transport,
            )
            stale_handle = stale.open_request(bound.request_id)
            self.assertEqual(stale.status(stale_handle), RequestState.CERTIFIED)
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
            journal_bytes = client.journal.path.read_bytes()
            with self.assertRaisesRegex(RuntimeJournalError, "REQUEST_STATE_CHANGED"):
                stale.cancel(stale_handle, reason="stale-cursor")
            self.assertEqual(stale_transport.calls, [])
            self.assertEqual(client.journal.path.read_bytes(), journal_bytes)

    def test_two_active_writers_share_one_cancellation_lease(self):
        with tempfile.TemporaryDirectory() as root:
            first_transport, second_transport = _ControlTransport(), _ControlTransport()
            client, pending, handle = self._pending_client(root, first_transport)
            peer = APPClient(
                RuntimeJournal.for_test(root, "alice"), requester_identity="/alice",
                execution_control_transport=second_transport,
            )
            same_request = peer.open_request(handle.request_id)
            unrelated = peer.submit("d", "r", b"another request")
            entered, release = threading.Event(), threading.Event()

            def pause_first(evidence):
                if evidence.provider == "/provider/a":
                    entered.set()
                    if not release.wait(timeout=5):
                        raise TimeoutError("fixture did not release cancel")
                return evidence

            first_transport.mutate = pause_first
            with ThreadPoolExecutor(max_workers=1) as runner:
                first = runner.submit(client.cancel, handle)
                try:
                    self.assertTrue(entered.wait(timeout=5))
                    with self.assertRaisesRegex(RequestRecoveryError, "CANCELLATION_IN_PROGRESS"):
                        peer.cancel(same_request, reason="overlapping cancel")
                    self.assertEqual(second_transport.calls, [])
                    peer.cancel(unrelated)
                    self.assertEqual(peer.status(unrelated), RequestState.CANCELLED)
                finally:
                    release.set()
                first.result(timeout=5)

            self.assertTrue(pending.cancelled())
            self.assertEqual(len(first_transport.calls), 2)
            journal = RuntimeJournal.for_test(root, "alice")
            kinds = [record["kind"] for record in journal.records()]
            self.assertEqual(kinds.count("request-cancellation-request"), 1)
            self.assertEqual(kinds.count("execution-control-evidence"), 2)

    def test_result_committed_during_cancel_stops_fanout_and_wins_terminal(self):
        for winner in ("/provider/a", "/provider/b"):
            with self.subTest(winner=winner), tempfile.TemporaryDirectory() as root:
                transport = _ControlTransport()
                client, pending, handle = self._pending_client(root, transport)

                def complete_during_cancel(evidence):
                    if evidence.provider == winner:
                        result = SimpleNamespace(
                            status=True, request_id=handle.request_id, payload=b"result",
                            data_name=winner + "/result", signer_certificate=winner + "/KEY/cert",
                            wire_digest="sha256:result",
                        )
                        client._network_result(handle, result)
                        pending.set_result(result)
                    return evidence

                transport.mutate = complete_during_cancel
                client.cancel(handle)

                self.assertEqual([call[0] for call in transport.calls],
                                 ["/provider/a"] if winner == "/provider/a" else
                                 ["/provider/a", "/provider/b"])
                reopened = APPClient(
                    RuntimeJournal.for_test(root, "alice"), requester_identity="/alice")
                terminal = reopened.open_request(handle.request_id)
                self.assertEqual(reopened.status(terminal), RequestState.COMPLETED)
                self.assertEqual(reopened.result(terminal), b"result")
                kinds = [record["kind"] for record in reopened.journal.records()]
                self.assertEqual(kinds.count("request-cancellation-request"), 1)
                self.assertNotIn("execution-control-evidence", kinds)

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
            encode_execution_control(payload)).hexdigest()[:24] + "-" + "a" * 32
        response = SimpleNamespace(
            status=True,
            payload=b"schema=ndnsf-di-execution-control-v2;status=1;reason=CANCELLED;",
            error="",
            request_id=f"/{component}",
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

        for change in (
            {"data_name": f"/provider/a/NDNSF/RESPONSE/{component}-other"},
            {"request_id": f"/{component[:-1]}b"},
            {"payload": response.payload + b"status=0;"},
            {"payload": b"schema=unrelated;status=1;reason=CANCELLED;"},
        ):
            with self.subTest(change=change):
                network.request_execution_control.return_value = SimpleNamespace(
                    **{**vars(response), **change})
                with self.assertRaises(ValueError):
                    ExistingServiceExecutionControlTransport(network).cancel(
                        "/provider/a", "/LLM/Qwen", payload, timeout_ms=700)

    def test_execution_control_facade_uses_unique_core_ids_for_unchanged_wire(self):
        from ndnsf_distributed_inference.app_sdk.facades import APPClient as NetworkClient

        client = NetworkClient.__new__(NetworkClient)
        service_user = SimpleNamespace(request_collaboration=mock.Mock())
        client._client = SimpleNamespace(user=service_user)
        wire = b"authenticated execution control"
        expected = "execution-control-" + hashlib.sha256(wire).hexdigest()[:24] + "-"
        request_ids = []
        for timeout in (700, 1300):
            client.request_execution_control(
                "/provider/a", "prefill", "/LLM/Qwen", wire, timeout_ms=timeout)
            call = service_user.request_collaboration.call_args
            actual = call.kwargs["request_id"]
            self.assertRegex(actual, f"^{expected}[0-9a-f]{{32}}$")
            self.assertEqual(call.kwargs["assignment_context"].request_id, actual)
            self.assertEqual(call.args[1], wire)
            request_ids.append(actual)
        self.assertNotEqual(*request_ids)


if __name__ == "__main__":
    unittest.main()
