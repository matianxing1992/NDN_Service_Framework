from __future__ import annotations

import unittest
from types import SimpleNamespace


class CollaborationOperationStatusContractTest(unittest.TestCase):
    def test_unknown_progress_is_distinct_from_zero(self):
        from ndnsf.runtime_telemetry import ServiceOperationStatus

        unknown = ServiceOperationStatus(
            operation_id="prepare:prefill", operation="prepare",
            role="prefill", progress_known=False, progress=0.0,
            attempt=1, epoch=1, sequence=1)
        known = ServiceOperationStatus(
            operation_id="prepare:prefill", operation="prepare",
            role="prefill", progress_known=True, progress=0.0,
            attempt=1, epoch=1, sequence=2)
        self.assertFalse(unknown.progress_known)
        self.assertTrue(known.progress_known)

    def test_status_rejects_invalid_sequence_and_oversized_details(self):
        from ndnsf.runtime_telemetry import ServiceOperationStatus

        with self.assertRaises(ValueError):
            ServiceOperationStatus(
                operation_id="op", operation="prepare", sequence=0)
        with self.assertRaises(ValueError):
            ServiceOperationStatus(
                operation_id="op", operation="prepare", sequence=1,
                details_payload=b"x" * 4097)

    def test_requester_query_and_request_snapshot_are_typed_and_bound(self):
        from ndnsf.service import ServiceUser

        value = {
            "providerName": "/provider/a", "serviceName": "/LLM/Test",
            "requestId": "request-1", "selectionDigest": "digest-1",
            "state": "Running", "memberStatuses": [{
                "operationId": "prepare:prefill", "operation": "prepare",
                "serviceName": "/LLM/Test", "providerName": "/provider/a",
                "requestId": "request-1", "role": "prefill", "attempt": 1,
                "epoch": 1, "sequence": 2, "state": "RUNNING",
                "progressKnown": True, "progress": 0.5,
            }],
        }
        native = SimpleNamespace(
            query_collaboration_status=lambda *args: dict(value),
            get_collaboration_status_snapshot=lambda *args: [dict(value)])
        user = ServiceUser.__new__(ServiceUser)
        user._native = native
        exact = user.query_collaboration_status(
            provider="/provider/a", service="/LLM/Test",
            selection_digest="digest-1")
        self.assertEqual(exact.member_statuses[0].role, "prefill")
        self.assertEqual(user.collaboration_status("request-1"), (exact,))

        native.query_collaboration_status = lambda *args: {
            **value, "providerName": "/provider/attacker"}
        with self.assertRaises(ValueError):
            user.query_collaboration_status(
                provider="/provider/a", service="/LLM/Test",
                selection_digest="digest-1")


if __name__ == "__main__":
    unittest.main()
