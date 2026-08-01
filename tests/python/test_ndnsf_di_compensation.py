from __future__ import annotations

from dataclasses import replace
import hashlib
import json
import unittest

from ndnsf_distributed_inference.core.recovery import (
    AdoptedInputEvidence,
    AttemptCompensationController,
    DICancelAttemptV2,
    DIReleaseOfferV2,
    DIStatusQueryV2,
)
from ndnsf_distributed_inference.app_sdk.placement import (
    replan_placement_request,
)
from ndnsf_distributed_inference.sdk.placement import (
    CandidateBudget,
    PlacementRequest,
)


def digest(value: str) -> str:
    return "sha256:" + hashlib.sha256(value.encode()).hexdigest()


def adoption(**changes):
    values = dict(
        request_id="request-1", old_attempt=1, new_attempt=2,
        old_lineage_digest=digest("lineage"),
        new_lineage_digest=digest("lineage"),
        old_semantic_digest=digest("semantics"),
        new_semantic_digest=digest("semantics"),
        old_schema_digest=digest("schema"),
        new_schema_digest=digest("schema"),
        old_segment_contract_digest=digest("segments"),
        new_segment_contract_digest=digest("segments"),
        authorization_digest=digest("authorization"),
        consumer_role="sink", authorized_requester="/requester",
        captured_at_ms=100, expires_at_ms=900,
        signer_key_id="requester-key", signature="valid",
    )
    values.update(changes)
    return AdoptedInputEvidence(**values)


class CompensationV2Test(unittest.TestCase):
    def test_control_payloads_are_canonical_closed_and_independently_bound(self):
        controls = (
            DICancelAttemptV2(
                request_id="request-1", attempt=1, plan_digest=digest("p1"),
                requester="/requester", target_provider="/provider/a",
                reason_code="REPLAN", issued_at_ms=100, expires_at_ms=900,
                nonce="cancel-1", signer_key_id="requester-key",
                signature="valid"),
            DIReleaseOfferV2(
                request_id="request-1", attempt=1,
                offer_digest=digest("offer"),
                requester="/requester", target_provider="/provider/a",
                reason_code="NOT_SELECTED", issued_at_ms=100,
                expires_at_ms=900, nonce="release-1",
                signer_key_id="requester-key", signature="valid"),
            DIStatusQueryV2(
                request_id="request-1", attempt=1,
                transaction_id="txn-1", requester="/requester",
                target_provider="/provider/a", issued_at_ms=100,
                expires_at_ms=900, nonce="status-1",
                signer_key_id="requester-key", signature="valid"),
        )
        for value in controls:
            self.assertEqual(type(value).from_bytes(value.to_bytes()), value)
            value.validate_target(
                now_ms=200, requester="/requester",
                target_provider="/provider/a", attempt_deadline_ms=900,
                verify_signature=lambda item: item.signature == "valid")
            with self.assertRaises(PermissionError):
                value.validate_target(
                    now_ms=200, requester="/attacker",
                    target_provider="/provider/a", attempt_deadline_ms=900,
                    verify_signature=lambda item: True)
            changed = json.loads(value.to_bytes())
            changed["roles"] = ["attacker-assigned-role"]
            with self.assertRaises(ValueError):
                type(value).from_bytes(json.dumps(
                    changed, sort_keys=True, separators=(",", ":")).encode())

    def test_replan_increments_attempt_and_requires_fresh_plan_and_token(self):
        controller = AttemptCompensationController(
            request_id="request-1", requester="/requester",
            authorization_digest=digest("authorization"),
            deadline_ms=1000, verify_signature=lambda item:
            item.signature == "valid")
        controller.begin(
            plan_digest=digest("plan-1"), token_digest=digest("token-1"),
            providers={"/provider/a": digest("offer-a")})
        transition = controller.replan(
            at_ms=200, new_plan_digest=digest("plan-2"),
            new_token_digest=digest("token-2"),
            providers={"/provider/b": digest("offer-b")},
            required_inputs=(("sink", digest("lineage")),),
            adopted_inputs=(adoption(),), fallback_complete=False)
        self.assertEqual(transition.attempt, 2)
        self.assertEqual(transition.adopted_input_digests,
                         (adoption().digest(),))
        with self.assertRaises(ValueError):
            controller.replan(
                at_ms=300, new_plan_digest=digest("plan-2"),
                new_token_digest=digest("token-3"),
                providers={"/provider/c": digest("offer-c")},
                required_inputs=(), adopted_inputs=(),
                fallback_complete=True)

    def test_cross_attempt_reuse_requires_exact_safe_adoption(self):
        controller = AttemptCompensationController(
            request_id="request-1", requester="/requester",
            authorization_digest=digest("authorization"),
            deadline_ms=1000, verify_signature=lambda item:
            item.signature == "valid")
        controller.begin(
            plan_digest=digest("plan-1"), token_digest=digest("token-1"),
            providers={"/provider/a": digest("offer-a")})
        with self.assertRaises(ValueError):
            controller.replan(
                at_ms=200, new_plan_digest=digest("plan-2"),
                new_token_digest=digest("token-2"),
                providers={"/provider/b": digest("offer-b")},
                required_inputs=(("sink", digest("lineage")),),
                adopted_inputs=(), fallback_complete=False)
        self.assertEqual(controller.terminal_outcome, "ABORTED")
        controller = AttemptCompensationController(
            request_id="request-1", requester="/requester",
            authorization_digest=digest("authorization"),
            deadline_ms=1000, verify_signature=lambda item:
            item.signature == "valid")
        controller.begin(
            plan_digest=digest("plan-1"), token_digest=digest("token-1"),
            providers={"/provider/a": digest("offer-a")})
        with self.assertRaises(ValueError):
            controller.replan(
                at_ms=200, new_plan_digest=digest("plan-2"),
                new_token_digest=digest("token-2"),
                providers={"/provider/b": digest("offer-b")},
                required_inputs=(("sink", digest("lineage")),),
                adopted_inputs=(adoption(
                    new_schema_digest=digest("different")),),
                fallback_complete=False)

    def test_compensation_is_idempotent_and_converges_after_partition(self):
        controller = AttemptCompensationController(
            request_id="request-1", requester="/requester",
            authorization_digest=digest("authorization"),
            deadline_ms=1000, verify_signature=lambda item: True)
        controller.begin(
            plan_digest=digest("plan-1"), token_digest=digest("token-1"),
            providers={"/provider/a": digest("offer-a")})
        controller.replan(
            at_ms=200, new_plan_digest=digest("plan-2"),
            new_token_digest=digest("token-2"),
            providers={"/provider/b": digest("offer-b")},
            required_inputs=(), adopted_inputs=(), fallback_complete=True)
        self.assertFalse(controller.accept_event(attempt=1, at_ms=201))
        self.assertTrue(controller.accept_event(attempt=2, at_ms=201))
        seen = []
        first = controller.dispatch_pending(
            at_ms=300, sender=lambda payload: (
                seen.append(payload.digest()) or False))
        self.assertEqual(first.pending, 2)
        second = controller.dispatch_pending(
            at_ms=400, sender=lambda payload: True)
        self.assertEqual(second.pending, 0)
        third = controller.dispatch_pending(
            at_ms=500, sender=lambda payload: True)
        self.assertEqual(third.sent, 0)

    def test_authenticated_status_query_retries_without_new_authority(self):
        controller = AttemptCompensationController(
            request_id="request-1", requester="/requester",
            authorization_digest=digest("authorization"),
            deadline_ms=1000, verify_signature=lambda item: True)
        controller.begin(
            plan_digest=digest("plan-1"), token_digest=digest("token-1"),
            providers={"/provider/a": digest("offer-a")})
        query = controller.enqueue_status_query(
            provider="/provider/a", transaction_id="txn-1", at_ms=100)
        self.assertIsInstance(query, DIStatusQueryV2)
        self.assertEqual(controller.dispatch_pending(
            at_ms=200, sender=lambda _payload: False).pending, 1)
        self.assertEqual(controller.dispatch_pending(
            at_ms=300, sender=lambda _payload: True).pending, 0)

    def test_deadline_and_cancel_response_race_are_first_terminal_wins(self):
        controller = AttemptCompensationController(
            request_id="request-1", requester="/requester",
            authorization_digest=digest("authorization"),
            deadline_ms=1000, verify_signature=lambda item: True)
        controller.begin(
            plan_digest=digest("plan-1"), token_digest=digest("token-1"),
            providers={"/provider/a": digest("offer-a")})
        self.assertTrue(controller.accept_response(attempt=1, at_ms=900))
        self.assertFalse(controller.cancel(at_ms=901, reason="too-late"))
        self.assertFalse(controller.accept_event(attempt=1, at_ms=902))
        expired = AttemptCompensationController(
            request_id="request-2", requester="/requester",
            authorization_digest=digest("authorization"),
            deadline_ms=1000, verify_signature=lambda item: True)
        expired.begin(
            plan_digest=digest("plan-1"), token_digest=digest("token-1"),
            providers={"/provider/a": digest("offer-a")})
        self.assertTrue(expired.expire(at_ms=1000))
        self.assertFalse(expired.accept_response(attempt=1, at_ms=1000))

    def test_replan_placement_request_fences_old_attempt(self):
        placement = PlacementRequest(
            request_id="request-1", attempt=1, deadline_ms=1000,
            model_digest=digest("model"), graph_digest=digest("graph"),
            candidate_ids=(digest("candidate"),), providers=(),
            required_roles=("source",), budget=CandidateBudget(4, 100),
        )
        replanned = replan_placement_request(
            placement, at_ms=200,
            candidate_ids=(digest("candidate-2"),), providers=())
        self.assertEqual(replanned.attempt, 2)
        self.assertEqual(replanned.deadline_ms, placement.deadline_ms)
        with self.assertRaises(TimeoutError):
            replan_placement_request(
                placement, at_ms=1000,
                candidate_ids=(digest("candidate-2"),), providers=())


if __name__ == "__main__":
    unittest.main()
