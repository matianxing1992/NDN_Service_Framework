from __future__ import annotations

import base64
import hashlib
import hmac
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

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
        with tempfile.TemporaryDirectory() as parent:
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
                test_only_allow_ephemeral_state_root=True,
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

            journal_without_key = RuntimeJournal(
                root, "alice-without-key", test_only_allow_ephemeral_state_root=True)
            with self.assertRaisesRegex(RuntimeJournalKeyError, "key provider"):
                journal_without_key.write_envelope(
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

    def test_corrupt_or_nested_transaction_never_replaces_live_index(self):
        def encode(record):
            return json.dumps(record, sort_keys=True, separators=(",", ":")).encode()

        def checksummed(record):
            body = {key: value for key, value in record.items() if key != "checksum"}
            return {**body, "checksum": hashlib.sha256(encode(body)).hexdigest()}

        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "alice")
            prior = journal.append("request-event", {"sequence": 1})
            reader = RuntimeJournal.for_test(root, "alice")
            prefix = journal.path.read_bytes()
            batch = journal.append_many((("request-event", {"sequence": 2}),
                                         ("request-event", {"sequence": 3})))
            transaction = journal.path.read_bytes()[len(prefix):]

            corrupt = json.loads(transaction)
            corrupt["payload"]["records"][1]["payload"]["sequence"] = 99
            nested = json.loads(transaction)
            nested["payload"]["records"][1] = json.loads(transaction)
            for malformed, message in ((corrupt, "transaction checksum mismatch"),
                                       (nested, "nested runtime journal transaction")):
                with self.subTest(message=message):
                    wire = prefix + encode(checksummed(malformed)) + b"\n"
                    journal.path.write_bytes(wire)
                    with self.assertRaisesRegex(ValueError, message):
                        reader.records()
                    self.assertEqual(reader._records_cache, (prior,))
                    self.assertEqual(journal.path.read_bytes(), wire)

            journal.path.write_bytes(prefix + transaction)
            self.assertEqual(reader.records(), (prior, *batch))

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

    def test_reads_legacy_envelopes_and_checks_the_requested_identity(self):
        key = bytes(range(32))
        provider = StaticRequestEnvelopeKeyProvider(RequestEnvelopeKey("old-key", key))
        encode = lambda value: json.dumps(
            value, sort_keys=True, separators=(",", ":")).encode("utf-8")
        payload = b"old request"
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal(
                root, "alice", envelope_key_provider=provider,
                test_only_allow_ephemeral_state_root=True)
            for version in (1, 2):
                with self.subTest(version=version):
                    request_id = f"old-v{version}"
                    metadata = {
                        "schema": f"ndnsf-di-protected-request-v{version}",
                        "requestId": request_id,
                        "expiresAtMs": 100,
                    }
                    if version == 1:
                        body = {**metadata, "payload": base64.b64encode(payload).decode()}
                        body["mac"] = hmac.new(key, encode(body), hashlib.sha256).hexdigest()
                    else:
                        nonce = bytes(12)
                        ciphertext = AESGCM(key).encrypt(nonce, payload, encode(metadata))
                        body = {
                            **metadata,
                            "nonce": base64.b64encode(nonce).decode(),
                            "ciphertext": base64.b64encode(ciphertext).decode(),
                        }
                    wire = encode(body)
                    (journal.spool / f"{request_id}.json").write_bytes(wire)
                    self.assertEqual(journal.read_envelope(request_id, at_ms=99), payload)
                    with self.assertRaisesRegex(ValueError, "expired"):
                        journal.read_envelope(request_id, at_ms=100)
                    (journal.spool / "other-request.json").write_bytes(wire)
                    with self.assertRaisesRegex(ValueError, "identity mismatch"):
                        journal.read_envelope("other-request", at_ms=99)

    def test_native_write_uses_legacy_v3_wire_for_large_ordinary_request(self):
        key = bytes(range(32))
        provider = StaticRequestEnvelopeKeyProvider(RequestEnvelopeKey("旧密钥", key))
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal(
                root, "alice", envelope_key_provider=provider,
                test_only_allow_ephemeral_state_root=True)
            payload = b"x" * ((16 << 20) + 1)
            prepared = journal.prepare_envelope("旧请求", payload, expires_at_ms=100)
            body = json.loads(prepared.encoded)
            nonce = base64.b64decode(body.pop("nonce"), validate=True)
            ciphertext = base64.b64decode(body.pop("ciphertext"), validate=True)
            self.assertEqual(body, {
                "schema": "ndnsf-di-protected-request-v3",
                "requestId": "旧请求",
                "expiresAtMs": 100,
                "keyId": "旧密钥",
            })
            aad = json.dumps(body, sort_keys=True, separators=(",", ":")).encode("utf-8")
            self.assertEqual(AESGCM(key).decrypt(nonce, ciphertext, aad), payload)
            self.assertEqual(
                prepared.wire_digest, "sha256:" + hashlib.sha256(prepared.encoded).hexdigest())

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
            records = journal.append_many((
                ("request-handle", {"request_id": "r"}),
                ("request-event", {"requestId": "r", "sequence": 1}),
                ("request-event", {"requestId": "r", "sequence": 2}),
            ))
            self.assertEqual(len(records), 3)
            line, = journal.path.read_bytes().splitlines()
            self.assertEqual(json.loads(line)["payload"]["records"], list(records))
            reopened = RuntimeJournal.for_test(root, "alice")
            self.assertEqual(reopened.records(), records)

    def test_native_journal_framing_matches_legacy_canonical_bytes(self):
        def canonical(record):
            return json.dumps(record, sort_keys=True, separators=(",", ":")).encode()

        def assert_legacy_checksum(record):
            unsigned = {key: value for key, value in record.items() if key != "checksum"}
            self.assertEqual(record["checksum"], hashlib.sha256(canonical(unsigned)).hexdigest())

        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "alice")
            data = {"控制": [-(1 << 130), -0.0, 1e-7, "\ud800", "\x7f"]}
            journal.append("单条\n", data)
            journal.append_many((("request-event", data), ("请求\ud800", data)))
            single_line, batch_line = journal.path.read_bytes().splitlines()
            single, batch = json.loads(single_line), json.loads(batch_line)
            self.assertEqual(single_line, canonical(single))
            self.assertEqual(batch_line, canonical(batch))
            for record in (single, batch, *batch["payload"]["records"]):
                assert_legacy_checksum(record)
            self.assertEqual(RuntimeJournal.for_test(root, "alice").records(),
                             (single, *batch["payload"]["records"]))

    def test_grouped_envelope_and_reference_use_one_authoritative_flush(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "alice")
            prepared = journal.prepare_envelope(
                "request", b"secret-prompt", expires_at_ms=10_000)
            journal.commit_prepared_envelope(
                prepared,
                (("request-handle", {
                    "request_id": "request",
                    "envelope_digest": prepared.wire_digest,
                }),),
            )
            line, = journal.path.read_bytes().splitlines()
            records = json.loads(line)["payload"]["records"]
            self.assertEqual([record["kind"] for record in records],
                             ["protected-envelope", "request-handle"])
            self.assertEqual(journal.read_envelope("request", at_ms=1),
                             b"secret-prompt")
            self.assertEqual(journal.envelope_digest("request"),
                             prepared.wire_digest)
            self.assertNotIn(b"secret-prompt", journal.path.read_bytes())
            self.assertFalse((journal.spool / "request.json").exists())

    def test_partial_native_append_rolls_back_and_allows_retry(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "alice")
            journal.append("request-event", {"sequence": 1})
            before = journal.path.read_bytes()
            subprocess.run([sys.executable, "-c", """
import resource
import signal
import sys
from ndnsf_distributed_inference.app_sdk.runtime_journal import RuntimeJournal

journal = RuntimeJournal.for_test(sys.argv[1], "alice")
original = journal.path.read_bytes()
soft, hard = resource.getrlimit(resource.RLIMIT_FSIZE)
signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
resource.setrlimit(resource.RLIMIT_FSIZE, (len(original) + 16, hard))
try:
    try:
        journal.append("request-event", {"sequence": 2, "details": "x" * 4096})
    except OSError as error:
        assert "journal append failed" in str(error), error
    else:
        raise AssertionError("partial append unexpectedly succeeded")
    assert journal.path.read_bytes() == original
    assert len(journal.records()) == 1
finally:
    resource.setrlimit(resource.RLIMIT_FSIZE, (soft, hard))
journal.append("request-event", {"sequence": 2})
""", root], check=True, timeout=20)
            self.assertTrue(journal.path.read_bytes().startswith(before))
            self.assertEqual([item["payload"]["sequence"] for item in journal.records()],
                             [1, 2])

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
            prior = journal.append("request-event", {"requestId": "prior"})
            prefix = journal.path.read_bytes()
            journal.append_many((
                ("request-handle", {"request_id": "r"}),
                ("request-event", {"requestId": "r", "sequence": 1}),
                ("request-event", {"requestId": "r", "sequence": 2}),
            ))
            partial = journal.path.read_bytes()[len(prefix):-10]
            journal.path.write_bytes(prefix)
            crash = subprocess.run([sys.executable, "-c", """
import os
import sys

fragment = bytes.fromhex(sys.argv[2])
with open(sys.argv[1], "ab", buffering=0) as journal_file:
    assert journal_file.write(fragment) == len(fragment)
    os.fsync(journal_file.fileno())
    os._exit(86)
""", str(journal.path), partial.hex()], check=False, timeout=20)
            self.assertEqual(crash.returncode, 86)

            reopened = RuntimeJournal.for_test(root, "alice")
            self.assertEqual(reopened.records(), (prior,))
            with self.assertRaisesRegex(ValueError, "corrupt runtime journal"):
                reopened.records(tolerate_torn_tail=False)
            after = reopened.append("request-event", {"requestId": "after"})
            self.assertEqual(RuntimeJournal.for_test(root, "alice").records(), (prior, after))
            self.assertTrue(reopened.path.read_bytes().startswith(prefix))
            self.assertNotIn(partial, reopened.path.read_bytes())

    def test_appends_to_valid_unterminated_record_and_rejects_corrupt_complete_line(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "alice")
            first = journal.append("request-event", {"sequence": 1})
            wire = journal.path.read_bytes()[:-1]
            journal.path.write_bytes(wire)
            reopened = RuntimeJournal.for_test(root, "alice")
            second = reopened.append("request-event", {"sequence": 2})
            self.assertEqual(RuntimeJournal.for_test(root, "alice").records(), (first, second))
            self.assertTrue(journal.path.read_bytes().startswith(wire + b"\n"))
            corrupt = journal.path.read_bytes() + b'{"torn"\n'
            journal.path.write_bytes(corrupt)
            with self.assertRaisesRegex(ValueError, "corrupt runtime journal"):
                RuntimeJournal.for_test(root, "alice")
            self.assertEqual(journal.path.read_bytes(), corrupt)

    def test_same_size_replacement_is_revalidated_before_tail_repair(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "alice")
            journal.append("request-event", {"sequence": 1})
            with journal.path.open("ab") as output:
                output.write(b'{"incomplete' + b" " * 128)
            size = journal.path.stat().st_size
            reopened = RuntimeJournal.for_test(root, "alice")

            other = RuntimeJournal.for_test(root, "new")
            replacement = other.append("request-event", {"sequence": 2})
            wire = other.path.read_bytes()[:-1]
            self.assertLess(len(wire), size)
            other.path.write_bytes(wire.ljust(size - 1, b" ") + b"\n")
            os.replace(other.path, journal.path)
            self.assertEqual(journal.path.stat().st_size, size)

            after = reopened.append("request-event", {"sequence": 3})
            self.assertEqual(RuntimeJournal.for_test(root, "alice").records(),
                             (replacement, after))


if __name__=="__main__": unittest.main()
