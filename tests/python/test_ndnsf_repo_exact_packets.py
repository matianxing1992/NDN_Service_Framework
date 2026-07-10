#!/usr/bin/env python3
"""Exact-name, immutable-wire repository tests."""

from __future__ import annotations

import hashlib
import json
import sqlite3
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from ndnsf import DataPacket, make_segmented_data_packets
from ndnsf_distributed_inference.repo import (
    LocalDistributedRepo,
    NetworkDistributedRepoClient,
    RepoNodeApp,
    RepoObjectManifest,
    StorageCapability,
    _BoundedRepoHotCache,
    _packet_set_versioned_data_name,
)


def make_repo(database: Path, *, budget: int = 64 * 1024) -> RepoNodeApp:
    repo = RepoNodeApp.__new__(RepoNodeApp)
    repo.repo_node = "/repo/exact"
    repo.capacity_bytes = 8 * 1024 * 1024
    repo.memory_cache_bytes = budget
    repo._hot_cache = _BoundedRepoHotCache(budget)
    repo._cache_bytes = 0
    repo._db_lock = threading.RLock()
    repo._db = sqlite3.connect(database, check_same_thread=False)
    repo._remember_catalog_change = lambda _manifest, _state: None
    repo._init_sqlite()
    return repo


def make_manifest(name: str, payload: bytes, packets: list[DataPacket]) -> RepoObjectManifest:
    return RepoObjectManifest(
        object_name=name,
        object_type="ndn-data-packet-set",
        sha256=hashlib.sha256(payload).hexdigest(),
        size=len(payload),
        segment_count=len(packets),
        packet_names=tuple(packet.name for packet in packets),
    )


class ExactPacketRepositoryTest(unittest.TestCase):
    def setUp(self) -> None:
        self.payload = b"exact-packet-payload-" * 700
        self.packets = make_segmented_data_packets(
            "/data/models/qwen",
            self.payload,
            signing_identity="/test/exact-packet-publisher",
            max_segment_size=4096,
        )
        self.assertGreater(len(self.packets), 1)

    def test_exact_names_and_wires_survive_restart(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            database = Path(tmp) / "repo.sqlite3"
            manifest = make_manifest("/objects/qwen", self.payload, self.packets)
            repo = make_repo(database)
            repo._persist_packets(manifest, self.packets)

            stored_names = [
                str(row[0]) for row in repo._db.execute(
                    "SELECT data_name FROM data_packets ORDER BY data_name"
                ).fetchall()
            ]
            self.assertEqual(set(stored_names), {packet.name for packet in self.packets})
            self.assertFalse(any("/ndn-data/" in name for name in stored_names))
            self.assertFalse(any(name.startswith(manifest.object_name + "/seg/")
                                 for name in stored_names))
            repo._db.close()

            restarted = make_repo(database)
            loaded_manifest, loaded_packets = restarted._load_persisted_packets(
                manifest.object_name)
            self.assertEqual(loaded_manifest.packet_names,
                             tuple(packet.name for packet in self.packets))
            self.assertEqual([packet.wire for packet in loaded_packets],
                             [packet.wire for packet in self.packets])
            for expected in self.packets:
                actual = restarted._load_persisted_packet(expected.name)
                self.assertEqual(actual.name, expected.name)
                self.assertEqual(actual.wire, expected.wire)
            prefix_packets = restarted._load_persisted_packet_prefix(
                self.packets[1].name)
            self.assertEqual(
                [packet.name for packet in prefix_packets],
                [packet.name for packet in self.packets],
            )
            restarted._db.close()

    def test_name_mismatch_and_immutable_conflict_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            manifest = make_manifest("/objects/conflict", self.payload, self.packets)
            repo._persist_packets(manifest, self.packets)

            mismatched = list(self.packets)
            mismatched[0] = DataPacket(
                "/data/wrong/v=1/seg=0",
                self.packets[0].segment,
                self.packets[0].wire,
            )
            with self.assertRaisesRegex(ValueError, "name/wire mismatch"):
                repo._persist_packets(manifest, mismatched)

            conflicting = list(self.packets)
            changed_wire = bytearray(conflicting[0].wire)
            changed_wire[-1] ^= 0x01
            conflicting[0] = DataPacket(
                conflicting[0].name,
                conflicting[0].segment,
                bytes(changed_wire),
            )
            with self.assertRaisesRegex(ValueError, "immutable NDN Data name conflict"):
                repo._persist_packets(manifest, conflicting)
            repo._db.close()

    def test_shared_packet_references_are_reclaimed_after_last_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            packet = [self.packets[0]]
            manifest_a = make_manifest("/objects/shared-a", self.payload, packet)
            manifest_b = make_manifest("/objects/shared-b", self.payload, packet)
            repo._persist_packets(manifest_a, packet)
            repo._persist_packets(manifest_b, packet)

            count = repo._db.execute("SELECT COUNT(*) FROM data_packets").fetchone()[0]
            self.assertEqual(count, 1)
            self.assertTrue(repo._delete_object(manifest_a.object_name))
            self.assertEqual(repo._load_persisted_packet(packet[0].name).wire,
                             packet[0].wire)
            self.assertTrue(repo._delete_object(manifest_b.object_name))
            with self.assertRaises(KeyError):
                repo._load_persisted_packet(packet[0].name)
            repo._db.close()

    def test_packet_serving_prefix_is_original_versioned_name(self) -> None:
        prefix = _packet_set_versioned_data_name(self.packets)
        self.assertTrue(prefix.startswith("/data/models/qwen/v="))
        self.assertTrue(all(packet.name.startswith(prefix + "/seg=")
                            for packet in self.packets))

    def test_exact_packet_read_uses_shared_bounded_cache(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            packet = [self.packets[0]]
            manifest = make_manifest("/objects/cache", self.payload, packet)
            repo._persist_packets(manifest, packet)
            repo._hot_cache = _BoundedRepoHotCache(repo.memory_cache_bytes)

            repo._load_persisted_packet(packet[0].name)
            cold = repo._cache_status()
            repo._load_persisted_packet(packet[0].name)
            hot = repo._cache_status()
            self.assertEqual(cold["backingReads"], 1)
            self.assertEqual(hot["backingReads"], 1)
            self.assertEqual(hot["hits"], 1)
            repo._db.close()

    def test_manifest_overwrite_cannot_serve_orphan_from_hot_cache(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            object_name = "/objects/overwrite"
            first = [self.packets[0]]
            second = [self.packets[1]]
            repo._persist_packets(
                make_manifest(object_name, self.payload, first), first)
            repo._load_persisted_packet(first[0].name)
            self.assertIsNotNone(repo._hot_cache.get_packet(first[0].name))

            repo._persist_packets(
                make_manifest(object_name, self.payload, second), second)
            with self.assertRaises(KeyError):
                repo._load_persisted_packet(first[0].name)
            self.assertEqual(
                repo._load_persisted_packet(second[0].name).wire,
                second[0].wire,
            )
            repo._db.close()

    def test_packet_consumer_preserves_manifest_order_and_wire_identity(self) -> None:
        client = NetworkDistributedRepoClient.__new__(NetworkDistributedRepoClient)
        manifest = make_manifest("/objects/consumer", self.payload, self.packets)
        manifest = RepoObjectManifest(
            **{
                **manifest.__dict__,
                "replica_nodes": ("/repo/a",),
            }
        )
        by_name = {packet.name: packet for packet in self.packets}
        client.fetch_packet = lambda _repo, name: by_name[name]

        fetched = client.fetch_signed_packets(manifest)

        self.assertEqual([packet.name for packet in fetched],
                         list(manifest.packet_names))
        self.assertEqual([packet.wire for packet in fetched],
                         [packet.wire for packet in self.packets])

    def test_packet_consumer_rejects_invalid_indexes_before_fetch(self) -> None:
        client = NetworkDistributedRepoClient.__new__(NetworkDistributedRepoClient)
        client.fetch_packet = lambda _repo, _name: self.fail("fetch should not run")
        base = make_manifest("/objects/invalid", self.payload, self.packets)

        empty = RepoObjectManifest(**{
            **base.__dict__, "packet_names": (), "segment_count": 0,
            "replica_nodes": ("/repo/a",),
        })
        with self.assertRaisesRegex(ValueError, "index is empty"):
            client.fetch_signed_packets(empty)

        wrong_count = RepoObjectManifest(**{
            **base.__dict__, "segment_count": base.segment_count + 1,
            "replica_nodes": ("/repo/a",),
        })
        with self.assertRaisesRegex(ValueError, "count mismatch"):
            client.fetch_signed_packets(wrong_count)

        duplicate = RepoObjectManifest(**{
            **base.__dict__,
            "packet_names": base.packet_names[:-1] + (base.packet_names[0],),
            "replica_nodes": ("/repo/a",),
        })
        with self.assertRaisesRegex(ValueError, "duplicate"):
            client.fetch_signed_packets(duplicate)

    def test_packet_consumer_fails_atomically_and_tries_next_replica(self) -> None:
        client = NetworkDistributedRepoClient.__new__(NetworkDistributedRepoClient)
        manifest = make_manifest("/objects/failover", self.payload, self.packets)
        manifest = RepoObjectManifest(**{
            **manifest.__dict__, "replica_nodes": ("/repo/a", "/repo/b"),
        })
        calls: list[tuple[str, str]] = []
        by_name = {packet.name: packet for packet in self.packets}

        def fetch(repo: str, name: str) -> DataPacket:
            calls.append((repo, name))
            if repo == "/repo/a" and name == manifest.packet_names[-1]:
                raise KeyError(name)
            return by_name[name]

        client.fetch_packet = fetch
        fetched = client.fetch_signed_packets(manifest)
        self.assertEqual([packet.name for packet in fetched],
                         list(manifest.packet_names))
        self.assertEqual(
            [name for repo, name in calls if repo == "/repo/b"],
            list(manifest.packet_names),
        )

    def test_packet_consumer_rejects_declared_name_different_from_wire(self) -> None:
        client = NetworkDistributedRepoClient.__new__(NetworkDistributedRepoClient)
        manifest = make_manifest("/objects/mismatch", self.payload, self.packets)
        manifest = RepoObjectManifest(**{
            **manifest.__dict__, "replica_nodes": ("/repo/a",),
        })
        client.fetch_packet = lambda _repo, name: DataPacket(
            name, self.packets[0].segment, self.packets[0].wire)
        with self.assertRaisesRegex(RuntimeError, "name mismatch"):
            client.fetch_signed_packets(manifest)

    def test_exact_packet_fetch_uses_prepared_repo_forwarding_hint(self) -> None:
        client = NetworkDistributedRepoClient.__new__(NetworkDistributedRepoClient)
        client.timeout_ms = 1000
        packet = self.packets[0]
        versioned_prefix = packet.name.rsplit("/seg=", 1)[0]
        client._prepared_packet_prefixes = {versioned_prefix}
        client._request_specific_repo = lambda **_kwargs: SimpleNamespace(
            payload=json.dumps({
                "dataName": packet.name,
                "wireSha256": hashlib.sha256(packet.wire).hexdigest(),
                "forwardingHints": ["/repo/provider/B"],
            }).encode()
        )

        with patch(
            "ndnsf_distributed_inference.repo.fetch_exact_data_packet",
            return_value=packet,
        ) as fetch:
            fetched = client.fetch_packet("/repo/B", packet.name)

        self.assertEqual(fetched.wire, packet.wire)
        self.assertEqual(
            fetch.call_args.kwargs["forwarding_hints"],
            ["/repo/provider/B"],
        )

    def test_opaque_object_path_remains_separate_from_packet_index(self) -> None:
        repo = LocalDistributedRepo((StorageCapability(
            repo_node="/repo/opaque", free_bytes=1024 * 1024),))
        payload = b"encrypted-uav-recording-or-model-bytes"
        manifest = repo.put(
            object_name="/objects/opaque",
            payload=payload,
            object_type="uav-recording",
        )

        self.assertEqual(manifest.packet_names, ())
        self.assertEqual(repo.fetch_object(manifest.object_name, manifest), payload)


if __name__ == "__main__":
    unittest.main()
