from __future__ import annotations

import unittest
from types import SimpleNamespace

from ndnsf.service import (
    CollaborationAckClosed,
    CollaborationInvocation,
    CollaborationRole,
    ServiceUser,
    _CollaborationInvocationState,
)


class _NativeCommitFixture:
    def __init__(self):
        self.value = None
        self.selection_count = 0
        self.status_snapshots = []

    def commit_collaboration_plan(self, *args):
        canonical = repr(args).encode()
        if self.value is None:
            self.value = canonical
            self.selection_count += 1
            return True
        return self.value == canonical

    def get_collaboration_status_snapshot(self, _request_id, _timeout_ms):
        return list(self.status_snapshots)


def invocation(native, closed=None, timeout_reason="", *, fail_fast=False):
    state = _CollaborationInvocationState()
    state.ack_closed = closed
    state.timeout_reason = timeout_reason
    return CollaborationInvocation(
        native=native, state=state, service="/opaque",
        request_id="request-1", ack_timeout_ms=10, timeout_ms=1,
        fail_fast_terminal_selection=fail_fast)


class DeferredCollaborationContractTest(unittest.TestCase):
    def test_python_deferred_collaboration_preserves_data_v1_key_offer(self):
        class Native:
            def begin_collaboration(
                    self, service, payload, on_ack_closed, on_response,
                    on_timeout, ack_timeout_ms, timeout_ms, request_id,
                    **kwargs):
                del service, payload, on_response, on_timeout
                del ack_timeout_ms, timeout_ms
                capabilities = kwargs.get("request_capabilities")
                self.request_capabilities = (
                    {} if capabilities is None else dict(capabilities.fields))
                on_ack_closed(SimpleNamespace(
                    request_id=request_id,
                    candidates=[SimpleNamespace(
                        provider_name="/provider/a",
                        service_name="/opaque",
                        request_id=request_id,
                        status=True,
                        message="ready",
                        payload=b"",
                        telemetry=None,
                        selection_input_key_offer={
                            "recipient": "/provider/a",
                            "recipientPublicKey": "aabb",
                            "ndnsfDataV1EndpointPrefix":
                                "/group/provider/a/provider/7",
                        },
                    )],
                    digest="sha256:" + "a" * 64,
                    closed_at_us=1_000,
                    request_deadline_us=20_000,
                ))
                return request_id

        native = Native()
        user = object.__new__(ServiceUser)
        user._native = native
        invocation = user.begin_collaboration(
            "/opaque", b"input", request_id="/request/data-v1",
            request_capabilities={"NDNSF_DATA_V1": "required"})
        closed = invocation.acks_closed(timeout_ms=10)

        self.assertEqual(
            native.request_capabilities, {"NDNSF_DATA_V1": "required"})
        self.assertEqual(len(closed.candidates), 1)
        self.assertEqual(
            closed.candidates[0].selection_input_key_offer[
                "ndnsfDataV1EndpointPrefix"],
            "/group/provider/a/provider/7")

    def test_byte_identical_commit_is_idempotent_and_conflict_has_no_selection(self):
        closed = CollaborationAckClosed(
            request_id="request-1",
            candidates=(), digest="sha256:" + "1" * 64,
            closed_at_us=100, request_deadline_us=1000)
        native = _NativeCommitFixture()
        handle = invocation(native, closed)
        kwargs = dict(
            ack_closed_digest=closed.digest,
            roles=[CollaborationRole(role="r", service="/opaque")],
            key_scopes={}, role_provider_assignments={"r": "/provider/a"},
            assignment_payloads_by_role={"r": b"opaque"})
        self.assertTrue(handle.commit_plan(**kwargs))
        self.assertTrue(handle.commit_plan(**kwargs))
        self.assertEqual(native.selection_count, 1)
        self.assertFalse(handle.commit_plan(
            **{**kwargs, "role_provider_assignments":
               {"r": "/provider/b"}}))
        self.assertEqual(native.selection_count, 1)

    def test_early_late_and_wrong_snapshot_commit_emit_zero_selection(self):
        native = _NativeCommitFixture()
        with self.assertRaises(TimeoutError):
            invocation(native).commit_plan(
                ack_closed_digest="sha256:" + "1" * 64, roles=[],
                key_scopes={})
        with self.assertRaises(TimeoutError):
            invocation(native, timeout_reason="late").commit_plan(
                ack_closed_digest="sha256:" + "1" * 64, roles=[],
                key_scopes={})
        closed = CollaborationAckClosed(
            request_id="request-1",
            candidates=(), digest="sha256:" + "1" * 64,
            closed_at_us=100, request_deadline_us=1000)
        with self.assertRaises(ValueError):
            invocation(native, closed).commit_plan(
                ack_closed_digest="sha256:" + "2" * 64, roles=[],
                key_scopes={})
        self.assertEqual(native.selection_count, 0)

    def test_terminal_selection_rejection_fails_without_full_request_timeout(self):
        native = _NativeCommitFixture()
        native.status_snapshots = [{
            "providerName": "/provider/a",
            "serviceName": "/opaque",
            "requestId": "request-1",
            "selectionDigest": "sha256:" + "1" * 64,
            "state": "Rejected",
            "message": "provider boot epoch mismatch",
        }]
        handle = invocation(native, timeout_reason="", fail_fast=True)
        with self.assertRaisesRegex(
                TimeoutError, "terminal rejection: provider boot epoch mismatch"):
            handle.result(timeout_ms=5000)

    def test_default_di_is_deferred_and_preplanned_is_compatibility_only(self):
        placement = (
            __import__(
                "ndnsf_distributed_inference.app_sdk.placement",
                fromlist=["AutomaticPlanningCoordinator"])
            .AutomaticPlanningCoordinator)
        source = __import__("inspect").getsource(placement.request)
        self.assertIn('mode="DEFERRED"', source)
        service_source = (
            __import__("inspect").getsource(
                __import__("ndnsf.service", fromlist=["ServiceUser"])
                .ServiceUser.begin_collaboration))
        self.assertIn("PREPLANNED", service_source)
        self.assertIn("DEFERRED", service_source)


if __name__ == "__main__":
    unittest.main()
