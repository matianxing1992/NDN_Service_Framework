#!/usr/bin/env python3
"""Operation metrics are represented by the native Repo value type."""

from __future__ import annotations

import json
import unittest

from py_repoclient import RepoCompatibilityError, RepoOperationMetrics
import py_repoclient.orchestration as legacy_repo


class NativeRepoOperationMetricsTest(unittest.TestCase):
    def test_native_metrics_round_trip_all_measurement_domains(self):
        expected = {
            "operationId": "publish-01HZZ9Q5",
            "startedAtMs": 1000,
            "completedAtMs": 1500,
            "phaseTimingsMs": {
                "transfer": 2.5,
                "sessionStart": 1.0,
            },
            "logicalPayloadBytes": 1024,
            "dataWireBytes": 1100,
            "interestWireBytes": 100,
            "wireBytes": 1200,
            "retransmittedBytes": 100,
            "payloadStoreBytesRead": 1024,
            "payloadStoreBytesWritten": 1024,
            "metadataStoreBytesRead": 64,
            "metadataStoreBytesWritten": 64,
            "storageBytesRead": 1088,
            "storageBytesWritten": 1088,
            "asymmetricVerifications": 1,
            "digestVerifications": 4,
            "asymmetricVerificationMs": 0.8,
            "digestVerificationMs": 1.2,
            "controlOperations": 3,
            "metadataOperations": 7,
            "metadataRecordCount": 5,
            "requestedReplicaCount": 3,
            "selectedReplicaCount": 3,
            "committedReplicaCount": 2,
            "rejectedReplicaReceiptCount": 1,
        }
        native = RepoOperationMetrics()
        native.operation_id = expected["operationId"]
        native.started_at_ms = expected["startedAtMs"]
        native.completed_at_ms = expected["completedAtMs"]
        native.phase_timings_ms = expected["phaseTimingsMs"]
        native.logical_payload_bytes = expected["logicalPayloadBytes"]
        native.data_wire_bytes = expected["dataWireBytes"]
        native.interest_wire_bytes = expected["interestWireBytes"]
        native.wire_bytes = expected["wireBytes"]
        native.retransmitted_bytes = expected["retransmittedBytes"]
        native.payload_store_bytes_read = expected["payloadStoreBytesRead"]
        native.payload_store_bytes_written = expected["payloadStoreBytesWritten"]
        native.metadata_store_bytes_read = expected["metadataStoreBytesRead"]
        native.metadata_store_bytes_written = expected["metadataStoreBytesWritten"]
        native.storage_bytes_read = expected["storageBytesRead"]
        native.storage_bytes_written = expected["storageBytesWritten"]
        native.asymmetric_verifications = expected["asymmetricVerifications"]
        native.digest_verifications = expected["digestVerifications"]
        native.asymmetric_verification_ms = expected["asymmetricVerificationMs"]
        native.digest_verification_ms = expected["digestVerificationMs"]
        native.control_operations = expected["controlOperations"]
        native.metadata_operations = expected["metadataOperations"]
        native.metadata_record_count = expected["metadataRecordCount"]
        native.requested_replica_count = expected["requestedReplicaCount"]
        native.selected_replica_count = expected["selectedReplicaCount"]
        native.committed_replica_count = expected["committedReplicaCount"]
        native.rejected_replica_receipt_count = expected[
            "rejectedReplicaReceiptCount"
        ]
        native.validate()
        self.assertEqual(json.loads(native.to_json()), expected)

    def test_python_metrics_shadow_is_versioned_retired(self):
        with self.assertRaises(RepoCompatibilityError) as captured:
            legacy_repo.RepoOperationMetrics()
        self.assertEqual(captured.exception.code, "SPEC192_COMPATIBILITY_V1")


if __name__ == "__main__":
    unittest.main()
