from __future__ import annotations

import tempfile
import os
import unittest
from concurrent.futures import Future
from types import SimpleNamespace
from unittest import mock

from ndnsf_distributed_inference.app_sdk import APPClient
from ndnsf_distributed_inference.app_sdk.runtime_journal import (
    RuntimeJournal as RuntimeJournalImpl,
)
from ndnsf_distributed_inference.app_sdk.contracts import (
    InferenceRequestHandle, RequestEnvelopeReference,
)
from ndnsf_distributed_inference.app_sdk.client import RequestRecoveryError
from ndnsf_distributed_inference.core.ports import (
    AssignmentProposal, ExecutionOutcome, ValidatedExecutionIntent,
)
from ndnsf_distributed_inference.app_sdk.status import RequestState


def RuntimeJournal(state_root, identity, *, quota_bytes=64 << 20):
    return RuntimeJournalImpl.for_test(
        state_root, identity, quota_bytes=quota_bytes)


class RequestHandleTest(unittest.TestCase):
    def test_submit_reopen_status_wait_result_stream_across_restart(self):
        with tempfile.TemporaryDirectory() as root:
            client=APPClient(RuntimeJournal(root,"alice"),executor=lambda p:p.upper())
            handle=client.submit("d","r",b"hello")
            restarted=APPClient(RuntimeJournal(root,"alice"))
            reopened=restarted.open_request(handle.request_id)
            self.assertEqual(restarted.wait(reopened),RequestState.SUCCEEDED)
            self.assertEqual(restarted.result(reopened),b"HELLO")
            self.assertEqual([e.state for e in restarted.stream(reopened)],
                             [RequestState.CREATED, RequestState.PLANNING,
                              RequestState.EXECUTING, RequestState.COMPLETED])

    def test_missing_handle_and_cancel(self):
        with tempfile.TemporaryDirectory() as root:
            client=APPClient(RuntimeJournal(root,"alice"))
            with self.assertRaises(KeyError): client.open_request("missing")

    def test_atomic_intent_and_optional_outcome_hooks(self):
        with tempfile.TemporaryDirectory() as root:
            client=APPClient(RuntimeJournal(root,"requester"))
            assignment=AssignmentProposal("a","p","v",{"r":"/p"})
            intent=ValidatedExecutionIntent(
                "intent","request",1,1,"v","p",assignment,
                "sha256:lease",1,0)
            committed=client.prepare_intent(intent)
            self.assertEqual(committed.state,"COMMITTED")
            self.assertEqual(client.emit_outcome(
                ExecutionOutcome("request",1,"SUCCEEDED","sha256:result"),
                idempotency_key="outcome"),{})
            handle=client.submit("d","r",b"hello"); client.cancel(handle)
            self.assertEqual(client.status(handle),RequestState.CANCELLED)

    def test_prepare_session_and_deploy_plan_alias_match(self):
        with tempfile.TemporaryDirectory() as root:
            client=APPClient(RuntimeJournal(root,"alice"))
            marker=object(); self.assertIs(client.prepare_session(marker),marker)
            self.assertIs(client.deploy_plan(marker),marker)

    def test_canonical_submit_binds_durable_and_wire_request_identity(self):
        with tempfile.TemporaryDirectory() as root:
            network = SimpleNamespace(
                encode_input=mock.Mock(return_value=b"encoded-prompt"),
            )

            def submit_network(service, payload, **kwargs):
                self.assertEqual(service, "/LLM/Qwen")
                self.assertEqual(payload, b"encoded-prompt")
                result = SimpleNamespace(
                    status=True, payload=b"network-result", error="",
                    request_id=kwargs["request_id"],
                    data_name="/provider/NDNSF/RESPONSE/result",
                    signer_certificate="/provider/KEY/cert",
                    wire_digest="sha256:" + "b" * 64,
                )
                kwargs["on_result"](result)
                future = Future(); future.set_result(result)
                return future

            network.async_distributed_inference = mock.Mock(
                side_effect=submit_network)
            client = APPClient(
                RuntimeJournal(root, "alice"), network_client=network)
            handle = client.submit(
                service="/LLM/Qwen",
                input={"prompt": "hello"},
                deployment_revision="sha256:" + "a" * 64,
                deadline=60,
            )

            self.assertEqual(client.wait(handle), RequestState.COMPLETED)
            self.assertIsInstance(handle, InferenceRequestHandle)
            self.assertEqual(handle.requester_identity, "alice")
            self.assertEqual(handle.attempt_epoch, 1)
            self.assertIsInstance(
                handle.envelope_reference, RequestEnvelopeReference)
            self.assertEqual(
                handle.envelope_reference.wire_digest,
                handle.envelope_digest)
            self.assertEqual(client.result(handle), b"network-result")
            kwargs = network.async_distributed_inference.call_args.kwargs
            self.assertEqual(kwargs["request_id"], handle.request_id)
            self.assertNotIn(b"encoded-prompt", client.journal.path.read_bytes())
            self.assertFalse(
                (client.journal.spool / f"{handle.request_id}.json").exists())
            rendezvous = [
                record["payload"] for record in client.journal.records()
                if record["kind"] == "result-rendezvous"
            ][-1]
            self.assertEqual(
                rendezvous["provider_result_data_name"],
                "/provider/NDNSF/RESPONSE/result",
            )
            self.assertEqual(
                rendezvous["signer_certificate"], "/provider/KEY/cert")
            self.assertEqual(
                rendezvous["network_wire_digest"], "sha256:" + "b" * 64)

    def test_request_and_result_each_use_one_grouped_durable_commit(self):
        with tempfile.TemporaryDirectory() as root:
            network = SimpleNamespace(
                encode_input=lambda service, value: bytes(value))

            def submit_network(service, payload, **kwargs):
                result = SimpleNamespace(
                    status=True,
                    payload=b"result",
                    error="",
                    request_id=kwargs["request_id"],
                    data_name="/provider/result",
                    signer_certificate="/provider/KEY/cert",
                    wire_digest="sha256:" + "c" * 64,
                )
                kwargs["on_result"](result)
                future = Future()
                future.set_result(result)
                return future

            network.async_distributed_inference = submit_network
            journal = RuntimeJournal(root, "alice")
            client = APPClient(journal, network_client=network)
            with mock.patch("os.fsync", wraps=os.fsync) as fsync:
                handle = client.submit(
                    service="/LLM/Qwen",
                    input=b"prompt",
                    deployment_revision="sha256:" + "a" * 64,
                    deadline=60,
                )

            self.assertEqual(client.status(handle), RequestState.COMPLETED)
            self.assertEqual(client.result(handle), b"result")
            self.assertEqual(fsync.call_count, 2)

    def test_reopen_is_identity_attempt_and_envelope_fenced(self):
        with tempfile.TemporaryDirectory() as root:
            client = APPClient(RuntimeJournal(root, "alice"))
            handle = client.submit("d", "r", b"payload")
            restarted = APPClient(RuntimeJournal(root, "alice"))
            self.assertEqual(
                restarted.open_request(handle.request_id, attempt_epoch=1),
                handle,
            )
            with self.assertRaisesRegex(RequestRecoveryError, "ATTEMPT_EPOCH"):
                restarted.open_request(handle.request_id, attempt_epoch=2)

            target = restarted.journal.spool / f"{handle.request_id}.json"
            target.write_bytes(b"tampered")
            tampered = APPClient(RuntimeJournal(root, "alice"))
            with self.assertRaisesRegex(RequestRecoveryError, "ENVELOPE"):
                tampered.open_request(handle.request_id, attempt_epoch=1)
            self.assertEqual(
                tampered.status(tampered._request_handles[handle.request_id]),
                RequestState.FAILED,
            )

    def test_cancellation_is_idempotent_and_attempt_fenced(self):
        with tempfile.TemporaryDirectory() as root:
            client = APPClient(RuntimeJournal(root, "alice"))
            handle = client.submit("d", "r", b"payload")
            with self.assertRaisesRegex(RequestRecoveryError, "ATTEMPT_EPOCH"):
                client.cancel(handle, reason="operator", attempt_epoch=2)
            client.cancel(handle, reason="operator", attempt_epoch=1)
            client.cancel(handle, reason="operator", attempt_epoch=1)
            self.assertEqual(client.status(handle), RequestState.CANCELLED)
            cancelled = [
                event for event in client.stream(handle)
                if event.state == RequestState.CANCELLED
            ]
            self.assertEqual(len(cancelled), 1)
            reopened = APPClient(RuntimeJournal(root, "alice")).open_request(
                handle.request_id, attempt_epoch=1)
            self.assertEqual(reopened.cancellation_reason, "operator")

    def test_wire_request_identity_mismatch_fails_closed(self):
        with tempfile.TemporaryDirectory() as root:
            network = SimpleNamespace(encode_input=lambda service, value: bytes(value))

            def submit_network(service, payload, **kwargs):
                result = SimpleNamespace(
                    status=True, payload=b"wrong", error="", request_id="other")
                kwargs["on_result"](result)
                future = Future(); future.set_result(result)
                return future

            network.async_distributed_inference = submit_network
            client = APPClient(
                RuntimeJournal(root, "alice"), network_client=network)
            handle = client.submit(
                service="/svc", input=b"request", deployment_revision="r")
            self.assertEqual(client.wait(handle), RequestState.FAILED)
            with self.assertRaises(RuntimeError):
                client.result(handle)

    def test_synchronous_distributed_inference_delegates_to_durable_submit(self):
        with tempfile.TemporaryDirectory() as root:
            network = SimpleNamespace(encode_input=lambda service, value: bytes(value))

            def submit_network(service, payload, **kwargs):
                result = SimpleNamespace(
                    status=True, payload=b"response", error="",
                    request_id=kwargs["request_id"],
                    data_name="/provider/result",
                    signer_certificate="/provider/KEY/cert",
                    wire_digest="sha256:" + "c" * 64)
                kwargs["on_result"](result)
                future = Future(); future.set_result(result)
                return future

            network.async_distributed_inference = mock.Mock(
                side_effect=submit_network)
            client = APPClient(
                RuntimeJournal(root, "alice"), network_client=network)
            result = client.distributed_inference(
                "/svc", b"request", deployment_revision="sha256:" + "a" * 64,
                timeout_ms=1000)
            self.assertTrue(result.status)
            self.assertEqual(result.payload, b"response")
            self.assertEqual(result.data_name, "/provider/result")
            self.assertEqual(result.signer_certificate, "/provider/KEY/cert")
            self.assertEqual(result.wire_digest, "sha256:" + "c" * 64)
            self.assertEqual(
                result.request_id,
                network.async_distributed_inference.call_args.kwargs["request_id"])

    def test_synchronous_result_uses_callback_payload_without_spool_reread(self):
        with tempfile.TemporaryDirectory() as root:
            network = SimpleNamespace(encode_input=lambda service, value: bytes(value))

            def submit_network(service, payload, **kwargs):
                result = SimpleNamespace(
                    status=True, payload=b"response", error="",
                    request_id=kwargs["request_id"])
                kwargs["on_result"](result)
                future = Future(); future.set_result(result)
                return future

            network.async_distributed_inference = submit_network
            journal = RuntimeJournal(root, "alice")
            client = APPClient(journal, network_client=network)
            with mock.patch.object(
                    journal, "read_envelope",
                    side_effect=AssertionError("same-process result reread")):
                result = client.distributed_inference(
                    "/svc", b"request", deployment_revision="revision",
                    timeout_ms=1000)
            self.assertEqual(result.payload, b"response")

            restarted = APPClient(RuntimeJournal(root, "alice"))
            handle = restarted.open_request(result.request_id)
            self.assertEqual(restarted.result(handle), b"response")

    def test_async_result_persistence_failure_becomes_terminal(self):
        with tempfile.TemporaryDirectory() as root:
            submitted = {}
            native_future = Future()
            network = SimpleNamespace(encode_input=lambda service, value: bytes(value))

            def submit_network(service, payload, **kwargs):
                submitted.update(kwargs)
                return native_future

            network.async_distributed_inference = submit_network
            journal = RuntimeJournal(root, "alice")
            original_commit = journal.commit_prepared_envelope

            def fail_result_commit(prepared, entries):
                if prepared.request_id.endswith("-result"):
                    raise OSError("result spool unavailable")
                return original_commit(prepared, entries)

            journal.commit_prepared_envelope = fail_result_commit
            client = APPClient(journal, network_client=network)
            public_future = client.async_distributed_inference(
                "/svc", b"request", deployment_revision="revision",
                timeout_ms=1000)
            wire_result = SimpleNamespace(
                status=True, payload=b"response", error="",
                request_id=submitted["request_id"])

            submitted["on_result"](wire_result)
            native_future.set_result(wire_result)
            result = public_future.result(timeout=1)

            self.assertFalse(result.status)
            self.assertEqual(result.error, "RESULT_PERSISTENCE_FAILED")
            handle = client.open_request(result.request_id)
            self.assertEqual(client.status(handle), RequestState.FAILED)

    def test_future_distributed_inference_uses_the_same_durable_identity(self):
        with tempfile.TemporaryDirectory() as root:
            network = SimpleNamespace(encode_input=lambda service, value: bytes(value))

            def submit_network(service, payload, **kwargs):
                result = SimpleNamespace(
                    status=True, payload=b"future-response", error="",
                    request_id=kwargs["request_id"])
                kwargs["on_result"](result)
                future = Future(); future.set_result(result)
                return future

            network.async_distributed_inference = mock.Mock(
                side_effect=submit_network)
            client = APPClient(
                RuntimeJournal(root, "alice"), network_client=network)
            result = client.async_distributed_inference(
                "/svc", b"request", deployment_revision="revision",
                timeout_ms=1000).result(timeout=1)
            self.assertEqual(result.payload, b"future-response")
            self.assertEqual(
                result.request_id,
                network.async_distributed_inference.call_args.kwargs["request_id"])

    def test_plan_infer_delegates_to_durable_submit_identity(self):
        with tempfile.TemporaryDirectory() as root:
            network = SimpleNamespace(encode_input=lambda service, value: bytes(value))

            def infer_network(plan, payload, **kwargs):
                result = SimpleNamespace(
                    status=True, payload=b"plan-response", error="",
                    request_id=kwargs["request_id"])
                kwargs["on_result"](result)
                future = Future(); future.set_result(result)
                return future

            network.infer_async = mock.Mock(side_effect=infer_network)
            client = APPClient(
                RuntimeJournal(root, "alice"), network_client=network)
            plan = SimpleNamespace(service="/svc")
            result = client.infer(
                plan, b"request", deployment_revision="revision", timeout_ms=1000)
            self.assertEqual(result.payload, b"plan-response")
            self.assertEqual(
                result.request_id, network.infer_async.call_args.kwargs["request_id"])

    def test_request_hot_path_does_not_rescan_all_journal_records(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal(root, "alice")
            with mock.patch.object(
                    journal, "records", wraps=journal.records) as records:
                client = APPClient(journal, executor=lambda payload: payload)
                initial_scans = records.call_count
                for _ in range(20):
                    handle = client.submit("d", "r", b"payload")
                    self.assertEqual(client.result(handle), b"payload")
                self.assertEqual(records.call_count, initial_scans)


if __name__=="__main__": unittest.main()
