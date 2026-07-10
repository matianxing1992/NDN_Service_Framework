#!/usr/bin/env python3
"""SQLite-authoritative, bounded hot-cache tests for NDNSF-DistributedRepo."""

from __future__ import annotations

import hashlib
import sqlite3
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from ndnsf import DataPacket, make_segmented_data_packets
import ndnsf_distributed_inference.repo as repo_module
from ndnsf_distributed_inference.repo import (
    NetworkDistributedRepoClient,
    RepoNodeApp,
    RepoObjectManifest,
    _BoundedRepoHotCache,
)


def make_manifest(name: str, payload: bytes, *, segments: int = 1) -> RepoObjectManifest:
    return RepoObjectManifest(
        object_name=name,
        object_type="tiered-cache-test",
        sha256=hashlib.sha256(payload).hexdigest(),
        size=len(payload),
        segment_count=segments,
        replication_factor=1,
        min_replication_factor=1,
        max_replication_factor=1,
        replica_nodes=("/repo/test",),
    )


def make_repo(database: Path, *, budget: int = 4096) -> RepoNodeApp:
    repo = RepoNodeApp.__new__(RepoNodeApp)
    repo.repo_node = "/repo/test"
    repo.capacity_bytes = 1024 * 1024
    repo.memory_cache_bytes = budget
    repo._hot_cache = _BoundedRepoHotCache(budget)
    repo._cache_bytes = 0
    repo._db_lock = threading.RLock()
    repo._db = sqlite3.connect(database, check_same_thread=False)
    repo._remember_catalog_change = lambda _manifest, _state: None
    repo._init_sqlite()
    return repo


class BoundedRepoHotCacheTest(unittest.TestCase):
    def test_object_and_packet_entries_share_one_lru_budget(self) -> None:
        payload = b"a" * 96
        manifest_a = make_manifest("/cache/A", payload)
        packets = [
            DataPacket(name="/cache/B/seg=0", segment=0, wire=b"b" * 80),
            DataPacket(name="/cache/B/seg=1", segment=1, wire=b"c" * 80),
        ]
        manifest_b = make_manifest(
            "/cache/B", b"".join(packet.wire for packet in packets), segments=2)
        object_charge = _BoundedRepoHotCache._object_charge(manifest_a, payload)
        packet_charge = _BoundedRepoHotCache._packet_charge(manifest_b, packets)
        cache = _BoundedRepoHotCache(max(object_charge, packet_charge))

        cache.put_object(manifest_a, payload)
        cache.put_packets(manifest_b, packets)
        status = cache.status(storage_backend="tiered", authoritative_backend="sqlite")

        self.assertLessEqual(status["usedBytes"], status["budgetBytes"])
        self.assertEqual(status["entryCount"], 1)
        self.assertEqual(status["evictions"], 1)
        self.assertIsNone(cache.get_object(manifest_a.object_name))
        self.assertIsNotNone(cache.get_packets(manifest_b.object_name))

    def test_lru_recency_oversized_and_zero_budget(self) -> None:
        payload = b"x" * 64
        manifests = [make_manifest(f"/cache/{name}", payload) for name in "ABC"]
        charge = _BoundedRepoHotCache._object_charge(manifests[0], payload)
        cache = _BoundedRepoHotCache(charge * 2)
        cache.put_object(manifests[0], payload)
        cache.put_object(manifests[1], payload)
        self.assertIsNotNone(cache.get_object(manifests[0].object_name))
        cache.put_object(manifests[2], payload)
        self.assertIsNone(cache.get_object(manifests[1].object_name))
        status = cache.status(storage_backend="tiered", authoritative_backend="sqlite")
        self.assertGreaterEqual(status["hits"], 1)
        self.assertGreaterEqual(status["misses"], 1)
        self.assertGreaterEqual(status["evictions"], 1)
        self.assertLessEqual(status["usedBytes"], status["budgetBytes"])

        oversized = _BoundedRepoHotCache(charge - 1)
        oversized.put_object(manifests[0], payload)
        oversized_status = oversized.status(
            storage_backend="tiered", authoritative_backend="sqlite")
        self.assertEqual(oversized_status["entryCount"], 0)
        self.assertEqual(oversized_status["oversizedBypasses"], 1)

        disabled = _BoundedRepoHotCache(0)
        disabled.put_object(manifests[0], payload)
        disabled_status = disabled.status(
            storage_backend="sqlite", authoritative_backend="sqlite")
        self.assertEqual(disabled_status["cachePolicy"], "disabled")
        self.assertEqual(disabled_status["entryCount"], 0)
        self.assertEqual(disabled_status["admissions"], 0)

    def test_concurrent_hits_preserve_accounting(self) -> None:
        payload = b"concurrent" * 16
        manifest = make_manifest("/cache/concurrent", payload)
        cache = _BoundedRepoHotCache(4096)
        cache.put_object(manifest, payload)
        failures: list[str] = []

        def read_many() -> None:
            for _ in range(100):
                item = cache.get_object(manifest.object_name)
                if item is None or item[1] != payload:
                    failures.append("mismatch")

        threads = [threading.Thread(target=read_many) for _ in range(8)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

        status = cache.status(storage_backend="tiered", authoritative_backend="sqlite")
        self.assertFalse(failures)
        self.assertEqual(status["hits"], 800)
        self.assertLessEqual(status["usedBytes"], status["budgetBytes"])


class SqliteAuthoritativeRepoTest(unittest.TestCase):
    def test_restart_is_cold_then_hot(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            database = Path(tmp) / "repo.sqlite3"
            payload = b"restart-payload" * 20
            manifest = make_manifest("/repo/restart", payload)

            first = make_repo(database)
            first._persist_object(manifest, payload)
            first_status = first._cache_status()
            self.assertEqual(first_status["backingWrites"], 1)
            self.assertEqual(first_status["admissions"], 1)
            first._db.close()

            restarted = make_repo(database)
            self.assertEqual(restarted._cache_status()["entryCount"], 0)
            loaded_manifest, loaded_payload = restarted._load_persisted_object(
                manifest.object_name)
            self.assertEqual(loaded_manifest.sha256, manifest.sha256)
            self.assertEqual(loaded_payload, payload)
            cold_status = restarted._cache_status()
            self.assertEqual(cold_status["misses"], 1)
            self.assertEqual(cold_status["backingReads"], 1)

            self.assertEqual(
                restarted._load_persisted_object(manifest.object_name)[1], payload)
            hot_status = restarted._cache_status()
            self.assertEqual(hot_status["hits"], 1)
            self.assertEqual(hot_status["backingReads"], 1)
            restarted._db.close()


    def test_failed_sqlite_write_never_admits_cache_entry(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            repo._db.execute("""
                CREATE TRIGGER reject_repo_insert
                BEFORE INSERT ON objects
                BEGIN
                  SELECT RAISE(ABORT, 'injected authoritative failure');
                END
            """)
            repo._db.commit()
            payload = b"must-not-be-cached"
            manifest = make_manifest("/repo/rejected", payload)

            with self.assertRaises(sqlite3.DatabaseError):
                repo._persist_object(manifest, payload)

            status = repo._cache_status()
            self.assertEqual(status["entryCount"], 0)
            self.assertEqual(status["admissions"], 0)
            self.assertEqual(status["backingWrites"], 0)
            row = repo._db.execute(
                "SELECT 1 FROM objects WHERE object_name=?",
                (manifest.object_name,),
            ).fetchone()
            self.assertIsNone(row)
            repo._db.close()

    def test_cache_admission_memory_error_does_not_fail_committed_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            old_payload = b"old-cache-value"
            new_payload = b"new-authoritative-value"
            old_manifest = make_manifest("/repo/cache-memory-error", old_payload)
            new_manifest = make_manifest("/repo/cache-memory-error", new_payload)
            repo._persist_object(old_manifest, old_payload)
            original_cache_put = repo._cache_put

            def reject_cache_admission(_manifest, _payload) -> None:
                raise MemoryError("injected cache allocation failure")

            repo._cache_put = reject_cache_admission
            repo._persist_object(new_manifest, new_payload)
            repo._cache_put = original_cache_put

            self.assertIsNone(repo._cache_get(new_manifest.object_name))
            row = repo._db.execute(
                "SELECT payload FROM objects WHERE object_name=?",
                (new_manifest.object_name,),
            ).fetchone()
            self.assertIsNotNone(row)
            self.assertEqual(bytes(row[0]), new_payload)
            self.assertEqual(repo._cache_status()["backingWrites"], 2)
            repo._db.close()

    def test_concurrent_read_cannot_recache_value_older_than_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            object_name = "/repo/concurrent-overwrite"
            old_payload = b"old-value"
            new_payload = b"new-value"
            old_manifest = make_manifest(object_name, old_payload)
            new_manifest = make_manifest(object_name, new_payload)
            repo._persist_object(old_manifest, old_payload)
            repo._cache_invalidate(object_name)

            reader_at_admission = threading.Event()
            release_reader = threading.Event()
            writer_done = threading.Event()
            failures: list[BaseException] = []
            original_cache_put = repo._cache_put

            def blocking_cache_put(manifest, payload) -> None:
                if (threading.current_thread().name == "repo-stale-reader" and
                        bytes(payload) == old_payload):
                    reader_at_admission.set()
                    if not release_reader.wait(2.0):
                        raise TimeoutError("reader admission barrier timed out")
                original_cache_put(manifest, payload)

            repo._cache_put = blocking_cache_put

            def read_old() -> None:
                try:
                    repo._load_persisted_object(object_name)
                except BaseException as exc:  # noqa: BLE001
                    failures.append(exc)

            def write_new() -> None:
                try:
                    repo._persist_object(new_manifest, new_payload)
                except BaseException as exc:  # noqa: BLE001
                    failures.append(exc)
                finally:
                    writer_done.set()

            reader = threading.Thread(target=read_old, name="repo-stale-reader")
            writer = threading.Thread(target=write_new, name="repo-overwrite-writer")
            reader.start()
            self.assertTrue(reader_at_admission.wait(1.0))
            writer.start()
            self.assertFalse(
                writer_done.wait(0.05),
                "overwrite was not serialized with read-through admission",
            )
            release_reader.set()
            reader.join(2.0)
            writer.join(2.0)
            repo._cache_put = original_cache_put

            self.assertFalse(reader.is_alive())
            self.assertFalse(writer.is_alive())
            self.assertFalse(failures)
            self.assertEqual(repo._load_persisted_object(object_name)[1], new_payload)
            repo._db.close()

    def test_overwrite_delete_and_zero_budget_remain_sqlite_authoritative(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            database = Path(tmp) / "repo.sqlite3"
            old_payload = b"old-value"
            new_payload = b"new-value"
            old_manifest = make_manifest("/repo/change", old_payload)
            new_manifest = make_manifest("/repo/change", new_payload)
            repo = make_repo(database)
            repo._persist_object(old_manifest, old_payload)
            repo._persist_object(new_manifest, new_payload)
            self.assertEqual(repo._load_persisted_object(new_manifest.object_name)[1],
                             new_payload)
            self.assertTrue(repo._delete_object(new_manifest.object_name))
            with self.assertRaises(KeyError):
                repo._load_manifest(new_manifest.object_name)
            self.assertEqual(repo._cache_status()["entryCount"], 0)
            repo._db.close()

            zero = make_repo(database, budget=0)
            zero_payload = b"sqlite-without-memory-cache"
            zero_manifest = make_manifest("/repo/zero-budget", zero_payload)
            zero._persist_object(zero_manifest, zero_payload)
            self.assertEqual(zero._load_persisted_object(zero_manifest.object_name)[1],
                             zero_payload)
            status = zero._cache_status()
            self.assertEqual(status["authoritativeBackend"], "sqlite")
            self.assertEqual(status["cachePolicy"], "disabled")
            self.assertEqual(status["entryCount"], 0)
            self.assertEqual(status["backingWrites"], 1)
            self.assertEqual(status["backingReads"], 1)
            zero._db.close()

    def test_complete_packet_set_uses_same_budget_and_survives_restart(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            database = Path(tmp) / "repo.sqlite3"
            packets = make_segmented_data_packets(
                "/repo/packets",
                b"packet-cache-payload" * 20,
                signing_identity="/test/repo-tiered-cache",
                max_segment_size=96,
            )
            payload = b"".join(packet.wire for packet in packets)
            manifest = make_manifest("/repo/packets", payload, segments=len(packets))
            repo = make_repo(database, budget=8192)
            repo._persist_packets(manifest, packets)
            self.assertEqual(repo._cache_status()["entryCount"], 1)
            repo._db.close()

            restarted = make_repo(database, budget=8192)
            loaded_manifest, loaded_packets = restarted._load_persisted_packets(
                manifest.object_name)
            self.assertEqual(loaded_manifest.segment_count, len(packets))
            self.assertEqual([packet.wire for packet in loaded_packets],
                             [packet.wire for packet in packets])
            status = restarted._cache_status()
            self.assertEqual(status["misses"], 1)
            self.assertEqual(status["backingReads"], 1)
            self.assertEqual(status["entryCount"], 1)
            self.assertLessEqual(status["usedBytes"], status["budgetBytes"])
            restarted._db.close()


class NetworkRepoRestartFetchTest(unittest.TestCase):
    def test_cache_status_reuses_the_clients_svs_runtime(self) -> None:
        client = NetworkDistributedRepoClient.__new__(NetworkDistributedRepoClient)
        client.timeout_ms = 1000
        calls: list[dict] = []

        def request_specific_repo(**kwargs):
            calls.append(dict(kwargs))
            return SimpleNamespace(payload=b'{"authoritativeBackend":"sqlite"}')

        client._request_specific_repo = request_specific_repo
        status = client.cache_status("/repo/A")

        self.assertEqual(status["authoritativeBackend"], "sqlite")
        self.assertEqual(len(calls), 1)
        self.assertNotIn("isolated_runtime", calls[0])

    def test_segment_locations_are_prepared_before_segment_fetch(self) -> None:
        payload = b"restart-network-payload"
        data_name = "/repo/A/NDNSF-DISTRIBUTED-REPO/DATA/object"
        object_name = "/publisher/NDNSF-DISTRIBUTED-REPO/OBJECT/restart"
        manifest = RepoObjectManifest(
            object_name=object_name,
            object_type="tiered-cache-test",
            sha256=hashlib.sha256(payload).hexdigest(),
            size=len(payload),
            segment_count=1,
            replication_factor=1,
            min_replication_factor=1,
            max_replication_factor=1,
            replica_nodes=("/repo/A",),
            replica_data_names=(data_name,),
            segment_locations=({
                "repoNode": "/repo/A",
                "dataName": data_name,
                "start": 0,
                "end": 0,
                "hints": [],
                "routeStrategy": "direct-first",
            },),
        )
        client = NetworkDistributedRepoClient.__new__(NetworkDistributedRepoClient)
        client.timeout_ms = 1000
        events: list[str] = []

        def prepare(repo_node: str, requested_object: str,
                    expected_data_name: str = "") -> dict:
            events.append(f"prepare:{repo_node}:{requested_object}")
            self.assertEqual(expected_data_name, data_name)
            return {
                "dataName": data_name,
                "forwardingHints": [repo_node],
                "manifest": manifest.to_dict(),
            }

        def fetch(*_args, **_kwargs) -> bytes:
            events.append("fetch")
            return payload

        client._prepare_fetch_source = prepare
        with patch.object(
            repo_module,
            "fetch_segmented_object_with_segment_hints",
            side_effect=fetch,
        ):
            self.assertEqual(client.fetch_object(object_name, manifest), payload)

        self.assertEqual(events, [
            f"prepare:/repo/A:{object_name}",
            "fetch",
        ])


if __name__ == "__main__":
    unittest.main()
