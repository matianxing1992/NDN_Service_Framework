#!/usr/bin/env python3

from __future__ import annotations

import math
import json
import unittest

from py_repoclient import RepoOperationMetrics as NativeRepoOperationMetrics
from py_repoclient.orchestration import (
    REPO_METRIC_PHASES,
    REPO_OPERATION_ID_MAX_BYTES,
    NetworkDistributedRepoClient,
    RepoOperationMetrics,
)


class RepoOperationMetricsContractTest(unittest.TestCase):
    def make_metrics(self) -> RepoOperationMetrics:
        return RepoOperationMetrics(
            operation_id="publish-01HZZ9Q5",
            started_at_ms=1000,
            completed_at_ms=1500,
            phase_timings_ms={
                phase: float(index)
                for index, phase in enumerate(sorted(REPO_METRIC_PHASES), 1)
            },
            logical_payload_bytes=1024,
            data_wire_bytes=1100,
            interest_wire_bytes=100,
            wire_bytes=1200,
            retransmitted_bytes=100,
            payload_store_bytes_read=1024,
            payload_store_bytes_written=1024,
            metadata_store_bytes_read=64,
            metadata_store_bytes_written=64,
            storage_bytes_read=1088,
            storage_bytes_written=1088,
            asymmetric_verifications=1,
            digest_verifications=4,
            asymmetric_verification_ms=0.8,
            digest_verification_ms=1.2,
            control_operations=3,
            metadata_operations=7,
            metadata_record_count=5,
            requested_replica_count=3,
            selected_replica_count=3,
            committed_replica_count=2,
            rejected_replica_receipt_count=1,
        )

    def test_canonical_round_trip_preserves_every_measurement_domain(self) -> None:
        metrics = self.make_metrics()
        encoded = metrics.to_dict()
        decoded = RepoOperationMetrics.from_dict(encoded)
        self.assertEqual(decoded.to_dict(), encoded)
        self.assertEqual(encoded["operationId"], "publish-01HZZ9Q5")
        self.assertEqual(set(encoded["phaseTimingsMs"]), REPO_METRIC_PHASES)
        self.assertEqual(encoded["committedReplicaCount"], 2)
        self.assertEqual(encoded["rejectedReplicaReceiptCount"], 1)

    def test_native_and_orchestration_contracts_round_trip_same_schema(self) -> None:
        expected = self.make_metrics().to_dict()
        native = NativeRepoOperationMetrics()
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
        native.metadata_store_bytes_written = expected[
            "metadataStoreBytesWritten"]
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
            "rejectedReplicaReceiptCount"]
        native.validate()
        self.assertEqual(json.loads(native.to_json()), expected)

    def test_mutation_helpers_keep_phase_and_counter_semantics_separate(self) -> None:
        metrics = RepoOperationMetrics(operation_id="operation-1")
        metrics.record_phase("transfer", 2.5)
        metrics.record_phase("transfer", 1.5)
        metrics.increment("wire_bytes", 4096)
        metrics.increment("control_operations")
        self.assertEqual(metrics.phase_timings_ms, {"transfer": 4.0})
        self.assertEqual(metrics.wire_bytes, 4096)
        self.assertEqual(metrics.control_operations, 1)

    def test_legacy_reservation_phase_is_read_as_session_start(self) -> None:
        decoded = RepoOperationMetrics.from_dict({
            "operationId": "legacy-operation",
            "phaseTimingsMs": {"reservation": 2.0},
            "wireBytes": 1200,
            "storageBytesRead": 1024,
        })
        self.assertEqual(decoded.phase_timings_ms, {"sessionStart": 2.0})
        self.assertEqual(decoded.data_wire_bytes, 1200)
        self.assertEqual(decoded.payload_store_bytes_read, 1024)

    def test_unbounded_or_malformed_input_is_rejected_before_use(self) -> None:
        bad_inputs = [
            {"operationId": ""},
            {"operationId": "x" * (REPO_OPERATION_ID_MAX_BYTES + 1)},
            {"operationId": "op\n1"},
            {"operationId": "op", "phaseTimingsMs": {"packet-loop": 1.0}},
            {"operationId": "op", "phaseTimingsMs": {"transfer": math.inf}},
            {"operationId": "op", "logicalPayloadBytes": -1},
            {"operationId": "op", "logicalPayloadBytes": 1 << 64},
            {
                "operationId": "op",
                "requestedReplicaCount": 1,
                "selectedReplicaCount": 1,
                "committedReplicaCount": 2,
            },
        ]
        for value in bad_inputs:
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    RepoOperationMetrics.from_dict(value)

    def test_network_client_emits_one_stable_operation_record(self) -> None:
        client = NetworkDistributedRepoClient.__new__(
            NetworkDistributedRepoClient)
        operation_id = client.begin_operation_metrics("caller-stable-id")
        client._record_control_phase("reserve", 2.0)
        client._record_control_phase("store", 3.0)
        self.assertEqual(client.operation_metrics().operation_id, operation_id)
        result = client.end_operation_metrics()
        self.assertEqual(result["operationId"], "caller-stable-id")
        self.assertEqual(result["controlOperations"], 2)
        self.assertEqual(
            result["phaseTimingsMs"],
            {"replication": 3.0, "sessionStart": 2.0},
        )
        self.assertIsNone(client.operation_metrics())


if __name__ == "__main__":
    unittest.main()
