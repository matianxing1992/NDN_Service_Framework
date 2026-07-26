from __future__ import annotations

import unittest

from ndnsf_distributed_inference.core import (
    AtomicReservationBook, DeploymentIntent, PreparationCallbacks,
    ProviderCapabilityOffer, SelectionGatedProvider,
)
from ndnsf_distributed_inference.provider import DistributedInferenceProvider


class ReservationBookTest(unittest.TestCase):
    def make_book(self) -> AtomicReservationBook:
        return AtomicReservationBook(
            "/provider/a", "boot-1", capacity=4,
            per_requester_limit=3, per_service_limit=4,
            max_lease_ms=100, committed_lease_ms=200)

    def test_authorization_precedes_reservation_and_duplicate_does_not_extend(self):
        book = self.make_book()
        with self.assertRaises(PermissionError):
            book.reserve(requester="/u", service="/s", request_id="r", attempt=1,
                         units=1, now_ms=10, requested_lease_ms=80,
                         authorized=False, signature="sig")
        self.assertEqual(book.live_units(now_ms=10), 0)
        first = book.reserve(requester="/u", service="/s", request_id="r", attempt=1,
                             units=1, now_ms=10, requested_lease_ms=80,
                             authorized=True, signature="sig")
        duplicate = book.reserve(requester="/u", service="/s", request_id="r", attempt=1,
                                 units=1, now_ms=20, requested_lease_ms=100,
                                 authorized=True, signature="sig")
        self.assertEqual(first, duplicate)
        self.assertEqual(first.fields["expiresAtMs"], "90")
        self.assertEqual(book.live_units(now_ms=20), 1)

    def test_quota_expiry_release_and_no_resurrection(self):
        book = self.make_book()
        lease = book.reserve(requester="/u", service="/s", request_id="r", attempt=1,
                             units=3, now_ms=0, requested_lease_ms=50,
                             authorized=True, signature="sig")
        with self.assertRaisesRegex(RuntimeError, "quota"):
            book.reserve(requester="/u", service="/s", request_id="r2", attempt=1,
                         units=1, now_ms=1, requested_lease_ms=50,
                         authorized=True, signature="sig")
        self.assertEqual(book.expire(now_ms=50), 1)
        self.assertEqual(book.live_units(now_ms=50), 0)
        with self.assertRaisesRegex(RuntimeError, "expired"):
            book.commit(lease.fields["reservationId"], now_ms=50)

    def test_commit_creates_bounded_execution_lease_and_release_is_idempotent(self):
        book = self.make_book()
        lease = book.reserve(requester="/u", service="/s", request_id="r", attempt=1,
                             units=1, now_ms=10, requested_lease_ms=100,
                             authorized=True, signature="sig")
        item = book.commit(lease.fields["reservationId"], now_ms=20)
        self.assertEqual(item.committed_expires_at_ms, 220)
        self.assertTrue(book.release(item.reservation_id, reason="NOT_SELECTED"))
        self.assertFalse(book.release(item.reservation_id, reason="NOT_SELECTED"))
        self.assertEqual(book.live_units(now_ms=21), 0)

    def test_provider_ack_reservation_has_no_prepare_side_effect(self):
        book = self.make_book()
        events = []
        provider = SelectionGatedProvider(
            "/provider/a", "boot-1",
            lambda _intent: ProviderCapabilityOffer(
                "/provider/a", "boot-1", (1,), False, ("native",), ("gpu",),
                ("fp16",), ("primary",), 1, 0, 0, 100),
            PreparationCallbacks(
                lambda *_: events.append("verify"),
                lambda *_: events.append("load"),
                lambda *_: events.append("warm")),
            lambda _message: True,
            reservation_book=book,
            reservation_authorizer=lambda intent: intent.requester_identity == "/u")
        intent = DeploymentIntent(
            "/u", "r", 1, "/artifact", "sha256:a", ("v",), ("primary",), 500)
        lease = provider.acknowledge_reservation(
            intent, service="/s", units=1, lease_ms=100, now_ms=10,
            signature="provider-signature")
        self.assertEqual(lease.fields["reservationId"],
                         provider.acknowledge_reservation(
                             intent, service="/s", units=1, lease_ms=100,
                             now_ms=20, signature="provider-signature").fields["reservationId"])
        self.assertEqual(events, [])
        self.assertEqual(provider.counters.verify, 0)
        self.assertEqual(provider.counters.load, 0)
        self.assertEqual(provider.counters.warm, 0)

    def test_restart_reclaims_expired_and_shutdown_releases_live(self):
        first = self.make_book()
        first.reserve(requester="/u", service="/s", request_id="r", attempt=1,
                      units=1, now_ms=0, requested_lease_ms=40,
                      authorized=True, signature="sig")
        snapshot = first.snapshot()
        restored = self.make_book()
        self.assertEqual(restored.restore(snapshot, now_ms=40), 1)
        self.assertEqual(restored.live_units(now_ms=40), 0)
        restored.reserve(requester="/u", service="/s", request_id="r", attempt=2,
                         units=1, now_ms=41, requested_lease_ms=40,
                         authorized=True, signature="sig")
        self.assertEqual(restored.shutdown(), 1)
        self.assertEqual(restored.release_counters["PROVIDER_SHUTDOWN"], 1)

    def test_high_level_ack_reserves_only_when_capability_is_negotiated(self):
        captured = {}

        class NetworkProvider:
            provider = "/provider/a"

            def add_collaboration_handler(self, service, roles, handler,
                                          ack_handler, **options):
                captured.update(ack=ack_handler, options=options)

            def set_r1_selection_decision_handler(self, service, handler):
                captured.update(decision_service=service,
                                decision_handler=handler)

            def set_r1_reservation_terminal_handler(self, service, handler):
                captured.update(terminal_service=service,
                                terminal_handler=handler)

        book = self.make_book()
        provider = DistributedInferenceProvider(NetworkProvider())
        provider.add_capability_handler(
            "/s", ["primary"], lambda _ctx: None,
            has_model=True, reservation_book=book,
            reservation_authorizer=lambda context:
                context["deployment_intent"].get("requesterIdentity") == "/u")
        self.assertTrue(captured["options"]["include_ack_context"])

        ordinary = captured["ack"]({
            "request_capabilities": {}, "deployment_intent": {}}, b"opaque")
        self.assertTrue(ordinary.status)
        self.assertEqual(dict(ordinary.reservation_lease), {})
        self.assertEqual(book.live_units(now_ms=0), 0)

        context = {
            "request_capabilities": {"DIReservationSelectionV1": "required"},
            "deployment_intent": {
                "requesterIdentity": "/u", "requestId": "r-network",
                "attempt": "1"},
        }
        reserved = captured["ack"](context, b"ciphertext")
        self.assertTrue(reserved.status)
        self.assertTrue(reserved.reservation_lease["reservationId"])
        receipt = captured["decision_handler"]({
            "decision": "SELECTED",
            "reservationId": reserved.reservation_lease["reservationId"],
        })
        self.assertEqual(receipt["state"], "COMMITTED")
        self.assertEqual(captured["decision_service"], "/s")
        self.assertEqual(captured["terminal_service"], "/s")
        self.assertEqual(book.live_units(now_ms=1), 1)
        captured["terminal_handler"](
            reserved.reservation_lease["reservationId"], "LOCAL_COMPLETE")
        self.assertEqual(book.live_units(now_ms=1), 0)
        self.assertEqual(book.release_counters["LOCAL_COMPLETE"], 1)
        captured["terminal_handler"](
            reserved.reservation_lease["reservationId"], "LOCAL_COMPLETE")
        self.assertEqual(book.release_counters["LOCAL_COMPLETE"], 1)

        denied = dict(context)
        denied["deployment_intent"] = {
            "requesterIdentity": "/denied", "requestId": "r-denied",
            "attempt": "1"}
        rejected = captured["ack"](denied, b"ciphertext")
        self.assertFalse(rejected.status)
        self.assertEqual(dict(rejected.reservation_lease), {})

    def test_spec130_gate_is_di_only_and_precedes_positive_ack_reservation(self):
        captured = {}

        class NetworkProvider:
            provider = "/provider/a"
            def add_collaboration_handler(self, service, roles, handler,
                                          ack_handler, **options):
                captured["ack"] = ack_handler
            def set_r1_selection_decision_handler(self, service, handler): pass
            def set_r1_reservation_terminal_handler(self, service, handler): pass

        book = self.make_book(); gate_calls = []
        provider = DistributedInferenceProvider(NetworkProvider())
        provider.add_capability_handler(
            "/s", ["primary"], lambda _ctx: None, has_model=True,
            reservation_book=book,
            reservation_authorizer=lambda _context: True,
            conflict_admission_gate=lambda context: gate_calls.append(context) or {
                "admitted": True, "canonicalResourceId": "resource-digest",
                "resourceSequence": 7, "permitId": "permit-1",
                "authorityEpoch": 3, "authorityDigest": "authority-digest"})
        base = {"deployment_intent": {"requesterIdentity": "/u",
                                      "requestId": "r", "attempt": "1"}}
        ordinary = captured["ack"]({**base, "request_capabilities": {}}, b"x")
        self.assertTrue(ordinary.status)
        self.assertEqual(gate_calls, [])
        self.assertEqual(dict(ordinary.reservation_lease), {})

        di = captured["ack"]({**base, "request_capabilities": {
            "DIReservationSelectionV1": "required",
            "DIConflictAdmissionV1": "required"}}, b"ciphertext")
        self.assertTrue(di.status)
        self.assertEqual(len(gate_calls), 1)
        self.assertEqual(di.reservation_lease["canonicalResourceId"],
                         "resource-digest")
        self.assertEqual(di.reservation_lease["conflictPermitId"], "permit-1")
        binding = book.reservation_binding(
            di.reservation_lease["reservationId"], now_ms=0)
        self.assertEqual(binding["resource_sequence"], 7)

        denied_book = self.make_book(); denied = {}
        denied_provider = DistributedInferenceProvider(NetworkProvider())
        # Rebind capture for this provider and prove the gate rejects before reserve.
        denied_provider.add_capability_handler(
            "/s", ["primary"], lambda _ctx: None, has_model=True,
            reservation_book=denied_book,
            reservation_authorizer=lambda _context: True,
            conflict_admission_gate=lambda _context: False)
        rejected = captured["ack"]({**base, "request_capabilities": {
            "DIReservationSelectionV1": "required",
            "DIConflictAdmissionV1": "required"}}, b"ciphertext")
        self.assertFalse(rejected.status)
        self.assertEqual(denied_book.live_units(now_ms=0), 0)

        required_book = self.make_book()
        required_provider = DistributedInferenceProvider(NetworkProvider())
        required_provider.add_capability_handler(
            "/s", ["primary"], lambda _ctx: None, has_model=True,
            reservation_book=required_book,
            reservation_authorizer=lambda _context: True,
            conflict_admission_gate=lambda _context: True,
            require_conflict_admission=True,
            reservation_resource_id="resource-digest",
            reservation_resource_sequence=1)
        missing_capability = captured["ack"]({**base, "request_capabilities": {
            "DIReservationSelectionV1": "required"}}, b"ciphertext")
        self.assertFalse(missing_capability.status)
        self.assertEqual(missing_capability.message,
                         "DI_CONFLICT_ADMISSION_REQUIRED")
        self.assertEqual(required_book.live_units(now_ms=0), 0)


if __name__ == "__main__":
    unittest.main()
