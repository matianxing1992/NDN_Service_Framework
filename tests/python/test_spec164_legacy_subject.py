#!/usr/bin/env python3

import hashlib
import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
EVIDENCE = (
    ROOT
    / "specs"
    / "164-distributed-repo-large-artifact-transport"
    / "evidence"
    / "legacy-subject.json"
)


class Spec164LegacySubjectTest(unittest.TestCase):
    def test_frozen_subject_has_stable_integrity_digest(self) -> None:
        evidence = json.loads(EVIDENCE.read_text())
        canonical = json.dumps(
            evidence["subject"],
            sort_keys=True,
            separators=(",", ":"),
        )
        observed = hashlib.sha256(canonical.encode()).hexdigest()
        self.assertEqual(observed, evidence["subjectSha256"])

    def test_evidence_does_not_claim_unmeasured_performance(self) -> None:
        evidence = json.loads(EVIDENCE.read_text())
        self.assertEqual(
            evidence["subject"]["claimLevel"],
            "captured-not-benchmarked",
        )
        self.assertEqual(evidence["measurements"], [])
        self.assertEqual(evidence["performanceVerdict"], "NOT_MEASURED")

    def test_legacy_scaling_facts_are_explicit(self) -> None:
        subject = json.loads(EVIDENCE.read_text())["subject"]
        behavior = subject["observedBehavior"]
        counts = subject["operationCountModel"]
        self.assertEqual(behavior["putFileDefaultChunkBytes"], 16 * 1024 * 1024)
        self.assertEqual(behavior["dataPacketMaxSegmentBytes"], 4000)
        self.assertEqual(behavior["storeObjectAttempts"], 3)
        self.assertTrue(behavior["packetRequestCarriesExactWire"])
        self.assertTrue(behavior["sqliteStoresPacketWireBlob"])
        self.assertIn("STORE_PACKET", behavior["storeObjectControlOperations"])
        self.assertIn(
            "STORE_PACKET_PULL",
            behavior["storeObjectControlOperations"],
        )
        self.assertIn(
            "COMMIT_PACKET_SET",
            behavior["storeObjectControlOperations"],
        )
        self.assertIn("data_packets", behavior["sqliteTables"])
        self.assertIn("object_packet_refs", behavior["sqliteTables"])
        self.assertEqual(counts["maximumStoreAttempts"], 3)
        self.assertIn(
            "signedDataPacketCount",
            counts["pushStoreCallsPerStoreAttempt"],
        )
        self.assertIn(
            "replicationFactor",
            counts["commitCallsPerSuccessfulStoreAttempt"],
        )


if __name__ == "__main__":
    unittest.main()
