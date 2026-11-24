from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
import unittest


from ndnsf.service import CollaborationRole, ServiceUser


class _DeferredNative:
    def __init__(self):
        self.on_ack_closed = None
        self.ack_coverage_predicate = None
        self.commit_identity = None
        self.commit_count = 0
        self.begin_count = 0

    def begin_collaboration(
        self, service, payload, on_ack_closed, on_response, on_timeout,
        ack_timeout_ms, timeout_ms, request_id, **kwargs,
    ):
        self.begin_count += 1
        self.on_ack_closed = on_ack_closed
        self.on_response = on_response
        self.on_timeout = on_timeout
        self.ack_coverage_predicate = kwargs.get("ack_coverage_predicate")
        self.service = service
        self.payload = payload
        return request_id or "/request/deferred-1"

    def close_acks(self, *, digest="sha256:" + "a" * 64):
        candidate = SimpleNamespace(
            provider_name="/provider/a",
            service_name="/generic/work",
            request_id="/request/deferred-1",
            status=True,
            message="willing",
            payload=b"role=worker;",
            telemetry={"rtt_ms": 2.0},
        )
        self.on_ack_closed(SimpleNamespace(
            request_id="/request/deferred-1",
            candidates=[candidate],
            digest=digest,
            closed_at_us=1_000,
            request_deadline_us=20_000,
        ))

    def commit_collaboration_plan(self, *args):
        identity = repr(args)
        if self.commit_identity is None:
            self.commit_identity = identity
            self.commit_count += 1
            return True
        if identity == self.commit_identity:
            return True
        raise RuntimeError("conflicting second collaboration plan commit")


class AutomaticCollaborationPlanTest(unittest.TestCase):
    def setUp(self):
        self.native = _DeferredNative()
        self.user = object.__new__(ServiceUser)
        self.user._native = self.native
        self.events = []


    def test_non_di_deferred_request_closes_once_then_commits_idempotently(self):
        invocation = self.user.begin_collaboration(
            "/generic/work",
            b"opaque application payload",
            mode="DEFERRED",
            ack_timeout_ms=100,
            timeout_ms=1000,
            request_id="/request/deferred-1",
        )
        with self.assertRaisesRegex(TimeoutError, "ACK_CLOSED"):
            invocation.acks_closed(timeout_ms=1)

        self.native.close_acks()
        closed = invocation.acks_closed()
        self.assertEqual(len(closed.candidates), 1)
        self.assertEqual(closed.candidates[0].payload, b"role=worker;")
        with self.assertRaises(TypeError):
            closed.candidates[0].telemetry["rtt_ms"] = 9.0

        commit = dict(
            ack_closed_digest=closed.digest,
            roles=[CollaborationRole("worker", service="/generic/work")],
            key_scopes={},
            role_provider_assignments={"worker": "/provider/a"},
            assignment_payloads_by_role={"worker": b"opaque-plan-bytes"},
        )
        self.assertTrue(invocation.commit_plan(**commit))
        self.assertTrue(invocation.commit_plan(**commit))
        self.assertEqual(self.native.begin_count, 1)
        self.assertEqual(self.native.commit_count, 1)

        with self.assertRaisesRegex(ValueError, "ACK_CLOSED"):
            invocation.commit_plan(**{
                **commit,
                "ack_closed_digest": "sha256:" + "b" * 64,
            })
        with self.assertRaisesRegex(RuntimeError, "conflicting"):
            invocation.commit_plan(**{
                **commit,
                "roles": [
                    CollaborationRole("other", service="/generic/work"),
                ],
            })
        with self.assertRaisesRegex(RuntimeError, "conflicting ACK_CLOSED"):
            self.native.close_acks(digest="sha256:" + "9" * 64)

    def test_deferred_request_accepts_application_ack_coverage_hook(self):
        observed = []
        invocation = self.user.begin_collaboration(
            "/generic/work",
            b"opaque application payload",
            mode="DEFERRED",
            ack_timeout_ms=100,
            timeout_ms=1000,
            request_id="/request/deferred-coverage-1",
            ack_coverage_predicate=lambda candidates: (
                observed.append(tuple(candidates)) or bool(candidates)),
        )
        self.assertEqual(invocation.request_id, "/request/deferred-coverage-1")
        candidate = SimpleNamespace(
            provider_name="/provider/a",
            service_name="/generic/work",
            request_id="/request/deferred-coverage-1",
            status=True,
            message="willing",
            payload=b"role=worker;",
            telemetry=None,
        )
        self.assertTrue(self.native.ack_coverage_predicate([candidate]))
        self.assertEqual(len(observed), 1)


    def test_preplanned_api_and_core_schema_remain_generic(self):
        root = Path(__file__).resolve().parents[2]
        header = (
            root / "ndn-service-framework" / "ServiceUser.hpp"
        ).read_text()
        source = (
            root / "ndn-service-framework" / "ServiceUser.cpp"
        ).read_text()
        self.assertIn("BeginCollaboration", header)
        self.assertIn("CommitCollaborationPlan", header)
        self.assertIn("RequestCollaboration", header)
        deferred_region = source[
            source.index("ndn::Name ServiceUser::BeginCollaboration("):
            source.index("bool ServiceUser::CommitCollaborationPlan(") + 6000
        ]
        for forbidden in ("Qwen", "logits", "KV cache", "model fragment"):
            self.assertNotIn(forbidden, deferred_region)


if __name__ == "__main__":
    unittest.main()
