#!/usr/bin/env python3

from copy import deepcopy
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))

from ndnsf_distributed_inference.core import (
    AtomicReservationBook, AuthorityEpoch, CanonicalResourceKey, ConflictAdmissionCoordinator,
    RequestAttempt, ResourceClaim, ResourceDeclaration,
    issue_permit_envelope, verify_permit_envelope,
)
from ndnsf_distributed_inference.deployment import JournaledConflictAdmissionCoordinator


def key(provider="/p1", boot="boot-1", rid="gpu0", domain="gpu"):
    return CanonicalResourceKey(provider, boot, "accelerator", rid, domain,
                                "slot", "v1")


def declaration(resource, capacity=1, exclusive=True, sequence=1):
    return ResourceDeclaration(resource, capacity, exclusive, sequence)


def request(name, claims, deadline=1000, attempt=1, capability="DIConflictAdmissionV1"):
    return RequestAttempt(f"/{name}", name, attempt, deadline, tuple(claims),
                          capability=capability)


def binding(req, resource, reservation="r1", quantity=1):
    return {resource.stable_id: {
        "request_identity": req.identity,
        "provider_boot_epoch": resource.provider_boot_epoch,
        "reservation_id": reservation,
        "quantity": quantity,
        "live": True,
    }}


class Spec130ConflictCoordinationTest(unittest.TestCase):
    def setUp(self):
        self.authority = AuthorityEpoch("/di/conflicts", "/controller", 1, "cboot-1")

    def coordinator(self, resources):
        value = ConflictAdmissionCoordinator(self.authority, max_permit_ms=100)
        value.register_declarations([declaration(item) for item in resources])
        return value

    def test_disjoint_requests_grant_together_and_overlap_serializes(self):
        a, b = key(rid="gpu0"), key(rid="gpu1")
        coord = self.coordinator((a, b))
        r1 = request("r1", (ResourceClaim(a),))
        r2 = request("r2", (ResourceClaim(b),))
        coord.submit(r1, now=1); coord.submit(r2, now=1)
        permits = coord.grant_next(self.authority, now=2, permit_ms=50)
        self.assertEqual({p.request.identity for p in permits}, {r1.identity, r2.identity})

        coord.release(permits[0].permit_id, now=3, reason="DONE")
        coord.release(permits[1].permit_id, now=3, reason="DONE")
        same1 = request("same1", (ResourceClaim(a),))
        same2 = request("same2", (ResourceClaim(a),))
        coord.submit(same1, now=4); coord.submit(same2, now=4)
        first = coord.grant_next(self.authority, now=5, permit_ms=50)
        self.assertEqual(len(first), 1)
        self.assertEqual(first[0].request.identity, same1.identity)
        self.assertEqual(coord.grant_next(self.authority, now=6, permit_ms=50), ())
        coord.release(first[0].permit_id, now=7, reason="DONE")
        self.assertEqual(len(coord.grant_next(self.authority, now=8, permit_ms=50)), 1)

    def test_cycle_breaks_by_stable_whole_request_order(self):
        a, b, c = key(rid="a"), key(rid="b"), key(rid="c")
        coord = self.coordinator((a, b, c))
        requests = (
            request("r1", (ResourceClaim(a), ResourceClaim(b))),
            request("r2", (ResourceClaim(b), ResourceClaim(c))),
            request("r3", (ResourceClaim(c), ResourceClaim(a))),
        )
        for item in requests:
            coord.submit(item, now=1)
        permits = coord.grant_next(self.authority, now=2, permit_ms=50)
        self.assertEqual(len(permits), 1)
        self.assertEqual(permits[0].request.identity, requests[0].identity)

    def test_activation_requires_complete_attempt_bound_live_bindings(self):
        a, b = key(provider="/p1", rid="a"), key(provider="/p2", rid="b")
        coord = self.coordinator((a, b))
        req = request("r", (ResourceClaim(a), ResourceClaim(b)))
        coord.submit(req, now=1)
        permit = coord.grant_next(self.authority, now=2, permit_ms=50)[0]
        with self.assertRaisesRegex(RuntimeError, "partial"):
            coord.activate(permit.permit_id, binding(req, a), now=3)
        bindings = binding(req, a)
        bindings.update(binding(req, b, reservation="r2"))
        self.assertEqual(coord.activate(permit.permit_id, bindings, now=3).state, "ACTIVE")
        coord.release(permit.permit_id, now=4, reason="DONE")
        coord.assert_safe()

    def test_provider_declaration_rejects_weakening_alias_and_stale_update(self):
        a = key()
        coord = self.coordinator((a,))
        weak = request("weak", (ResourceClaim(a, exclusive=False),))
        with self.assertRaisesRegex(RuntimeError, "weaken"):
            coord.submit(weak, now=1)
        alias = CanonicalResourceKey(a.provider_identity, a.provider_boot_epoch,
                                     a.resource_class, a.resource_id,
                                     a.exclusivity_domain, a.capacity_unit, "v2")
        with self.assertRaisesRegex(ValueError, "aliased"):
            coord.register_declarations([declaration(alias, sequence=2)])
        changed = ResourceDeclaration(a, 2, True, 1)
        with self.assertRaisesRegex(RuntimeError, "stale"):
            coord.register_declarations([changed])

    def test_capability_authority_and_cleanup_only_fail_closed(self):
        a = key(); coord = self.coordinator((a,))
        with self.assertRaisesRegex(RuntimeError, "capability"):
            coord.submit(request("old", (ResourceClaim(a),), capability="legacy"), now=1)
        good = request("good", (ResourceClaim(a),)); coord.submit(good, now=1)
        stale = AuthorityEpoch(self.authority.scope, self.authority.authority_identity,
                               2, "other")
        with self.assertRaisesRegex(RuntimeError, "authority"):
            coord.grant_next(stale, now=2, permit_ms=10)
        coord.set_available(False, cleanup_only=True, now=2)
        with self.assertRaisesRegex(RuntimeError, "unavailable"):
            coord.grant_next(self.authority, now=3, permit_ms=10)

    def test_equal_expiry_is_half_open_and_cannot_resurrect(self):
        a = key(); coord = self.coordinator((a,))
        req = request("r", (ResourceClaim(a),), deadline=20)
        coord.submit(req, now=1)
        permit = coord.grant_next(self.authority, now=2, permit_ms=8)[0]
        coord.activate(permit.permit_id, binding(req, a), now=3)
        self.assertEqual(coord.expire(now=10), 1)
        self.assertEqual(permit.state, "RELEASED")
        with self.assertRaisesRegex(RuntimeError, "not activatable"):
            coord.activate(permit.permit_id, binding(req, a), now=10)
        interval = coord.ownership_intervals()[0]
        self.assertEqual((interval["start"], interval["end"]), (3, 10))

    def test_snapshot_restore_rejects_gap_tamper_and_preserves_idempotency(self):
        a = key(); coord = self.coordinator((a,))
        req = request("r", (ResourceClaim(a),)); coord.submit(req, now=1)
        coord.grant_next(self.authority, now=2, permit_ms=10)
        restored = ConflictAdmissionCoordinator.restore(coord.snapshot())
        self.assertEqual(restored.submit(req, now=3), "GRANTED")
        broken = deepcopy(coord.snapshot())
        broken["events"][1]["sequence"] = 9
        with self.assertRaisesRegex(RuntimeError, "gap"):
            ConflictAdmissionCoordinator.restore(broken)
        tampered = deepcopy(coord.snapshot())
        tampered["events"][0]["subject_digest"] = "bad"
        with self.assertRaisesRegex(RuntimeError, "digest"):
            ConflictAdmissionCoordinator.restore(tampered)

    def test_authority_rotation_requires_drain_and_fences_old_epoch(self):
        a = key(); coord = self.coordinator((a,))
        req = request("r", (ResourceClaim(a),)); coord.submit(req, now=1)
        permit = coord.grant_next(self.authority, now=2, permit_ms=10)[0]
        newer = AuthorityEpoch(self.authority.scope, "/controller", 2, "cboot-2")
        with self.assertRaisesRegex(RuntimeError, "live"):
            coord.rotate_authority(newer, now=3)
        coord.release(permit.permit_id, now=4, reason="DRAIN")
        coord.rotate_authority(newer, now=5)
        with self.assertRaisesRegex(RuntimeError, "authority"):
            coord.grant_next(self.authority, now=6, permit_ms=10)

    def test_atomic_reservation_book_supplies_attempt_bound_resource_binding(self):
        resource = key()
        book = AtomicReservationBook("/p1", "boot-1", capacity=1,
                                     per_requester_limit=1, per_service_limit=1,
                                     max_lease_ms=50, committed_lease_ms=100)
        lease = book.reserve(requester="/r", service="/svc", request_id="r",
                             attempt=1, units=1, now_ms=1, requested_lease_ms=20,
                             authorized=True, signature="sig",
                             canonical_resource_id=resource.stable_id,
                             resource_sequence=1)
        reservation_id = lease.fields["reservationId"]
        value = book.reservation_binding(reservation_id, now_ms=2)
        self.assertEqual(value["request_identity"], ("/r", "r", 1))
        self.assertEqual(value["canonical_resource_id"], resource.stable_id)
        req = request("r", (ResourceClaim(resource),))
        coord = self.coordinator((resource,)); coord.submit(req, now=1)
        permit = coord.grant_next(self.authority, now=2, permit_ms=20)[0]
        coord.activate(permit.permit_id, {resource.stable_id: value}, now=3)
        book.release(reservation_id, reason="BEFORE_RETRY")
        self.assertFalse(book.reservation_binding(reservation_id, now_ms=4)["live"])

    def test_journal_restore_fences_competing_and_live_replacement_authority(self):
        class Journal:
            def __init__(self): self.values = []
            def records(self): return list(self.values)
            def append(self, kind, payload):
                self.values.append({"kind": kind, "payload": deepcopy(payload)})

        resource = key(); journal = Journal()
        wrapped = JournaledConflictAdmissionCoordinator(self.coordinator((resource,)), journal)
        req = request("r", (ResourceClaim(resource),))
        wrapped.submit(req, now=1)
        permit = wrapped.grant_next(self.authority, now=2, permit_ms=10)[0]
        restored = JournaledConflictAdmissionCoordinator(
            ConflictAdmissionCoordinator(self.authority), journal)
        self.assertEqual(restored.permits[0].permit_id, permit.permit_id)
        competing = AuthorityEpoch(self.authority.scope, "/other", 1, "other-boot")
        with self.assertRaisesRegex(RuntimeError, "competing"):
            JournaledConflictAdmissionCoordinator(
                ConflictAdmissionCoordinator(competing), journal)
        newer = AuthorityEpoch(self.authority.scope, "/controller", 2, "cboot-2")
        with self.assertRaisesRegex(RuntimeError, "live"):
            JournaledConflictAdmissionCoordinator(
                ConflictAdmissionCoordinator(newer), journal)

    def test_conflict_evidence_contains_no_input_key_or_assignment_plaintext(self):
        resource = key(); coord = self.coordinator((resource,))
        req = request("opaque", (ResourceClaim(resource),))
        coord.submit(req, now=1); coord.grant_next(self.authority, now=2, permit_ms=10)
        encoded = str(coord.snapshot()).lower()
        for forbidden in ("inputkey", "symmetric_key", "assignmentplaintext",
                          "modelreference", "payload"):
            self.assertNotIn(forbidden, encoded)

    def test_provider_ledger_reconstructs_half_open_ownership_and_rejects_tamper(self):
        resource = key()
        book = AtomicReservationBook("/p1", "boot-1", capacity=1,
                                     per_requester_limit=1, per_service_limit=1,
                                     max_lease_ms=50, committed_lease_ms=50)
        lease = book.reserve(requester="/r", service="/s", request_id="r",
                             attempt=1, units=1, now_ms=10,
                             requested_lease_ms=20, authorized=True,
                             signature="sig", canonical_resource_id=resource.stable_id,
                             resource_sequence=1)
        rid = lease.fields["reservationId"]
        book.commit(rid, now_ms=12); book.release(rid, reason="DONE", now_ms=15)
        interval = book.ownership_intervals()[0]
        self.assertEqual((interval["start_ms"], interval["end_ms"]), (10, 15))
        self.assertTrue(interval["half_open"])
        snapshot = book.snapshot()
        restored = AtomicReservationBook("/p1", "boot-1", capacity=1,
                                         per_requester_limit=1, per_service_limit=1,
                                         max_lease_ms=50, committed_lease_ms=50)
        restored.restore(snapshot, now_ms=16)
        self.assertEqual(restored.ownership_intervals(), book.ownership_intervals())
        broken = deepcopy(snapshot); broken["ledger_events"][0]["digest"] = "bad"
        with self.assertRaisesRegex(ValueError, "ledger"):
            restored.restore(broken, now_ms=16)

    def test_signed_permit_is_request_epoch_target_and_expiry_bound(self):
        a, b = key(provider="/p1", rid="a"), key(provider="/p2", rid="b")
        coord = self.coordinator((a, b))
        req = request("r", (ResourceClaim(a), ResourceClaim(b)), deadline=100)
        coord.submit(req, now=10)
        permit = coord.grant_next(self.authority, now=11, permit_ms=20)[0]
        envelope = issue_permit_envelope(permit, b"test-key")
        result = verify_permit_envelope(
            envelope, b"test-key", expected_authority=self.authority,
            expected_request_identity=req.identity, expected_resource=a, now=12)
        self.assertEqual(result["permitId"], permit.permit_id)
        tampered = deepcopy(envelope); tampered["requestId"] = "other"
        with self.assertRaises(PermissionError):
            verify_permit_envelope(
                tampered, b"test-key", expected_authority=self.authority,
                expected_request_identity=req.identity, expected_resource=a, now=12)
        with self.assertRaisesRegex(RuntimeError, "target"):
            verify_permit_envelope(
                envelope, b"test-key", expected_authority=self.authority,
                expected_request_identity=req.identity,
                expected_resource=key(provider="/p3", rid="c"), now=12)
        with self.assertRaisesRegex(RuntimeError, "not live"):
            verify_permit_envelope(
                envelope, b"test-key", expected_authority=self.authority,
                expected_request_identity=req.identity, expected_resource=a, now=31)


if __name__ == "__main__":
    unittest.main()
