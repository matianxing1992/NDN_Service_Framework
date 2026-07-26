from __future__ import annotations

import hashlib
import fcntl
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from ndnsf_distributed_inference.app_sdk.runtime_journal import (
    FileRequestEnvelopeKeyProvider,
    RequestEnvelopeKey,
    RequestEnvelopeKeyRing,
    RuntimeJournal,
    RuntimeJournalKeyError,
    RuntimeJournalLockError,
    RuntimeJournalQuotaError,
    StaticRequestEnvelopeKeyProvider,
    RuntimeJournalUnsafeRootError,
    RuntimeJournalVersionError,
)
from ndnsf_distributed_inference.core import AtomicReservationBook
from ndnsf_distributed_inference.deployment import JournaledReservationBook


class RuntimeJournalTest(unittest.TestCase):
    def test_spec129_reservation_snapshot_recovers_and_expires_without_orphan(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "provider-a")
            first = AtomicReservationBook(
                "/provider/a", "boot-1", capacity=2,
                per_requester_limit=2, per_service_limit=2,
                max_lease_ms=100, committed_lease_ms=200)
            durable = JournaledReservationBook(first, journal, now_ms=0)
            durable.reserve(
                requester="/u", service="/s", request_id="r", attempt=1,
                units=1, now_ms=0, requested_lease_ms=50,
                authorized=True, signature="sig")

            restored = AtomicReservationBook(
                "/provider/a", "boot-1", capacity=2,
                per_requester_limit=2, per_service_limit=2,
                max_lease_ms=100, committed_lease_ms=200)
            recovered = JournaledReservationBook(restored, journal, now_ms=50)
            self.assertEqual(recovered.live_units(now_ms=50), 0)
            self.assertEqual(recovered.release_counters["LEASE_EXPIRED"], 1)
            self.assertTrue(any(
                record["kind"] == "spec129-reservation-book-v1"
                for record in journal.records()))

    def test_spec129_new_provider_boot_reclaims_old_epoch_without_resurrection(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "provider-a")
            old = JournaledReservationBook(AtomicReservationBook(
                "/provider/a", "boot-old", capacity=2,
                per_requester_limit=2, per_service_limit=2,
                max_lease_ms=100, committed_lease_ms=200), journal, now_ms=0)
            old.reserve(requester="/u", service="/s", request_id="r",
                        attempt=1, units=1, now_ms=0,
                        requested_lease_ms=100, authorized=True,
                        signature="sig")
            restarted = JournaledReservationBook(AtomicReservationBook(
                "/provider/a", "boot-new", capacity=2,
                per_requester_limit=2, per_service_limit=2,
                max_lease_ms=100, committed_lease_ms=200), journal, now_ms=10)
            self.assertEqual(restarted.live_units(now_ms=10), 0)
            self.assertEqual(restarted.release_counters["PROVIDER_RESTART"], 1)

    def test_file_key_provider_requires_owner_only_external_file(self):
        with tempfile.TemporaryDirectory(dir=Path.home()) as parent:
            root = Path(parent)
            key_path = root / "owner-envelope.key"
            key_path.write_bytes(b"f" * 32)
            os.chmod(key_path, 0o644)
            with self.assertRaisesRegex(RuntimeJournalKeyError, "owner-only"):
                FileRequestEnvelopeKeyProvider(key_path)

            os.chmod(key_path, 0o600)
            provider = FileRequestEnvelopeKeyProvider(key_path)
            journal = RuntimeJournal(
                root / "state",
                "alice",
                envelope_key_provider=provider,
            )
            journal.write_envelope(
                "request", b"payload", expires_at_ms=10_000)
            self.assertEqual(
                journal.read_envelope("request", at_ms=1), b"payload")
            self.assertFalse(
                (journal.root / "request-envelope.key").exists())

    def test_production_root_and_envelope_key_are_explicit(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(
                    RuntimeJournalUnsafeRootError, "volatile"):
                RuntimeJournal(
                    root,
                    "alice",
                    envelope_key_provider=StaticRequestEnvelopeKeyProvider(
                        RequestEnvelopeKey("active", b"a" * 32)),
                )

            journal = RuntimeJournal.for_test(root, "alice")
            self.assertFalse((journal.root / "request-envelope.key").exists())

        with tempfile.TemporaryDirectory(dir=Path.home()) as root:
            journal = RuntimeJournal(root, "alice")
            with self.assertRaisesRegex(RuntimeJournalKeyError, "key provider"):
                journal.write_envelope(
                    "request", b"payload", expires_at_ms=10_000)

    def test_wrong_and_rotated_owner_keys_fail_closed_or_reopen(self):
        old_key = RequestEnvelopeKey("old", b"o" * 32)
        new_key = RequestEnvelopeKey("new", b"n" * 32)
        old_provider = StaticRequestEnvelopeKeyProvider(old_key)
        rotated_provider = StaticRequestEnvelopeKeyProvider(
            new_key, previous=(old_key,))
        wrong_provider = StaticRequestEnvelopeKeyProvider(
            RequestEnvelopeKey("wrong", b"w" * 32))

        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal(
                root,
                "alice",
                envelope_key_provider=old_provider,
                test_only_allow_ephemeral_state_root=True,
            )
            journal.write_envelope(
                "before-rotation", b"old-payload", expires_at_ms=10_000)

            with self.assertRaisesRegex(ValueError, "key|integrity"):
                RuntimeJournal(
                    root,
                    "alice",
                    envelope_key_provider=wrong_provider,
                    test_only_allow_ephemeral_state_root=True,
                ).read_envelope("before-rotation", at_ms=1)

            restarted = RuntimeJournal(
                root,
                "alice",
                envelope_key_provider=rotated_provider,
                test_only_allow_ephemeral_state_root=True,
            )
            self.assertEqual(
                restarted.read_envelope("before-rotation", at_ms=1),
                b"old-payload",
            )
            restarted.write_envelope(
                "after-rotation", b"new-payload", expires_at_ms=10_000)
            body = json.loads(
                (restarted.spool / "after-rotation.json").read_text())
            self.assertEqual(body["keyId"], "new")

    def test_first_initialization_syncs_directories_without_persisting_key(self):
        with tempfile.TemporaryDirectory() as parent:
            root = Path(parent, "new-state-root")
            with mock.patch("os.fsync", wraps=os.fsync) as fsync:
                journal = RuntimeJournal.for_test(root, "alice")

            self.assertFalse((journal.root / "request-envelope.key").exists())
            # Newly created state-root, identity-root and child entries are
            # durable before use; owner key bytes remain outside the journal.
            self.assertGreaterEqual(fsync.call_count, 3)

    def test_invalid_injected_envelope_key_fails_closed(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaises(RuntimeJournalKeyError):
                RuntimeJournal(
                    root,
                    "alice",
                    envelope_key_provider=StaticRequestEnvelopeKeyProvider(
                        RequestEnvelopeKey("short", b"short")),
                    test_only_allow_ephemeral_state_root=True,
                )

    def test_owner_permissions_append_restart_and_torn_tail(self):
        with tempfile.TemporaryDirectory() as root:
            journal=RuntimeJournal.for_test(root,"alice"); journal.append("x",{"n":1})
            self.assertEqual(os.stat(journal.path).st_mode & 0o777,0o600)
            self.assertEqual(len(RuntimeJournal.for_test(root,"alice").records()),1)
            with journal.path.open("ab") as out: out.write(b'{"torn"')
            self.assertEqual(len(journal.records()),1)

    def test_identity_traversal_symlink_corruption_and_quota_fail(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaises(ValueError): RuntimeJournal.for_test(root,"../bob")
            journal=RuntimeJournal.for_test(root,"alice",quota_bytes=512)
            journal.path.write_text('{"checksum":"bad"}\n')
            with self.assertRaisesRegex(ValueError,"checksum"): journal.records()
            with self.assertRaises(RuntimeJournalQuotaError):
                journal.write_envelope("large", b"x" * 1024, expires_at_ms=2000)
        with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as target:
            Path(root,"alice").symlink_to(target)
            with self.assertRaisesRegex(ValueError,"symlink"): RuntimeJournal.for_test(root,"alice")

    def test_unsupported_schema_and_lock_contention_fail_typed(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "alice")
            record = {
                "schema": "ndnsf-di-app-runtime-journal-v999",
                "kind": "x",
                "timestampMs": 1,
                "payload": {},
            }
            body = json.dumps(record, sort_keys=True, separators=(",", ":"))
            record["checksum"] = hashlib.sha256(body.encode()).hexdigest()
            journal.path.write_text(json.dumps(record) + "\n")
            with self.assertRaises(RuntimeJournalVersionError):
                RuntimeJournal.for_test(root, "alice")

        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "alice")
            with journal.lock_path.open("rb") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with self.assertRaises(RuntimeJournalLockError):
                    journal.append("x", {"n": 1})

    def test_unsupported_schema_inside_atomic_transaction_fails_typed(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "alice")
            journal.append_many((("x", {"n": 1}), ("y", {"n": 2})))
            transaction = json.loads(journal.path.read_text())
            nested = transaction["payload"]["records"][0]
            nested["schema"] = "ndnsf-di-app-runtime-journal-v999"
            nested_body = dict(nested)
            nested_body.pop("checksum")
            nested["checksum"] = hashlib.sha256(json.dumps(
                nested_body, sort_keys=True, separators=(",", ":")
            ).encode()).hexdigest()
            outer_body = dict(transaction)
            outer_body.pop("checksum")
            transaction["checksum"] = hashlib.sha256(json.dumps(
                outer_body, sort_keys=True, separators=(",", ":")
            ).encode()).hexdigest()
            journal.path.write_text(json.dumps(transaction) + "\n")

            with self.assertRaises(RuntimeJournalVersionError):
                RuntimeJournal.for_test(root, "alice")

    def test_read_only_state_root_fails_typed(self):
        with tempfile.TemporaryDirectory() as root:
            os.chmod(root, 0o500)
            try:
                with self.assertRaises(RuntimeJournalUnsafeRootError):
                    RuntimeJournal.for_test(root, "alice")
            finally:
                os.chmod(root, 0o700)

    def test_compaction_retains_authoritative_state_and_live_request(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "alice")
            journal.append("deployment-state", {
                "deploymentId": "d", "state": "APPLYING"})
            journal.append("deployment-state", {
                "deploymentId": "d", "state": "ACTIVE"})
            journal.append("request-handle", {
                "request_id": "expired", "expires_at_ms": 100})
            journal.append("request-event", {
                "requestId": "expired", "state": "COMPLETED"})
            journal.append("request-handle", {
                "request_id": "live", "expires_at_ms": 10_000})
            journal.append("request-event", {
                "requestId": "live", "state": "EXECUTING"})
            before = journal.path.stat().st_size

            result = journal.compact(at_ms=1000, retention_ms=100)
            records = RuntimeJournal.for_test(root, "alice").records()

            self.assertLess(journal.path.stat().st_size, before)
            self.assertGreater(result["removedRecords"], 0)
            self.assertEqual(
                [item["payload"]["state"] for item in records
                 if item["kind"] == "deployment-state"],
                ["ACTIVE"],
            )
            self.assertNotIn(
                "expired", json.dumps(records, sort_keys=True))
            self.assertIn("live", json.dumps(records, sort_keys=True))

    def test_protected_envelope_tamper_expiry_and_cleanup(self):
        with tempfile.TemporaryDirectory() as root:
            journal=RuntimeJournal.for_test(root,"alice"); journal.write_envelope("r",b"payload",expires_at_ms=100)
            target=journal.spool/"r.json"; wire=target.read_bytes()
            self.assertNotIn(b"payload",wire)
            self.assertEqual(json.loads(wire)["schema"],
                             "ndnsf-di-protected-request-v3")
            self.assertEqual(journal.read_envelope("r",at_ms=99),b"payload")
            with self.assertRaisesRegex(ValueError,"expired"): journal.read_envelope("r",at_ms=100)
            data=json.loads(target.read_text()); data["ciphertext"]="AAAA"; target.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError,"integrity"): journal.read_envelope("r",at_ms=1)
            self.assertEqual(journal.cleanup(at_ms=200),("r",))

    def test_protected_envelope_is_synced_without_digest_reread(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "alice")
            target = journal.spool / "r.json"
            with mock.patch("os.fsync", wraps=os.fsync) as fsync, \
                 mock.patch.object(
                     Path, "read_bytes",
                     side_effect=AssertionError("envelope digest reread"),
                 ):
                digest = journal.write_envelope(
                    "r", b"payload", expires_at_ms=10_000)

            self.assertTrue(target.is_file())
            self.assertEqual(
                digest,
                "sha256:" + hashlib.sha256(target.read_bytes()).hexdigest(),
            )
            # One sync makes the temporary file contents durable; the other
            # makes the atomic directory entry replacement durable.
            self.assertEqual(fsync.call_count, 2)

    def test_hot_path_uses_incremental_usage_and_record_indexes(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "alice")
            journal.append("request-event", {"requestId": "r", "sequence": 1})
            journal.write_envelope("r", b"payload", expires_at_ms=10_000)
            expected_records = journal.records()
            expected_usage = journal.usage_bytes()

            with mock.patch.object(
                    journal, "_scan_usage_bytes",
                    side_effect=AssertionError("recursive usage scan on hot path")), \
                 mock.patch.object(
                    journal, "_load_records",
                    side_effect=AssertionError("journal reparse on hot path")):
                for sequence in range(2, 22):
                    journal.append("request-event", {
                        "requestId": "r", "sequence": sequence})
                journal.write_envelope(
                    "r-result", b"result", expires_at_ms=10_000)
                self.assertEqual(len(journal.records()), len(expected_records) + 20)
                self.assertGreater(journal.usage_bytes(), expected_usage)

    def test_append_many_flushes_one_atomic_journal_transaction(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "alice")
            with mock.patch("os.fsync", wraps=os.fsync) as fsync:
                records = journal.append_many((
                    ("request-handle", {"request_id": "r"}),
                    ("request-event", {"requestId": "r", "sequence": 1}),
                    ("request-event", {"requestId": "r", "sequence": 2}),
                ))
            self.assertEqual(len(records), 3)
            self.assertEqual(fsync.call_count, 1)
            self.assertEqual(len(journal.records()), 3)

    def test_grouped_envelope_and_reference_use_one_authoritative_flush(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "alice")
            prepared = journal.prepare_envelope(
                "request", b"secret-prompt", expires_at_ms=10_000)
            with mock.patch("os.fsync", wraps=os.fsync) as fsync:
                journal.commit_prepared_envelope(
                    prepared,
                    (("request-handle", {
                        "request_id": "request",
                        "envelope_digest": prepared.wire_digest,
                    }),),
                )

            self.assertEqual(fsync.call_count, 1)
            self.assertEqual(journal.read_envelope("request", at_ms=1),
                             b"secret-prompt")
            self.assertEqual(journal.envelope_digest("request"),
                             prepared.wire_digest)
            self.assertNotIn(b"secret-prompt", journal.path.read_bytes())
            self.assertFalse((journal.spool / "request.json").exists())

    def test_grouped_envelope_recovers_without_spool_mirror(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "alice")
            prepared = journal.prepare_envelope(
                "request", b"recoverable", expires_at_ms=10_000)
            journal.commit_prepared_envelope(
                prepared,
                (("request-handle", {
                    "request_id": "request",
                    "envelope_digest": prepared.wire_digest,
                }),),
            )
            (journal.spool / "request.json").unlink(missing_ok=True)

            restarted = RuntimeJournal.for_test(root, "alice")
            self.assertEqual(restarted.read_envelope("request", at_ms=1),
                             b"recoverable")
            self.assertEqual(restarted.envelope_digest("request"),
                             prepared.wire_digest)

    def test_torn_batch_recovers_none_of_its_logical_records(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "alice")
            journal.append_many((
                ("request-handle", {"request_id": "r"}),
                ("request-event", {"requestId": "r", "sequence": 1}),
                ("request-event", {"requestId": "r", "sequence": 2}),
            ))
            wire = journal.path.read_bytes()
            journal.path.write_bytes(wire[:-10])

            recovered = RuntimeJournal.for_test(root, "alice").records()
            self.assertEqual(recovered, ())


if __name__=="__main__": unittest.main()
