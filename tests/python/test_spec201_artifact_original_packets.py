"""Owned SQLite wiring with actual source-linked native Data packets."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from test_spec201_repo_name_admission import RepoNameAdmission


class OriginalArtifactPackets(unittest.TestCase):
    def test_real_file_export_reopens_exact_originals_without_serving(self):
        from ndnsf import (FileSegmentedObjectProducer, make_content_data_prefix,
                           verify_data_packet_signature)
        with tempfile.TemporaryDirectory() as root:
            source = Path(root) / "immutable.bin"
            payload = b"a" * 9000
            source.write_bytes(payload)
            profile = json.dumps({"encoding": "raw", "segmentBytes": 4000,
                                  "freshnessMs": 60000}, sort_keys=True, separators=(",", ":"))
            prefix = make_content_data_prefix("/spec201/repo-name-owner", "blob",
                "export-proof", hashlib.sha256(payload).hexdigest(), profile)
            with mock.patch.dict(os.environ, {"NDNSF_FILE_PUBLICATION_CACHE": str(Path(root) / "cache")}):
                first = FileSegmentedObjectProducer(prefix, str(source),
                    signing_identity="/spec201/repo-name-owner", max_segment_size=4000)
                packets = [first.packet(i) for i in range(first.segment_count)]
                self.assertEqual(len(packets), 3)
                self.assertEqual(b"".join(p.content for p in packets), payload)
                self.assertEqual(first.data_count, 0)
                for packet in packets:
                    self.assertTrue(verify_data_packet_signature(packet.wire, first.public_key_der))
                with self.assertRaises((IndexError, RuntimeError)):
                    first.packet(3)
                source.write_bytes(b"modified")
                self.assertEqual(first.packet(0).wire, packets[0].wire)
                source.write_bytes(payload)
                reopened = FileSegmentedObjectProducer(prefix, str(source),
                    signing_identity="/spec201/repo-name-owner", max_segment_size=4000)
                self.assertEqual(reopened.versioned_name, first.versioned_name)
                self.assertEqual([reopened.packet(i).wire for i in range(3)],
                                 [p.wire for p in packets])
                self.assertEqual(reopened.data_count, 0)

    @classmethod
    def setUpClass(cls):
        RepoNameAdmission.setUpClass()
        cls.addClassCleanup(RepoNameAdmission.doClassCleanups)
        from ndnsf import make_segmented_data_packets, inspect_data_name
        cls.packets = make_segmented_data_packets(
            "/spec201/repo-name-owner/original-source", b"x" * 1000,
            signing_identity="/spec201/repo-name-owner", max_segment_size=400)
        cls.names = [inspect_data_name(packet.name)["canonical_name"] for packet in cls.packets]
        cls.prefix = inspect_data_name(cls.packets[0].name)["segment_prefix"]
        cls.source_name = inspect_data_name("/spec201/repo-name-owner/original-source")["canonical_name"]

    def setUp(self):
        from py_repoclient.persistence import SqliteRepositoryPersistence
        self.directory = tempfile.TemporaryDirectory(prefix="spec201-original-artifact-")
        self.addCleanup(self.directory.cleanup)
        self.persistence = SqliteRepositoryPersistence(
            Path(self.directory.name) / "repo.sqlite3", "original-artifact-test", capacity_bytes=1 << 20)
        self.addCleanup(self.persistence.close)

    def retain(self, operation, index=0, wire=None):
        packet = self.packets[index]
        self.persistence.store_original_packet(operation, "payload", self.names[index],
            packet.segment, len(packet.content), packet.wire if wire is None else wire)

    def test_shared_wire_is_charged_once_and_abort_preserves_other_owner(self):
        self.retain("one")
        charged = self.persistence.capacity_status().committed_bytes
        self.assertEqual(charged, len(self.packets[0].wire))
        self.retain("two")
        self.assertEqual(self.persistence.capacity_status().committed_bytes, charged)
        self.assertIsNone(self.persistence.lookup_original_packet(self.names[0], False))
        self.persistence.discard_unactivated_original_packets("one")
        self.assertEqual(self.persistence.capacity_status().committed_bytes, charged)
        self.persistence.discard_unactivated_original_packets("two")
        self.assertEqual(self.persistence.capacity_status().committed_bytes, 0)

    def test_existing_gc_releases_only_failed_original_owner(self):
        from py_repoclient.persistence import ArtifactStorageIdentity
        self.retain("failed")
        self.retain("survivor")
        identity = ArtifactStorageIdentity("a" * 64, 1000, 1)
        self.persistence.claim_garbage_collection(operation_id="failed",
            artifact_digest=identity.content_digest, generation=identity.generation,
            gc_owner="spec201-gc", now_ms=1000, deadline_ms=2000)
        self.persistence.reclaim_temporary_generation(identity, operation_id="failed",
            gc_owner="spec201-gc", now_ms=1001)
        self.assertEqual(self.persistence.capacity_status().committed_bytes, len(self.packets[0].wire))
        self.persistence.discard_unactivated_original_packets("survivor")
        self.assertEqual(self.persistence.capacity_status().committed_bytes, 0)

    def test_name_conflict_does_not_replace_original_wire(self):
        self.retain("one")
        corrupted = self.packets[0].wire[:-1] + bytes([self.packets[0].wire[-1] ^ 1])
        with self.assertRaisesRegex(ValueError, "name-conflict"):
            self.retain("two", wire=corrupted)
        row = self.persistence.connection.execute(
            "SELECT wire, wire_sha256 FROM artifact_original_wires WHERE data_name=?",
            (self.names[0],)).fetchone()
        self.assertEqual(bytes(row[0]), self.packets[0].wire)
        self.assertEqual(row[1], hashlib.sha256(self.packets[0].wire).hexdigest())

    def test_incomplete_source_never_records_ready_metadata(self):
        self.retain("one")
        with self.assertRaisesRegex(ValueError, "incomplete"):
            self.persistence.record_original_source("one", "payload", self.source_name,
                self.prefix, len(self.packets), 1000)
        self.assertEqual(self.persistence.original_sources("one"), ())
        for index in range(1, len(self.packets)):
            self.retain("one", index)
        self.persistence.record_original_source("one", "payload", self.source_name,
            self.prefix, len(self.packets), 1000)
        self.assertEqual(self.persistence.original_sources("one")[0]["source_prefix"], self.prefix)
        self.assertIsNone(self.persistence.lookup_original_packet(self.prefix, True))

    def test_wire_quota_rejection_leaves_no_packet_or_owner_row(self):
        from py_repoclient.persistence import SqliteRepositoryPersistence, LifecycleTransitionError
        persistence = SqliteRepositoryPersistence(
            Path(self.directory.name) / "quota.sqlite3", "quota-test",
            capacity_bytes=len(self.packets[0].wire) - 1)
        try:
            with self.assertRaisesRegex(LifecycleTransitionError, "capacity-insufficient"):
                persistence.store_original_packet("one", "payload", self.names[0], 0,
                    len(self.packets[0].content), self.packets[0].wire)
            self.assertEqual(persistence.connection.execute(
                "SELECT COUNT(*) FROM artifact_original_wires").fetchone()[0], 0)
            self.assertEqual(persistence.connection.execute(
                "SELECT COUNT(*) FROM artifact_original_packets").fetchone()[0], 0)
        finally:
            persistence.close()


if __name__ == "__main__":
    unittest.main()
