from __future__ import annotations

import base64
import tempfile
import unittest
from dataclasses import replace

from ndnsf_distributed_inference.app_sdk import APPClient, RequestState
from ndnsf_distributed_inference.app_sdk.client import RequestRecoveryError
from ndnsf_distributed_inference.app_sdk.runtime_journal import RuntimeJournal
from ndnsf_distributed_inference.core import ExecutionActivateMessage, ReadySetMember


class HistoricalActivationJournalTest(unittest.TestCase):
    def _write_activation_record(self, client, handle):
        # Encode the persisted contract independently of the current runtime API.
        handle = client.open_request(handle.request_id)
        activation = ExecutionActivateMessage(
            handle.requester_identity, handle.request_id, handle.attempt_epoch,
            "sha256:selection", "sha256:plan",
            (ReadySetMember("/provider/a", "prefill", "boot-a", "sha256:ready-a"),),
            handle.expires_at_ms, 1, "historical-requester-signature",
        )
        historical = replace(
            handle, activation_digest=activation.digest(),
            execution_activation_wire=base64.b64encode(activation.to_bytes()).decode("ascii"),
        )
        client.journal.append("request-handle-update", historical.to_record())
        return historical

    def test_active_historical_request_reopens_and_control_rejects_without_writes(self):
        with tempfile.TemporaryDirectory() as root:
            writer = APPClient(RuntimeJournal.for_test(root, "alice"))
            historical = self._write_activation_record(
                writer, writer.submit("deployment", "revision", b"prompt"))
            reader = APPClient(RuntimeJournal.for_test(root, "alice"))
            reopened = reader.open_request(historical.request_id)
            self.assertEqual(reopened.activation_digest, historical.activation_digest)
            self.assertEqual(reopened.execution_activation_wire, historical.execution_activation_wire)
            self.assertEqual(reader.journal.read_envelope(reopened.request_id), b"prompt")
            before_state = reader.status(reopened)
            before_events = tuple(reader.stream(reopened))
            before_wire = reader.journal.path.read_bytes()
            with self.assertRaisesRegex(RequestRecoveryError, "LEGACY_EXECUTION_CONTROL_RETIRED"):
                reader.cancel(reopened, "stop")
            with self.assertRaisesRegex(RequestRecoveryError, "LEGACY_EXECUTION_RESULT_RETIRED"):
                reader._network_result(reopened, object())
            self.assertEqual(reader.status(reopened), before_state)
            self.assertEqual(tuple(reader.stream(reopened)), before_events)
            self.assertEqual(reader.journal.path.read_bytes(), before_wire)

    def test_historical_request_still_rejects_wrong_attempt_and_requester(self):
        with tempfile.TemporaryDirectory() as root:
            writer = APPClient(RuntimeJournal.for_test(root, "alice"))
            historical = self._write_activation_record(
                writer, writer.submit("deployment", "revision", b"prompt"))
            reader = APPClient(RuntimeJournal.for_test(root, "alice"))
            before_wire = reader.journal.path.read_bytes()
            with self.assertRaisesRegex(RequestRecoveryError, "ATTEMPT_EPOCH_MISMATCH"):
                reader.open_request(historical.request_id,
                                    attempt_epoch=historical.attempt_epoch + 1)
            with self.assertRaisesRegex(RequestRecoveryError, "ATTEMPT_EPOCH_MISMATCH"):
                reader.cancel(historical, attempt_epoch=historical.attempt_epoch + 1)
            outsider = APPClient(RuntimeJournal.for_test(root, "alice"), requester_identity="bob")
            with self.assertRaisesRegex(RequestRecoveryError, "REQUESTER_IDENTITY_MISMATCH"):
                outsider.open_request(historical.request_id)
            with self.assertRaisesRegex(RequestRecoveryError, "REQUESTER_IDENTITY_MISMATCH"):
                outsider.cancel(historical)
            self.assertEqual(reader.journal.path.read_bytes(), before_wire)

    def test_completed_historical_request_keeps_result_and_idempotent_cancel(self):
        with tempfile.TemporaryDirectory() as root:
            writer = APPClient(RuntimeJournal.for_test(root, "alice"), executor=bytes.upper)
            historical = self._write_activation_record(
                writer, writer.submit("deployment", "revision", b"prompt"))
            reader = APPClient(RuntimeJournal.for_test(root, "alice"))
            reopened = reader.open_request(historical.request_id)
            self.assertEqual(reader.status(reopened), RequestState.COMPLETED)
            self.assertEqual(reader.result(reopened), b"PROMPT")
            before_wire = reader.journal.path.read_bytes()
            reader.cancel(reopened)
            self.assertEqual(reader.status(reopened), RequestState.COMPLETED)
            self.assertEqual(reader.result(reopened), b"PROMPT")
            self.assertEqual(reader.journal.path.read_bytes(), before_wire)


if __name__ == "__main__":
    unittest.main()
