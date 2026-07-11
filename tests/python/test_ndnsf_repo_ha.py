#!/usr/bin/env python3
"""High-availability and concurrency contracts for NDNSF-DistributedRepo."""

from __future__ import annotations

import hashlib
import json
from concurrent.futures import ThreadPoolExecutor
import sqlite3
import tempfile
import threading
import time
import unittest
import uuid
from pathlib import Path
from unittest.mock import patch

from ndnsf import make_segmented_data_packets

from py_repoclient.orchestration import (
    NetworkDistributedRepoClient,
    RepoIncompleteWriteError,
    RepoNodeApp,
    RepoObjectManifest,
    RepoWriteIntent,
    RepoWriteReceipt,
    StorageCapability,
    WriteConsistency,
    normalize_repo_operation_state,
    required_write_acks,
    validate_write_receipts,
    select_replicas,
    PlacementPolicy,
    _BoundedRepoHotCache,
    decode_repo_request,
    encode_repo_request,
)


class FakeRepoDataPlane:
    def __init__(self) -> None:
        self.prefixes: set[str] = set()
        self.started = False
        self.stopped = False

    def activate_prefix(self, prefix: str) -> None:
        self.prefixes.add(prefix)

    def start(self):
        self.started = True
        return self

    def stop(self) -> None:
        self.stopped = True

    @property
    def status(self) -> dict[str, object]:
        return {
            "activePrefixCount": len(self.prefixes),
            "threadCount": 1 if self.started and not self.stopped else 0,
        }


class ControlDispatcherTest(unittest.TestCase):
    def test_control_metrics_can_reset_after_warmup(self) -> None:
        class FakeUser:
            user = "/publisher"

            def request_service_targeted_async(
                self, provider, service, payload, *, on_response, on_timeout,
                timeout_ms,
            ) -> None:
                del provider, service, payload, on_timeout, timeout_ms
                on_response(type("Response", (), {
                    "status": True, "error": "", "payload": b"ok"})())

        client = NetworkDistributedRepoClient(
            user=FakeUser(), control_mode="targeted",
            enable_targeted_fallback=False)
        try:
            client._request_specific_repos_parallel({
                "/repo/A": encode_repo_request("STATUS")})
            self.assertGreater(client.control_metrics()["targetedCalls"], 0)
            client.reset_control_metrics()
            metrics = client.control_metrics()
            self.assertEqual(metrics["targetedCalls"], 0)
            self.assertEqual(metrics["targetedAsyncCompleted"], 0)
            self.assertEqual(metrics["replicaFanouts"], 0)
            self.assertEqual(metrics["controlMode"], "targeted")
        finally:
            client.close()

    def test_definitive_provider_failure_uses_stronger_cooldown(self) -> None:
        client = NetworkDistributedRepoClient.__new__(NetworkDistributedRepoClient)
        client._replica_health = {}
        client.replica_cooldown_ms = 100
        client.timeout_ms = 1000
        client._placement_cache = []

        now = client._now_ms()
        client._record_replica_result("/repo/A", success=False)
        transient_until = client._replica_health["/repo/A"]["cooldownUntilMs"]
        client._record_replica_result(
            "/repo/B", success=False, definitive_failure=True)
        definitive_until = client._replica_health["/repo/B"]["cooldownUntilMs"]

        self.assertGreaterEqual(transient_until - now, 90)
        self.assertGreaterEqual(definitive_until - now, 7_900)

    def test_concurrent_callers_enter_service_user_from_one_thread(self) -> None:
        class FakeUser:
            user = "/publisher"

            def __init__(self) -> None:
                self.thread_ids: set[int] = set()
                self.lock = threading.Lock()

            def request_service(self, *args, **kwargs):
                with self.lock:
                    self.thread_ids.add(threading.get_ident())
                time.sleep(0.002)
                return type("Response", (), {
                    "status": True,
                    "error": "",
                    "payload": b'{"repoNode":"/repo/A"}',
                })()

        user = FakeUser()
        client = NetworkDistributedRepoClient(user=user)
        try:
            with ThreadPoolExecutor(max_workers=8) as callers:
                results = list(callers.map(lambda _: client.capability(), range(24)))
            self.assertEqual(len(results), 24)
            self.assertEqual(len(user.thread_ids), 1)
        finally:
            client.close()

    def test_targeted_replica_calls_are_submitted_together_and_ordered(self) -> None:
        class FakeUser:
            user = "/publisher"

            def __init__(self) -> None:
                self.lock = threading.Lock()
                self.outstanding = 0
                self.max_outstanding = 0
                self.submit_threads: set[int] = set()

            def request_service_targeted_async(
                self, provider, service, payload, *, on_response, on_timeout,
                timeout_ms,
            ) -> None:
                del service, on_timeout, timeout_ms
                with self.lock:
                    self.submit_threads.add(threading.get_ident())
                    self.outstanding += 1
                    self.max_outstanding = max(
                        self.max_outstanding, self.outstanding)

                def finish() -> None:
                    with self.lock:
                        self.outstanding -= 1
                    on_response(type("Response", (), {
                        "status": True,
                        "error": "",
                        "payload": provider.encode() + b":" + payload,
                    })())

                delay = 0.01 if provider.endswith("B") else 0.04
                threading.Timer(delay, finish).start()

        user = FakeUser()
        client = NetworkDistributedRepoClient(
            user=user,
            control_mode="targeted",
            enable_targeted_fallback=False,
        )
        try:
            responses, failures = client._request_specific_repos_parallel({
                "/repo/A": encode_repo_request("STATUS", marker="A"),
                "/repo/B": encode_repo_request("STATUS", marker="B"),
            }, timeout_ms=500)
            self.assertEqual(failures, {})
            self.assertEqual(list(responses), ["/repo/A", "/repo/B"])
            self.assertEqual(user.max_outstanding, 2)
            self.assertEqual(len(user.submit_threads), 1)
            self.assertEqual(
                client.control_metrics()["maxConcurrentReplicaCalls"], 2)
        finally:
            client.close()

    def test_targeted_replica_deadline_preserves_successful_sibling(self) -> None:
        class FakeUser:
            user = "/publisher"

            def request_service_targeted_async(
                self, provider, service, payload, *, on_response, on_timeout,
                timeout_ms,
            ) -> None:
                del service, payload, on_timeout, timeout_ms
                if provider.endswith("A"):
                    threading.Timer(
                        0.01,
                        lambda: on_response(type("Response", (), {
                            "status": True,
                            "error": "",
                            "payload": b"A-ok",
                        })()),
                    ).start()

        client = NetworkDistributedRepoClient(
            user=FakeUser(),
            control_mode="targeted",
            enable_targeted_fallback=False,
        )
        started = time.monotonic()
        try:
            responses, failures = client._request_specific_repos_parallel({
                "/repo/A": encode_repo_request("STATUS", marker="A"),
                "/repo/B": encode_repo_request("STATUS", marker="B"),
            }, timeout_ms=80)
            elapsed = time.monotonic() - started
            self.assertEqual(responses["/repo/A"].payload, b"A-ok")
            self.assertIn("/repo/B", failures)
            self.assertLess(elapsed, 0.25)
            self.assertFalse(client._replica_in_cooldown("/repo/A"))
            self.assertTrue(client._replica_in_cooldown("/repo/B"))
        finally:
            client.close()

    def test_targeted_failure_uses_observable_normal_fallback(self) -> None:
        class FakeUser:
            user = "/publisher"

            def __init__(self) -> None:
                self.normal_calls = 0

            def request_service_targeted_async(
                self, provider, service, payload, *, on_response, on_timeout,
                timeout_ms,
            ) -> None:
                del provider, service, payload, on_response, timeout_ms
                on_timeout("/targeted/bootstrap-timeout")

            def request_service_select(self, *args, **kwargs):
                del args, kwargs
                self.normal_calls += 1
                return type("Response", (), {
                    "status": True,
                    "error": "",
                    "payload": b"normal-ok",
                })()

        user = FakeUser()
        client = NetworkDistributedRepoClient(
            user=user,
            control_mode="targeted",
            enable_targeted_fallback=True,
            timeout_ms=1000,
        )
        try:
            responses, failures = client._request_specific_repos_parallel(
                {"/repo/A": encode_repo_request("STATUS")}, timeout_ms=1000)
            self.assertEqual(failures, {})
            self.assertEqual(responses["/repo/A"].payload, b"normal-ok")
            self.assertEqual(user.normal_calls, 1)
            metrics = client.control_metrics()
            self.assertEqual(metrics["targetedFallbacks"], 1)
            self.assertEqual(metrics["normalCalls"], 1)
            self.assertFalse(client._replica_in_cooldown("/repo/A"))
        finally:
            client.close()

    def test_close_wakes_pending_targeted_fanout(self) -> None:
        class FakeUser:
            user = "/publisher"

            def request_service_targeted_async(self, *args, **kwargs) -> None:
                del args, kwargs

        client = NetworkDistributedRepoClient(
            user=FakeUser(),
            control_mode="targeted",
            enable_targeted_fallback=False,
            timeout_ms=5000,
        )
        finished = threading.Event()

        def invoke() -> None:
            client._request_specific_repos_parallel(
                {"/repo/A": encode_repo_request("STATUS")}, timeout_ms=5000)
            finished.set()

        thread = threading.Thread(target=invoke)
        thread.start()
        time.sleep(0.03)
        client.close()
        thread.join(timeout=0.5)
        self.assertTrue(finished.is_set())


def make_repo(database: Path, *, budget: int = 4096) -> RepoNodeApp:
    repo = RepoNodeApp.__new__(RepoNodeApp)
    repo.repo_node = "/repo/ha-test"
    repo.service_name = "/NDNSF/DistributedRepo"
    repo.provider_name = "/provider/ha-test"
    repo.capacity_bytes = 1024 * 1024
    repo.capability = StorageCapability(
        repo_node=repo.repo_node,
        free_bytes=repo.capacity_bytes,
        failure_domain="test-domain",
    )
    repo.memory_cache_bytes = budget
    repo._hot_cache = _BoundedRepoHotCache(budget)
    repo._cache_bytes = 0
    repo._db_lock = threading.RLock()
    repo._db = sqlite3.connect(database, check_same_thread=False)
    repo._data_plane = FakeRepoDataPlane()
    repo._init_sqlite()
    repo._catalog_lock = threading.RLock()
    repo._catalog_epoch = 0
    repo._catalog_changes = []
    repo._global_catalog = {}
    repo._repo_status = {}
    repo._peer_catalog_epochs = {}
    repo._catalog_stale_after_ms = 30_000
    repo._catalog_boot_id = uuid.uuid4().hex
    repo._catalog_sequence = 0
    repo._catalog_history_limit = 10_000
    repo._restore_catalog_state()
    return repo


class RepoContractTest(unittest.TestCase):
    def test_unspecified_repair_floor_defaults_to_replication_factor(self) -> None:
        common = {
            "object_name": "/publisher/replicated",
            "object_type": "generic",
            "sha256": hashlib.sha256(b"payload").hexdigest(),
            "size": 7,
            "replication_factor": 3,
        }
        self.assertEqual(
            RepoObjectManifest(**common).to_dict()["minReplicationFactor"], 3)
        self.assertEqual(
            RepoObjectManifest(
                **common, min_replication_factor=1
            ).to_dict()["minReplicationFactor"], 1)

    def test_legacy_manifest_decodes_with_committed_defaults(self) -> None:
        legacy = {
            "objectName": "/publisher/object",
            "objectType": "artifact",
            "sha256": hashlib.sha256(b"payload").hexdigest(),
            "size": 7,
            "replicationFactor": 2,
            "replicaNodes": ["/repo/A", "/repo/B"],
        }
        manifest = RepoObjectManifest.from_dict(legacy)

        self.assertEqual(manifest.generation, 0)
        self.assertEqual(manifest.parent_generation, -1)
        self.assertEqual(manifest.write_consistency, WriteConsistency.ALL.value)
        self.assertEqual(manifest.required_write_acks, 2)
        self.assertEqual(manifest.confirmed_replica_nodes, ("/repo/A", "/repo/B"))
        self.assertEqual(manifest.lifecycle_state, "COMMITTED")

    def test_versioned_manifest_round_trip(self) -> None:
        manifest = RepoObjectManifest(
            object_name="/publisher/versioned",
            object_type="mutable-alias",
            object_class="mutable-alias",
            sha256="ab" * 32,
            size=91,
            replication_factor=3,
            min_replication_factor=1,
            max_replication_factor=3,
            replica_nodes=("/repo/A", "/repo/B", "/repo/C"),
            generation=7,
            parent_generation=6,
            write_consistency=WriteConsistency.QUORUM.value,
            required_write_acks=2,
            confirmed_replica_nodes=("/repo/A", "/repo/C"),
            operation_id="op-version-7",
            lifecycle_state="COMMITTED",
        )
        parsed = RepoObjectManifest.from_dict(manifest.to_dict())
        self.assertEqual(parsed, manifest)

    def test_write_intent_and_receipt_validate_and_round_trip(self) -> None:
        intent = RepoWriteIntent(
            operation_id="op-1",
            object_name="/publisher/object",
            generation=1,
            expected_generation=0,
            digest="cd" * 32,
            replication_factor=3,
            required_acks=2,
            consistency=WriteConsistency.QUORUM.value,
            selected_replicas=("/repo/A", "/repo/B", "/repo/C"),
        )
        parsed_intent = RepoWriteIntent.from_dict(intent.to_dict())
        self.assertEqual(parsed_intent, intent)

        receipt = RepoWriteReceipt(
            operation_id=intent.operation_id,
            repo_node="/repo/A",
            object_name=intent.object_name,
            generation=intent.generation,
            digest=intent.digest,
            persisted_bytes=1234,
            state="COMMITTED",
            completed_at_ms=100,
        )
        self.assertEqual(
            RepoWriteReceipt.from_dict(receipt.to_dict()), receipt)

        with self.assertRaises(ValueError):
            RepoWriteIntent(
                operation_id="bad",
                object_name="/publisher/object",
                generation=1,
                digest="ef" * 32,
                replication_factor=2,
                required_acks=3,
                selected_replicas=("/repo/A", "/repo/B"),
            )

    def test_consistency_ack_thresholds_and_states(self) -> None:
        self.assertEqual(required_write_acks(3, WriteConsistency.ONE.value), 1)
        self.assertEqual(required_write_acks(3, WriteConsistency.QUORUM.value), 2)
        self.assertEqual(required_write_acks(3, WriteConsistency.ALL.value), 3)
        self.assertEqual(normalize_repo_operation_state("committed"), "COMMITTED")
        with self.assertRaises(ValueError):
            normalize_repo_operation_state("mystery")


class RepoSchemaMigrationTest(unittest.TestCase):
    EXPECTED_TABLES = {
        "repo_meta",
        "write_operations",
        "write_receipts",
        "serving_prefixes",
        "serving_packets",
        "catalog_journal",
        "catalog_tombstones",
        "peer_watermarks",
        "repo_membership",
        "repair_jobs",
        "capacity_reservations",
    }

    def test_ha_schema_is_idempotent_and_configures_sqlite(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            database = Path(tmp) / "repo.sqlite3"
            repo = make_repo(database)
            repo._init_sqlite()

            tables = {
                str(row[0]) for row in repo._db.execute(
                    "SELECT name FROM sqlite_master WHERE type='table'"
                ).fetchall()
            }
            self.assertTrue(self.EXPECTED_TABLES.issubset(tables))
            self.assertEqual(repo._db.execute("PRAGMA foreign_keys").fetchone()[0], 1)
            self.assertGreaterEqual(
                repo._db.execute("PRAGMA busy_timeout").fetchone()[0], 5000)
            schema_version = repo._db.execute(
                "SELECT value FROM repo_meta WHERE key='schema_version'"
            ).fetchone()
            self.assertEqual(schema_version, ("8",))
            repair_columns = {
                str(row[1]) for row in repo._db.execute(
                    "PRAGMA table_info(repair_jobs)").fetchall()
            }
            self.assertTrue({
                "available_replicas", "missing_replicas", "object_priority",
                "object_updated_at_ms",
            }.issubset(repair_columns))
            repo._db.close()

    def test_schema_v7_repair_jobs_upgrade_in_place(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            database = Path(tmp) / "repo.sqlite3"
            db = sqlite3.connect(database)
            db.execute("""
                CREATE TABLE repair_jobs (
                    repair_id TEXT PRIMARY KEY,
                    object_name TEXT NOT NULL,
                    generation INTEGER NOT NULL DEFAULT 0,
                    source_repo TEXT NOT NULL,
                    target_repo TEXT NOT NULL,
                    state TEXT NOT NULL,
                    attempts INTEGER NOT NULL DEFAULT 0,
                    next_attempt_ms INTEGER NOT NULL DEFAULT 0,
                    lease_owner TEXT NOT NULL DEFAULT '',
                    lease_deadline_ms INTEGER NOT NULL DEFAULT 0,
                    result_json TEXT NOT NULL DEFAULT '{}'
                )
            """)
            db.execute("""
                INSERT INTO repair_jobs
                  (repair_id, object_name, source_repo, target_repo, state,
                   result_json)
                VALUES ('legacy-repair', '/publisher/legacy-repair',
                        '/repo/B', '/repo/A', 'PENDING', '{}')
            """)
            db.commit()
            db.close()

            repo = make_repo(database)
            row = repo._db.execute("""
                SELECT repair_id, available_replicas, missing_replicas,
                       object_priority, object_updated_at_ms
                FROM repair_jobs WHERE repair_id='legacy-repair'
            """).fetchone()
            self.assertEqual(row, ("legacy-repair", 0, 1, 0, 0))
            repo._db.close()


class ConfirmedWriteTest(unittest.TestCase):
    def test_network_store_response_contains_durable_receipt(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            payload = b"network-write"
            manifest = RepoObjectManifest(
                object_name="/publisher/network",
                object_type="artifact",
                sha256=hashlib.sha256(payload).hexdigest(),
                size=len(payload),
                replica_nodes=(repo.repo_node,),
                operation_id="network-op",
            )
            intent = RepoWriteIntent(
                operation_id=manifest.operation_id,
                object_name=manifest.object_name,
                generation=0,
                digest=manifest.sha256,
                replication_factor=1,
                selected_replicas=(repo.repo_node,),
            )
            request = encode_repo_request(
                "STORE",
                manifest=manifest.to_dict(),
                writeIntent=intent.to_dict(),
                payloadB64=__import__("base64").b64encode(payload).decode(),
            )

            first = repo._handle(request)
            replay = repo._handle(request)
            self.assertTrue(first.status)
            first_obj = json.loads(first.payload.decode())
            replay_obj = json.loads(replay.payload.decode())
            self.assertEqual(first_obj["writeReceipt"], replay_obj["writeReceipt"])
            self.assertEqual(first_obj["writeReceipt"]["state"], "COMMITTED")
            self.assertEqual(repo._db.execute(
                "SELECT COUNT(*) FROM write_receipts WHERE operation_id='network-op'"
            ).fetchone()[0], 1)
            repo._db.close()


class AlwaysOnDataPlaneTest(unittest.TestCase):
    def test_repeated_packet_prepare_uses_one_long_lived_data_plane(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            payload = b"exact-data-plane" * 300
            packets = make_segmented_data_packets(
                "/data/exact/v=9", payload,
                signing_identity="/test/repo/data-plane",
                max_segment_size=1000,
            )
            manifest = RepoObjectManifest(
                object_name="/publisher/exact-data-plane",
                object_type="ndn-data-wire",
                sha256=hashlib.sha256(payload).hexdigest(),
                size=len(payload),
                segment_count=len(packets),
                packet_names=tuple(packet.name for packet in packets),
                replica_nodes=(repo.repo_node,),
            )
            repo._persist_packets(manifest, packets)

            for _ in range(25):
                repo._serve_packets(packets)

            self.assertEqual(repo._data_plane.status["threadCount"], 0)
            self.assertLessEqual(len(repo._data_plane.prefixes), 2)
            self.assertEqual(repo._lookup_data_plane_wire(packets[0].name, False),
                             packets[0].wire)
            parent = packets[0].name.rsplit("/", 1)[0]
            self.assertEqual(repo._lookup_data_plane_wire(parent, True),
                             packets[0].wire)
            self.assertEqual(repo._db.execute(
                "SELECT COUNT(*) FROM serving_prefixes WHERE active=1"
            ).fetchone()[0], len(repo._data_plane.prefixes))
            repo._db.close()

    def test_opaque_serving_packets_are_created_once_and_restored(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            database = Path(tmp) / "repo.sqlite3"
            repo = make_repo(database)
            payload = b"opaque-data-plane" * 800
            manifest = RepoObjectManifest(
                object_name="/publisher/opaque",
                object_type="opaque",
                sha256=hashlib.sha256(payload).hexdigest(),
                size=len(payload),
                replica_nodes=(repo.repo_node,),
            )
            repo._persist_object(manifest, payload)
            data_name = repo.data_name(repo.repo_node, manifest.object_name)
            repo._serve_object(data_name, payload, manifest.object_name)
            first_names = tuple(row[0] for row in repo._db.execute(
                "SELECT data_name FROM serving_packets ORDER BY data_name"
            ).fetchall())
            repo._serve_object(data_name, payload, manifest.object_name)
            second_names = tuple(row[0] for row in repo._db.execute(
                "SELECT data_name FROM serving_packets ORDER BY data_name"
            ).fetchall())
            self.assertEqual(first_names, second_names)
            self.assertTrue(first_names)
            expected_prefixes = set(repo._data_plane.prefixes)
            repo._db.close()

            restarted = make_repo(database)
            restarted._restore_serving_prefixes()
            self.assertEqual(restarted._data_plane.prefixes, expected_prefixes)
            self.assertIsNotNone(
                restarted._lookup_data_plane_wire(first_names[0], False))
            restarted._db.close()


class ConcurrentStorageTest(unittest.TestCase):
    def test_read_path_does_not_wait_for_global_writer_lock(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3", budget=0)
            payload = b"parallel-read"
            manifest = RepoObjectManifest(
                object_name="/publisher/parallel",
                object_type="artifact",
                sha256=hashlib.sha256(payload).hexdigest(),
                size=len(payload),
            )
            repo._persist_object(manifest, payload)
            completed = threading.Event()
            observed: list[bytes] = []

            with repo._db_lock:
                worker = threading.Thread(
                    target=lambda: (
                        observed.append(repo._load_persisted_object(manifest.object_name)[1]),
                        completed.set(),
                    )
                )
                worker.start()
                self.assertTrue(completed.wait(1.0))
            worker.join(timeout=1.0)
            self.assertEqual(observed, [payload])
            repo._db.close()


class DurableCatalogRepairTest(unittest.TestCase):
    @staticmethod
    def _manifest(name: str, payload: bytes, *, min_replicas: int = 1,
                  generation: int = 0) -> RepoObjectManifest:
        return RepoObjectManifest(
            object_name=name,
            object_type="artifact",
            sha256=hashlib.sha256(payload).hexdigest(),
            size=len(payload),
            generation=generation,
            min_replication_factor=min_replicas,
            max_replication_factor=max(1, min_replicas),
            replication_factor=max(1, min_replicas),
        )

    def test_catalog_journal_tombstone_and_membership_survive_restart(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            database = Path(tmp) / "repo.sqlite3"
            repo = make_repo(database)
            manifest = self._manifest("/publisher/durable", b"payload")
            repo._persist_object(manifest, b"payload")
            first_boot = repo._catalog_boot_id
            repo._merge_catalog_entries([], {
                "repoNode": "/repo/peer",
                "bootId": "peer-boot",
                "sourceSequence": 9,
                "repoMode": "persistent",
            })
            repo._delete_object(manifest.object_name)
            sequence = repo._catalog_sequence
            self.assertGreaterEqual(sequence, 2)
            self.assertEqual(repo._db.execute(
                "SELECT COUNT(*) FROM catalog_tombstones"
            ).fetchone()[0], 1)
            repo._db.close()

            restarted = make_repo(database)
            self.assertNotEqual(restarted._catalog_boot_id, first_boot)
            self.assertEqual(restarted._catalog_sequence, sequence)
            self.assertIn(manifest.object_name, restarted._global_catalog)
            self.assertEqual(
                restarted._global_catalog[manifest.object_name][repo.repo_node]["state"],
                "DELETED",
            )
            self.assertIn("/repo/peer", restarted._repo_status)
            restarted._db.close()

    def test_capacity_reservations_prevent_oversubscription_and_are_consumed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            repo.capacity_bytes = 100
            first = repo._reserve_capacity("reserve-1", "op-1", 80, 10_000)
            self.assertEqual(first.state, "RESERVED")
            with self.assertRaisesRegex(RuntimeError, "repo-capacity-rejected"):
                repo._reserve_capacity("reserve-2", "op-2", 30, 10_000)
            payload = b"x" * 20
            manifest = RepoObjectManifest(
                object_name="/publisher/reserved",
                object_type="artifact",
                sha256=hashlib.sha256(payload).hexdigest(),
                size=len(payload),
                operation_id="op-1",
            )
            intent = RepoWriteIntent(
                operation_id="op-1",
                object_name=manifest.object_name,
                generation=0,
                digest=manifest.sha256,
                replication_factor=1,
            )
            repo._persist_object(manifest, payload, intent=intent)
            self.assertEqual(repo._db.execute(
                "SELECT state FROM capacity_reservations WHERE reservation_id='reserve-1'"
            ).fetchone()[0], "CONSUMED")
            repo._db.close()

    def test_runtime_and_network_metrics_affect_placement(self) -> None:
        slow = StorageCapability(
            repo_node="/repo/slow", free_bytes=10_000_000,
            queue_depth=20, inflight_operations=10,
            storage_latency_ms=100, network_rtt_ms=80,
            network_bandwidth_mbps=10,
        )
        fast = StorageCapability(
            repo_node="/repo/fast", free_bytes=10_000_000,
            queue_depth=0, inflight_operations=0,
            storage_latency_ms=2, network_rtt_ms=4,
            network_bandwidth_mbps=1000,
        )
        selected = select_replicas(
            [slow, fast], PlacementPolicy(replication_factor=1), 100)
        self.assertEqual(selected[0].repo_node, "/repo/fast")

    def test_placement_ttl_and_failure_cooldown_invalidate_cached_node(self) -> None:
        client = NetworkDistributedRepoClient.__new__(NetworkDistributedRepoClient)
        client._placement_cache = ["/repo/A"]
        client._placement_cache_updated_ms = int(time.time() * 1000)
        client.placement_cache_ttl_ms = 1000
        client.replica_cooldown_ms = 1000
        client._replica_health = {}
        self.assertTrue(client._placement_cache_valid(1))
        client._record_replica_result("/repo/A", success=False)
        self.assertFalse(client._placement_cache_valid(1))
        self.assertEqual(client._ordered_replicas(["/repo/A", "/repo/B"])[0],
                         "/repo/B")

    def test_packet_failover_restarts_whole_set_and_records_health(self) -> None:
        client = NetworkDistributedRepoClient.__new__(NetworkDistributedRepoClient)
        client.timeout_ms = 1000
        client.replica_cooldown_ms = 1000
        client._replica_health = {}
        client._placement_cache = []
        packets = make_segmented_data_packets(
            "/data/failover/v=1", b"payload" * 100,
            signing_identity="/test/failover", max_segment_size=300)
        manifest = RepoObjectManifest(
            object_name="/publisher/failover",
            object_type="ndn-data-wire",
            sha256="aa" * 32,
            size=700,
            segment_count=len(packets),
            packet_names=tuple(packet.name for packet in packets),
            replica_nodes=("/repo/dead", "/repo/live"),
        )
        calls: list[tuple[str, str]] = []

        def fetch(repo_node: str, data_name: str):
            calls.append((repo_node, data_name))
            if repo_node == "/repo/dead":
                raise TimeoutError("dead replica")
            return next(packet for packet in packets if packet.name == data_name)

        client.fetch_packet = fetch
        fetched = client.fetch_signed_packets(manifest)
        self.assertEqual([packet.wire for packet in fetched],
                         [packet.wire for packet in packets])
        self.assertEqual(calls[0][0], "/repo/dead")
        self.assertEqual(calls[1][0], "/repo/live")
        self.assertGreater(client._replica_health["/repo/dead"]["failures"], 0)

    def test_bucket_digest_is_deterministic_and_conflict_is_explicit(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            now = repo._now_ms()
            base = {
                "objectName": "/publisher/conflict",
                "objectType": "artifact",
                "generation": 4,
                "state": "AVAILABLE",
                "manifestSha256": "manifest",
                "updatedAtMs": now,
                "minReplicationFactor": 1,
                "maxReplicationFactor": 2,
                "repairAllowed": True,
            }
            repo._merge_catalog_entries([
                {**base, "sourceRepo": "/repo/A", "sourceBootId": "A1",
                 "sourceSequence": 1, "objectSha256": "aa" * 32},
                {**base, "sourceRepo": "/repo/B", "sourceBootId": "B1",
                 "sourceSequence": 1, "objectSha256": "bb" * 32},
            ])
            first = repo._catalog_bucket_digest(8)
            second = repo._catalog_bucket_digest(8)
            self.assertEqual(first["digests"], second["digests"])
            summary = repo._object_catalog_summary(base["objectName"])
            self.assertEqual(summary["state"], "CONFLICT")
            self.assertFalse(summary["eligibleForRepair"])
            self.assertEqual(len(summary["conflictingDigests"]), 2)
            repo._db.close()

    def test_repair_jobs_are_leased_retried_and_recur_after_later_loss(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            payload = b"repairable"
            manifest = self._manifest(
                "/publisher/repairable", payload, min_replicas=2)
            repo._persist_object(manifest, payload)
            repo._merge_repo_status({
                "repoNode": "/repo/target",
                "bootId": "target-1",
                "sourceSequence": 1,
                "repoMode": "persistent",
                "acceptsBackupReplica": True,
            })
            scan = repo._scan_repair_jobs()
            self.assertEqual(scan["createdCount"], 1)
            job = repo._claim_repair_job("test-worker", 1000, "/repo/target")
            self.assertIsNotNone(job)
            self.assertEqual(job["state"], "RUNNING")
            failed = repo._finish_repair_job(
                job["repairId"], success=False, error="temporary")
            self.assertEqual(failed["state"], "RETRY")
            repo._db.execute(
                "UPDATE repair_jobs SET next_attempt_ms=0 WHERE repair_id=?",
                (job["repairId"],),
            )
            repo._db.commit()
            retry = repo._claim_repair_job("test-worker", 1000, "/repo/target")
            repo._finish_repair_job(retry["repairId"], success=True)

            target_entry = repo._catalog_entry(manifest, "AVAILABLE")
            target_entry.update({
                "sourceRepo": "/repo/target",
                "sourceBootId": "target-1",
                "sourceSequence": 10,
                "catalogEpoch": 10,
            })
            repo._merge_catalog_entries([target_entry], {
                "repoNode": "/repo/target",
                "bootId": "target-1",
                "sourceSequence": 10,
                "repoMode": "persistent",
                "acceptsBackupReplica": True,
            })
            repo._repo_status["/repo/target"]["lastSeenMs"] = (
                repo._now_ms() - repo._catalog_stale_after_ms - 1)
            repo._merge_repo_status({
                "repoNode": "/repo/target2",
                "bootId": "target-2",
                "sourceSequence": 1,
                "repoMode": "persistent",
                "acceptsBackupReplica": True,
            })
            recurrence = repo._scan_repair_jobs()
            self.assertEqual(recurrence["createdCount"], 1)
            self.assertNotEqual(recurrence["created"][0], job["repairId"])
            repo._db.close()

    def test_recovered_repo_is_selected_for_degraded_rf3_object(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            payload = b"written-while-repo-a-was-offline"
            manifest = RepoObjectManifest(
                object_name="/publisher/degraded-rf3",
                object_type="artifact",
                sha256=hashlib.sha256(payload).hexdigest(),
                size=len(payload),
                replication_factor=3,
                min_replication_factor=3,
                max_replication_factor=3,
                replica_nodes=(repo.repo_node, "/repo/C"),
                confirmed_replica_nodes=(repo.repo_node, "/repo/C"),
                required_write_acks=2,
                write_consistency=WriteConsistency.QUORUM.value,
            )
            repo._persist_object(manifest, payload)
            repo_c_entry = repo._catalog_entry(manifest, "AVAILABLE")
            repo_c_entry.update({
                "sourceRepo": "/repo/C",
                "sourceBootId": "repo-c-boot",
                "sourceSequence": 7,
                "catalogEpoch": 7,
            })
            repo._merge_catalog_entries([repo_c_entry], {
                "repoNode": "/repo/C",
                "bootId": "repo-c-boot",
                "sourceSequence": 7,
                "repoMode": "persistent",
                "acceptsBackupReplica": True,
            })
            repo._merge_repo_status({
                "repoNode": "/repo/A",
                "bootId": "repo-a-recovered",
                "sourceSequence": 1,
                "repoMode": "persistent",
                "acceptsBackupReplica": True,
            })

            summary = repo._object_catalog_summary(manifest.object_name)

            self.assertEqual(summary["state"], "UNDER_REPLICATED")
            self.assertEqual(summary["availableReplicaCount"], 2)
            self.assertEqual(summary["repairPlan"]["missingReplicas"], 1)
            self.assertEqual(summary["repairPlan"]["targetCandidates"], ["/repo/A"])
            self.assertEqual(
                summary["repairPlan"]["actions"][0]["targetRepo"], "/repo/A")
            scan = repo._scan_repair_jobs()
            self.assertEqual(scan["createdCount"], 1)
            job = repo._claim_repair_job(
                "recovery-worker", 1000, target_repo="/repo/A")
            self.assertIsNotNone(job)
            self.assertEqual(job["targetRepo"], "/repo/A")
            repo._db.close()

    def test_repair_claim_prioritizes_risk_priority_age_and_backoff(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            now_ms = repo._now_ms()
            jobs = (
                ("safer", 2, 1, 100, 10),
                ("critical-low", 1, 2, 1, 20),
                ("critical-high-new", 1, 2, 9, 30),
                ("critical-high-old", 1, 2, 9, 5),
                ("backoff", 0, 3, 99, 1),
            )
            for repair_id, available, missing, priority, updated_ms in jobs:
                repo._db.execute("""
                    INSERT INTO repair_jobs
                      (repair_id, object_name, generation, source_repo,
                       target_repo, state, attempts, next_attempt_ms,
                       lease_owner, lease_deadline_ms, result_json,
                       available_replicas, missing_replicas, object_priority,
                       object_updated_at_ms)
                    VALUES (?, ?, 0, '/repo/B', '/repo/A', 'PENDING', 0,
                            ?, '', 0, '{}', ?, ?, ?, ?)
                """, (
                    repair_id, f"/publisher/{repair_id}",
                    now_ms + 60_000 if repair_id == "backoff" else 0,
                    available, missing, priority, updated_ms,
                ))
            repo._db.commit()

            claimed = []
            for _ in range(4):
                job = repo._claim_repair_job(
                    "priority-worker", 1000, target_repo="/repo/A")
                self.assertIsNotNone(job)
                claimed.append(job)
                repo._finish_repair_job(job["repairId"], success=True)

            self.assertEqual(
                [job["repairId"] for job in claimed],
                ["critical-high-old", "critical-high-new",
                 "critical-low", "safer"],
            )
            self.assertEqual(claimed[0]["availableReplicas"], 1)
            self.assertEqual(claimed[0]["missingReplicas"], 2)
            self.assertEqual(claimed[0]["objectPriority"], 9)
            self.assertEqual(claimed[0]["objectUpdatedAtMs"], 5)
            self.assertIsNone(repo._claim_repair_job(
                "priority-worker", 1000, target_repo="/repo/A"))
            repo._db.close()

    def test_repair_scan_reports_state_and_target_claimability(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            now_ms = repo._now_ms()
            jobs = (
                ("local-ready", repo.repo_node, "PENDING", 0),
                ("local-backoff", repo.repo_node, "RETRY", now_ms + 60_000),
                ("other-ready", "/repo/B", "PENDING", 0),
                ("local-running", repo.repo_node, "RUNNING", 0),
            )
            for repair_id, target, state, next_attempt_ms in jobs:
                repo._db.execute("""
                    INSERT INTO repair_jobs
                      (repair_id, object_name, generation, source_repo,
                       target_repo, state, attempts, next_attempt_ms,
                       lease_owner, lease_deadline_ms, result_json,
                       available_replicas, missing_replicas, object_priority,
                       object_updated_at_ms)
                    VALUES (?, ?, 0, '/repo/source', ?, ?, 0, ?, '', 0, '{}',
                            1, 1, 0, ?)
                """, (
                    repair_id, f"/publisher/{repair_id}", target, state,
                    next_attempt_ms, now_ms,
                ))
            repo._db.commit()

            scan = repo._scan_repair_jobs()

            self.assertEqual(scan["jobCount"], 4)
            self.assertEqual(scan["claimableCount"], 1)
            self.assertEqual(scan["targetRepo"], repo.repo_node)
            self.assertEqual(scan["earliestRetryMs"], now_ms + 60_000)
            self.assertEqual(scan["stateCounts"], {
                "PENDING": 2, "RETRY": 1, "RUNNING": 1})
            repo._db.close()

    def test_catalog_repair_skips_known_missing_target_probe(self) -> None:
        client = NetworkDistributedRepoClient.__new__(NetworkDistributedRepoClient)
        client.user = type("User", (), {"user": "/repair/user"})()
        client.timeout_ms = 1000
        client.upload_prefix = "/repair/user/NDNSF-DISTRIBUTED-REPO/UPLOAD"
        payload = b"repair-fast-path"
        packets = make_segmented_data_packets(
            "/data/repair-fast-path/v=1",
            payload,
            signing_identity="/test/repo/repair-fast-path",
            max_segment_size=1500,
        )
        manifest = RepoObjectManifest(
            object_name="/publisher/repair-fast-path",
            object_type="artifact",
            sha256=hashlib.sha256(payload).hexdigest(),
            size=len(payload),
            segment_count=len(packets),
            replication_factor=3,
            min_replication_factor=3,
            max_replication_factor=3,
            replica_nodes=("/repo/B", "/repo/C"),
            lifecycle_state="COMMITTED",
        )
        operations = []

        def response(value: dict):
            return type("Response", (), {
                "status": True,
                "error": "",
                "payload": json.dumps(value).encode(),
            })()

        def request_specific_repo(*, repo_node: str, payload: bytes, **_kwargs):
            request = decode_repo_request(payload)
            operations.append((repo_node, request["operation"]))
            if request["operation"] == "FETCH_PREPARE":
                self.assertEqual(repo_node, "/repo/B")
                return response({
                    "manifest": manifest.to_dict(),
                    "dataName": "/data/repair-fast-path/v=1",
                })
            self.assertEqual(request["operation"], "STORE_PACKET_PULL")
            self.assertEqual(repo_node, "/repo/A")
            self.assertIn("repairAuthorization", request)
            return response({
                "catalogEntry": {
                    "objectName": manifest.object_name,
                    "sourceRepo": "/repo/A",
                }
            })

        class FakeProducer:
            versioned_name = "/repair/packet-manifest/v=1"

            def start(self):
                return self

            def stop(self):
                pass

        client._request_specific_repo = request_specific_repo
        action = {
            "objectName": manifest.object_name,
            "objectSha256": manifest.sha256,
            "manifestSha256": "manifest-digest",
            "sourceRepo": "/repo/B",
            "targetRepo": "/repo/A",
            "minReplicationFactor": 3,
            "maxReplicationFactor": 3,
        }
        with patch(
                "py_repoclient.orchestration.fetch_segmented_object",
                return_value=payload), patch(
                "py_repoclient.orchestration.fetch_segmented_data_packets",
                return_value=packets), patch(
                "py_repoclient.orchestration.SegmentedObjectProducer",
                return_value=FakeProducer()), patch(
                "py_repoclient.orchestration.time.sleep"):
            first = client.catalog_repair("/repo/A", action)
            second = client.catalog_repair("/repo/A", action)

        self.assertEqual(first["status"], "repaired")
        self.assertEqual(second["status"], "repaired")
        self.assertEqual(operations, [
            ("/repo/B", "FETCH_PREPARE"),
            ("/repo/A", "STORE_PACKET_PULL"),
            ("/repo/B", "FETCH_PREPARE"),
            ("/repo/A", "STORE_PACKET_PULL"),
        ])

    def test_catalog_merge_pull_validates_integrity_and_limits(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            merge_payload = json.dumps({
                "schemaVersion": 1,
                "entries": [],
                "sourceStatus": {
                    "repoNode": "/repo/B",
                    "bootId": "repo-b-boot",
                    "sourceSequence": 1,
                    "repoMode": "persistent",
                    "acceptsBackupReplica": True,
                },
            }, sort_keys=True, separators=(",", ":")).encode()
            digest = hashlib.sha256(merge_payload).hexdigest()
            request = encode_repo_request(
                "CATALOG_MERGE_PULL",
                schemaVersion=1,
                sourceName="/repo/B/catalog/v=1",
                payloadSha256=digest,
                payloadBytes=len(merge_payload),
                entryCount=0,
            )
            with patch(
                    "py_repoclient.orchestration.fetch_segmented_object",
                    return_value=merge_payload) as fetch:
                accepted = repo._handle(request)
            self.assertTrue(accepted.status)
            self.assertEqual(json.loads(accepted.payload.decode())["mode"], "pull")
            fetch.assert_called_once()

            with patch(
                    "py_repoclient.orchestration.fetch_segmented_object",
                    return_value=merge_payload):
                bad_hash = repo._handle(encode_repo_request(
                    "CATALOG_MERGE_PULL", schemaVersion=1,
                    sourceName="/repo/B/catalog/v=1",
                    payloadSha256="00" * 32,
                    payloadBytes=len(merge_payload), entryCount=0))
                bad_count = repo._handle(encode_repo_request(
                    "CATALOG_MERGE_PULL", schemaVersion=1,
                    sourceName="/repo/B/catalog/v=1",
                    payloadSha256=digest,
                    payloadBytes=len(merge_payload), entryCount=1))
            self.assertFalse(bad_hash.status)
            self.assertIn("hash mismatch", bad_hash.error)
            self.assertFalse(bad_count.status)
            self.assertIn("entryCount mismatch", bad_count.error)

            with patch(
                    "py_repoclient.orchestration.fetch_segmented_object") as fetch:
                oversized = repo._handle(encode_repo_request(
                    "CATALOG_MERGE_PULL", schemaVersion=1,
                    sourceName="/repo/B/catalog/v=1",
                    payloadSha256=digest,
                    payloadBytes=16 * 1024 * 1024 + 1,
                    entryCount=0))
            self.assertFalse(oversized.status)
            self.assertIn("outside allowed range", oversized.error)
            fetch.assert_not_called()
            repo._db.close()

    def test_quorum_write_is_staged_until_receipt_backed_finalize(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            payload = b"quorum-finalization-boundary"
            manifest = RepoObjectManifest(
                object_name="/publisher/quorum-finalize",
                object_type="artifact",
                sha256=hashlib.sha256(payload).hexdigest(),
                size=len(payload),
                replication_factor=2,
                min_replication_factor=2,
                max_replication_factor=2,
                replica_nodes=(repo.repo_node, "/repo/B"),
                write_consistency=WriteConsistency.QUORUM.value,
                required_write_acks=2,
                operation_id="quorum-finalize-op",
                lifecycle_state="RUNNING",
            )
            intent = RepoWriteIntent(
                operation_id=manifest.operation_id,
                object_name=manifest.object_name,
                generation=0,
                digest=manifest.sha256,
                replication_factor=2,
                required_acks=2,
                consistency=WriteConsistency.QUORUM.value,
                selected_replicas=manifest.replica_nodes,
            )
            staged_manifest = repo._manifest_for_write_intent(manifest, intent)
            local_receipt = repo._persist_object(
                staged_manifest, payload, intent=intent)

            staged_summary = repo._object_catalog_summary(manifest.object_name)
            self.assertEqual(staged_manifest.lifecycle_state, "RUNNING")
            self.assertEqual(staged_summary["state"], "STAGED")
            self.assertFalse(staged_summary["eligibleForRepair"])
            self.assertEqual(repo._scan_repair_jobs()["createdCount"], 0)
            self.assertFalse(repo._handle(encode_repo_request(
                "FETCH", objectName=manifest.object_name)).status)
            self.assertFalse(repo._handle(encode_repo_request(
                "FETCH_PREPARE", objectName=manifest.object_name)).status)
            self.assertFalse(repo._handle(encode_repo_request(
                "MANIFEST", objectName=manifest.object_name)).status)
            inventory = json.loads(repo._handle(encode_repo_request(
                "INVENTORY")).payload.decode())
            self.assertNotIn(manifest.object_name, inventory)
            snapshot_entries = repo._catalog_snapshot()["entries"]
            self.assertEqual(
                next(entry["state"] for entry in snapshot_entries
                     if entry["objectName"] == manifest.object_name),
                "STAGED",
            )

            peer_receipt = RepoWriteReceipt(
                operation_id=intent.operation_id,
                repo_node="/repo/B",
                object_name=intent.object_name,
                generation=intent.generation,
                digest=intent.digest,
                persisted_bytes=len(payload),
                state="COMMITTED",
                completed_at_ms=repo._now_ms(),
            )
            finalized = repo._finalize_write(
                manifest,
                intent,
                (local_receipt, peer_receipt),
            )

            self.assertEqual(finalized.lifecycle_state, "COMMITTED")
            self.assertEqual(
                finalized.confirmed_replica_nodes, (repo.repo_node, "/repo/B"))
            available_summary = repo._object_catalog_summary(manifest.object_name)
            self.assertEqual(available_summary["state"], "UNDER_REPLICATED")
            self.assertTrue(available_summary["eligibleForRepair"])
            self.assertTrue(repo._handle(encode_repo_request(
                "FETCH", objectName=manifest.object_name)).status)
            self.assertTrue(repo._data_plane.prefixes)
            repo._db.close()

    def test_finalize_write_rejects_incomplete_receipt_set(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            payload = b"incomplete-finalization"
            manifest = RepoObjectManifest(
                object_name="/publisher/incomplete-finalize",
                object_type="artifact",
                sha256=hashlib.sha256(payload).hexdigest(),
                size=len(payload),
                replication_factor=2,
                min_replication_factor=2,
                max_replication_factor=2,
                replica_nodes=(repo.repo_node, "/repo/B"),
                write_consistency=WriteConsistency.QUORUM.value,
                required_write_acks=2,
                operation_id="incomplete-finalize-op",
                lifecycle_state="RUNNING",
            )
            intent = RepoWriteIntent(
                operation_id=manifest.operation_id,
                object_name=manifest.object_name,
                generation=0,
                digest=manifest.sha256,
                replication_factor=2,
                required_acks=2,
                consistency=WriteConsistency.QUORUM.value,
                selected_replicas=manifest.replica_nodes,
            )
            staged_manifest = repo._manifest_for_write_intent(manifest, intent)
            local_receipt = repo._persist_object(
                staged_manifest, payload, intent=intent)

            response = repo._handle(encode_repo_request(
                "FINALIZE_WRITE",
                manifest=manifest.to_dict(),
                writeIntent=intent.to_dict(),
                writeReceipts=[local_receipt.to_dict()],
            ))

            self.assertFalse(response.status)
            self.assertIn("repo-write-incomplete", response.error)
            summary = repo._object_catalog_summary(manifest.object_name)
            self.assertEqual(summary["state"], "STAGED")
            self.assertFalse(summary["eligibleForRepair"])
            self.assertEqual(repo._scan_repair_jobs()["createdCount"], 0)
            repo._db.close()

    def test_ineligible_recovered_repos_are_not_repair_targets(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            payload = b"repair-target-filtering"
            manifest = RepoObjectManifest(
                object_name="/publisher/target-filtering",
                object_type="artifact",
                sha256=hashlib.sha256(payload).hexdigest(),
                size=len(payload),
                replication_factor=3,
                min_replication_factor=3,
                max_replication_factor=3,
            )
            repo._persist_object(manifest, payload)
            for repo_node, mode in (
                    ("/repo/stale", "persistent"),
                    ("/repo/ephemeral", "ephemeral")):
                repo._merge_repo_status({
                    "repoNode": repo_node,
                    "bootId": repo_node.rsplit("/", 1)[-1],
                    "sourceSequence": 1,
                    "repoMode": mode,
                    "acceptsBackupReplica": True,
                })
            repo._repo_status["/repo/stale"]["lastSeenMs"] = (
                repo._now_ms() - repo._catalog_stale_after_ms - 1)

            summary = repo._object_catalog_summary(manifest.object_name)

            self.assertEqual(summary["repairPlan"]["targetCandidates"], [])
            self.assertEqual(
                summary["repairPlan"]["reason"], "insufficient-live-targets")

            owning_entry = repo._catalog_entry(manifest, "AVAILABLE")
            owning_entry.update({
                "sourceRepo": "/repo/owner",
                "sourceBootId": "owner-boot",
                "sourceSequence": 2,
            })
            repo._merge_catalog_entries([owning_entry], {
                "repoNode": "/repo/owner",
                "bootId": "owner-boot",
                "sourceSequence": 2,
                "repoMode": "persistent",
                "acceptsBackupReplica": True,
            })
            owner_summary = repo._object_catalog_summary(manifest.object_name)
            self.assertNotIn(
                "/repo/owner", owner_summary["repairPlan"]["targetCandidates"])
            repo._db.close()

    def test_scrub_reports_clean_object(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            payload = b"scrubbed"
            repo._persist_object(self._manifest("/publisher/scrub", payload), payload)
            result = repo._scrub(10)
            self.assertEqual(result["checked"], 1)
            self.assertEqual(result["corruptCount"], 0)
            repo._db.close()

    def test_authenticated_requester_cannot_overwrite_another_publisher(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            payload = b"owned"
            object_name = (
                "/alice/NDNSF-DISTRIBUTED-REPO/OBJECT/private")
            manifest = RepoObjectManifest(
                object_name=object_name,
                object_type="mutable-alias",
                sha256=hashlib.sha256(payload).hexdigest(),
                size=len(payload),
                replica_nodes=(repo.repo_node,),
                operation_id="owned-op",
            )
            request = encode_repo_request(
                "STORE", manifest=manifest.to_dict(),
                writeIntent=RepoWriteIntent(
                    operation_id="owned-op", object_name=object_name,
                    generation=0, digest=manifest.sha256,
                    replication_factor=1,
                ).to_dict(),
                payloadB64=__import__("base64").b64encode(payload).decode(),
            )
            denied = repo._handle_context(
                {"requesterIdentity": "/mallory"}, request)
            self.assertFalse(denied.status)
            self.assertIn("repo-publisher-ownership-mismatch", denied.error)
            allowed = repo._handle_context(
                {"requesterIdentity": "/alice"}, request)
            self.assertTrue(allowed.status)

            repair_action = {
                "objectName": object_name,
                "objectSha256": manifest.sha256,
                "manifestSha256": "manifest-digest",
                "minReplicationFactor": 2,
                "maxReplicationFactor": 2,
                "sourceRepo": "/repo/source",
                "targetRepo": repo.repo_node,
                "reason": "under-replicated",
            }
            repo._db.execute("""
                INSERT INTO repair_jobs
                  (repair_id, object_name, generation, source_repo, target_repo,
                   state, attempts, next_attempt_ms, lease_owner,
                   lease_deadline_ms, result_json)
                VALUES ('repair-auth', ?, 0, ?, ?, 'RUNNING', 1, 0,
                        'test', ?, ?)
            """, (
                object_name, repair_action["sourceRepo"], repo.repo_node,
                repo._now_ms() + 1000,
                json.dumps({"action": repair_action}),
            ))
            repo._db.commit()
            repo._enforce_request_ownership(
                "STORE_PACKET_PULL",
                {
                    "manifest": manifest.to_dict(),
                    "repairAuthorization": repair_action,
                },
                repo.repo_node,
            )
            forged = dict(repair_action, sourceRepo="/repo/forged")
            with self.assertRaisesRegex(
                    PermissionError, "repo-publisher-ownership-mismatch"):
                repo._enforce_request_ownership(
                    "STORE_PACKET_PULL",
                    {"manifest": manifest.to_dict(),
                     "repairAuthorization": forged},
                    repo.repo_node,
                )
            repo._db.close()

    def test_same_object_read_write_is_coherent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3", budget=0)
            name = "/publisher/coherent"
            seen: list[bytes] = []
            errors: list[Exception] = []

            def write(payload: bytes) -> None:
                repo._persist_object(RepoObjectManifest(
                    object_name=name,
                    object_type="artifact",
                    sha256=hashlib.sha256(payload).hexdigest(),
                    size=len(payload),
                ), payload)

            write(b"old")

            def reader() -> None:
                try:
                    for _ in range(30):
                        seen.append(repo._load_persisted_object(name)[1])
                except Exception as exc:  # pragma: no cover - diagnostic path
                    errors.append(exc)

            worker = threading.Thread(target=reader)
            worker.start()
            write(b"new-value")
            worker.join(timeout=2.0)
            self.assertFalse(errors)
            self.assertTrue(seen)
            self.assertTrue(set(seen).issubset({b"old", b"new-value"}))
            repo._db.close()

    def test_capacity_is_incremental_and_admission_is_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            payload = b"capacity-counter"
            manifest = RepoObjectManifest(
                object_name="/publisher/capacity",
                object_type="artifact",
                sha256=hashlib.sha256(payload).hexdigest(),
                size=len(payload),
            )
            repo._persist_object(manifest, payload)
            self.assertEqual(repo._sqlite_used_bytes(), len(payload))
            repo._calculate_used_bytes_locked = lambda: (_ for _ in ()).throw(
                AssertionError("capability must not scan storage tables"))
            self.assertEqual(repo._capability().used_bytes, len(payload))

            repo._write_semaphore = threading.BoundedSemaphore(1)
            held = repo._admit_operation("STORE")
            started = time.monotonic()
            with self.assertRaisesRegex(RuntimeError, "repo-overloaded"):
                repo._admit_operation("STORE")
            self.assertLess(time.monotonic() - started, 0.25)
            repo._release_operation(held)
            self.assertGreaterEqual(repo._runtime_snapshot()["rejected"], 1)
            repo._db.close()

    def test_capability_publishes_live_runtime_metrics(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            response = repo._handle(encode_repo_request("CAPABILITY"))
            self.assertTrue(response.status)
            payload = json.loads(response.payload.decode())
            for key in (
                    "queueDepth", "inflightReads", "inflightWrites",
                    "inflightRepair", "rejected", "storageReadLatencyMs",
                    "storageWriteLatencyMs", "metricsTimestampMs"):
                self.assertIn(key, payload)
            hint = payload["providerCapabilityHint"]["runtime_hint"]
            self.assertGreaterEqual(hint["active_work_count"], 1)
            repo._db.close()

    def test_store_once_commits_only_confirmed_replicas(self) -> None:
        client = NetworkDistributedRepoClient.__new__(NetworkDistributedRepoClient)
        client.user = type("User", (), {"user": "/publisher"})()
        client.timeout_ms = 1000
        client.ack_timeout_ms = 10
        client.verbose = False
        client._select_repo_nodes = lambda **_kwargs: ["/repo/A", "/repo/B"]

        def request_specific_repo(*, repo_node: str, payload: bytes, **_kwargs):
            request = decode_repo_request(payload)
            if request["operation"] == "FINALIZE_WRITE":
                return type("Response", (), {
                    "payload": json.dumps({"status": "finalized"}).encode()
                })()
            intent = RepoWriteIntent.from_dict(request["writeIntent"])
            receipt = RepoWriteReceipt(
                operation_id=intent.operation_id,
                repo_node=repo_node,
                object_name=intent.object_name,
                generation=intent.generation,
                digest=intent.digest,
                persisted_bytes=4,
            )
            return type("Response", (), {
                "payload": json.dumps({"writeReceipt": receipt.to_dict()}).encode()
            })()

        def request_specific_repos(payload_by_repo, **_kwargs):
            return ({
                repo_node: request_specific_repo(
                    repo_node=repo_node, payload=payload)
                for repo_node, payload in payload_by_repo.items()
            }, {})

        client._request_specific_repos_parallel = request_specific_repos
        manifest = client._store_once(
            object_name="/publisher/object",
            payload=b"data",
            object_type="artifact",
            replication_factor=2,
            policy_epoch="test",
            operation="STORE",
        )
        self.assertEqual(manifest.lifecycle_state, "COMMITTED")
        self.assertEqual(manifest.confirmed_replica_nodes, ("/repo/A", "/repo/B"))

        def one_failure(*, repo_node: str, payload: bytes, **_kwargs):
            if repo_node == "/repo/B":
                raise TimeoutError("down")
            return request_specific_repo(repo_node=repo_node, payload=payload)

        def request_specific_repos_one_failure(payload_by_repo, **_kwargs):
            responses = {}
            failures = {}
            for repo_node, payload in payload_by_repo.items():
                try:
                    responses[repo_node] = one_failure(
                        repo_node=repo_node, payload=payload)
                except Exception as exc:  # noqa: BLE001
                    failures[repo_node] = str(exc)
            return responses, failures

        client._request_specific_repos_parallel = request_specific_repos_one_failure
        incomplete_operations = []

        def track_incomplete_operations(payload_by_repo, **kwargs):
            operation = decode_repo_request(next(iter(payload_by_repo.values())))[
                "operation"]
            incomplete_operations.append(operation)
            return request_specific_repos_one_failure(payload_by_repo, **kwargs)

        client._request_specific_repos_parallel = track_incomplete_operations
        with self.assertRaises(RepoIncompleteWriteError):
            client._store_once(
                object_name="/publisher/incomplete",
                payload=b"data",
                object_type="artifact",
                replication_factor=2,
                policy_epoch="test",
                operation="STORE",
            )
        self.assertEqual(incomplete_operations, ["STORE"])

    def test_rf3_quorum_commits_with_two_reservations_and_receipts(self) -> None:
        client = NetworkDistributedRepoClient.__new__(NetworkDistributedRepoClient)
        client.user = type("User", (), {"user": "/publisher"})()
        client.timeout_ms = 1000
        client.ack_timeout_ms = 10
        client.verbose = False
        client.enable_capacity_reservations = True
        client._replica_health = {}
        client._select_repo_nodes = lambda **_kwargs: [
            "/repo/A", "/repo/B", "/repo/C"]
        client._record_control_phase = lambda *_args, **_kwargs: None
        released = []
        client._release_reservations_parallel = lambda reservations: (
            released.append(dict(reservations)) or {})
        operations = []

        def response(payload: dict):
            return type("Response", (), {
                "status": True,
                "error": "",
                "payload": json.dumps(payload).encode(),
            })()

        def request_specific_repos(payload_by_repo, **_kwargs):
            operation = decode_repo_request(next(iter(payload_by_repo.values())))[
                "operation"]
            operations.append((operation, tuple(payload_by_repo)))
            responses = {}
            for repo_node, payload in payload_by_repo.items():
                request = decode_repo_request(payload)
                if repo_node == "/repo/C":
                    continue
                if operation == "RESERVE_CAPACITY":
                    responses[repo_node] = response({
                        "reservationId": request["reservationId"],
                        "operationId": request["operationId"],
                        "repoNode": repo_node,
                        "reservedBytes": request["reservedBytes"],
                        "state": "ACTIVE",
                        "expiresAtMs": int(time.time() * 1000) + 30_000,
                    })
                elif operation == "FINALIZE_WRITE":
                    responses[repo_node] = response({"status": "finalized"})
                else:
                    intent = RepoWriteIntent.from_dict(request["writeIntent"])
                    receipt = RepoWriteReceipt(
                        operation_id=intent.operation_id,
                        repo_node=repo_node,
                        object_name=intent.object_name,
                        generation=intent.generation,
                        digest=intent.digest,
                        persisted_bytes=4,
                    )
                    responses[repo_node] = response({
                        "writeReceipt": receipt.to_dict()})
            failures = ({"/repo/C": "provider unavailable"}
                        if "/repo/C" in payload_by_repo else {})
            return responses, failures

        client._request_specific_repos_parallel = request_specific_repos
        manifest = client.store_versioned(
            object_name="/publisher/quorum",
            payload=b"data",
            object_type="artifact",
            generation=0,
            expected_generation=-1,
            write_consistency=WriteConsistency.QUORUM.value,
            replication_factor=3,
            policy_epoch="test",
        )

        self.assertEqual(manifest.replication_factor, 3)
        self.assertEqual(manifest.required_write_acks, 2)
        self.assertEqual(manifest.confirmed_replica_nodes, ("/repo/A", "/repo/B"))
        self.assertEqual(len(manifest.replica_data_names), 2)
        self.assertEqual(operations[0], (
            "RESERVE_CAPACITY", ("/repo/A", "/repo/B", "/repo/C")))
        self.assertEqual(
            operations[1], ("STORE_PACKETS", ("/repo/A", "/repo/B")))
        self.assertEqual(
            operations[2], ("FINALIZE_WRITE", ("/repo/A", "/repo/B")))
        self.assertEqual(released, [])

    def test_rf3_all_rejects_two_reservations(self) -> None:
        client = NetworkDistributedRepoClient.__new__(NetworkDistributedRepoClient)
        client.user = type("User", (), {"user": "/publisher"})()
        client.timeout_ms = 1000
        client.ack_timeout_ms = 10
        client.verbose = False
        client.enable_capacity_reservations = True
        client._replica_health = {}
        client._select_repo_nodes = lambda **_kwargs: [
            "/repo/A", "/repo/B", "/repo/C"]
        client._record_control_phase = lambda *_args, **_kwargs: None
        released = []
        client._release_reservations_parallel = lambda reservations: (
            released.append(tuple(reservations)) or {})

        def request_specific_repos(payload_by_repo, **_kwargs):
            responses = {}
            for repo_node, payload in payload_by_repo.items():
                if repo_node == "/repo/C":
                    continue
                request = decode_repo_request(payload)
                responses[repo_node] = type("Response", (), {
                    "status": True,
                    "error": "",
                    "payload": json.dumps({
                        "reservationId": request["reservationId"],
                        "operationId": request["operationId"],
                        "repoNode": repo_node,
                        "reservedBytes": request["reservedBytes"],
                        "state": "ACTIVE",
                        "expiresAtMs": int(time.time() * 1000) + 30_000,
                    }).encode(),
                })()
            return responses, {"/repo/C": "provider unavailable"}

        client._request_specific_repos_parallel = request_specific_repos
        with self.assertRaisesRegex(RuntimeError, "reservation failed"):
            client.store_versioned(
                object_name="/publisher/all",
                payload=b"data",
                object_type="artifact",
                generation=0,
                expected_generation=-1,
                write_consistency=WriteConsistency.ALL.value,
                replication_factor=3,
                policy_epoch="test",
            )
        self.assertEqual(released, [("/repo/A", "/repo/B")])

    def test_quorum_omits_explicit_replica_in_active_cooldown(self) -> None:
        client = NetworkDistributedRepoClient.__new__(NetworkDistributedRepoClient)
        client.user = type("User", (), {"user": "/publisher"})()
        client.timeout_ms = 1000
        client.ack_timeout_ms = 10
        client.verbose = False
        client.enable_capacity_reservations = False
        client._replica_health = {
            "/repo/C": {"cooldownUntilMs": int(time.time() * 1000) + 60_000}}
        client._select_repo_nodes = lambda **_kwargs: [
            "/repo/A", "/repo/B", "/repo/C"]
        client._record_control_phase = lambda *_args, **_kwargs: None
        observed = []

        def request_specific_repos(payload_by_repo, **_kwargs):
            observed.append(tuple(payload_by_repo))
            responses = {}
            for repo_node, payload in payload_by_repo.items():
                request = decode_repo_request(payload)
                if request["operation"] == "FINALIZE_WRITE":
                    responses[repo_node] = type("Response", (), {
                        "status": True,
                        "error": "",
                        "payload": json.dumps({"status": "finalized"}).encode(),
                    })()
                    continue
                intent = RepoWriteIntent.from_dict(request["writeIntent"])
                receipt = RepoWriteReceipt(
                    operation_id=intent.operation_id,
                    repo_node=repo_node,
                    object_name=intent.object_name,
                    generation=intent.generation,
                    digest=intent.digest,
                    persisted_bytes=4,
                )
                responses[repo_node] = type("Response", (), {
                    "status": True,
                    "error": "",
                    "payload": json.dumps({
                        "writeReceipt": receipt.to_dict()}).encode(),
                })()
            return responses, {}

        client._request_specific_repos_parallel = request_specific_repos
        manifest = client.store_versioned(
            object_name="/publisher/cooldown",
            payload=b"data",
            object_type="artifact",
            generation=0,
            expected_generation=-1,
            write_consistency=WriteConsistency.QUORUM.value,
            replication_factor=3,
            policy_epoch="test",
        )
        self.assertEqual(observed, [
            ("/repo/A", "/repo/B"),
            ("/repo/A", "/repo/B"),
        ])
        self.assertEqual(manifest.replication_factor, 3)
        self.assertEqual(manifest.confirmed_replica_nodes, ("/repo/A", "/repo/B"))
        self.assertEqual(len(manifest.replica_data_names), 2)

    def test_object_write_receipt_is_atomic_and_replay_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            payload = b"confirmed-write"
            intent = RepoWriteIntent(
                operation_id="object-op-1",
                object_name="/publisher/object",
                generation=0,
                expected_generation=-1,
                digest=hashlib.sha256(payload).hexdigest(),
                replication_factor=1,
                selected_replicas=(repo.repo_node,),
            )
            manifest = RepoObjectManifest(
                object_name=intent.object_name,
                object_type="artifact",
                sha256=intent.digest,
                size=len(payload),
                replication_factor=1,
                replica_nodes=(repo.repo_node,),
                generation=0,
                operation_id=intent.operation_id,
            )

            first = repo._persist_object(manifest, payload, intent=intent)
            replay = repo._persist_object(manifest, payload, intent=intent)

            self.assertEqual(first, replay)
            self.assertEqual(first.state, "COMMITTED")
            self.assertEqual(first.persisted_bytes, len(payload))
            self.assertEqual(repo._db.execute(
                "SELECT COUNT(*) FROM write_receipts WHERE operation_id=?",
                (intent.operation_id,),
            ).fetchone()[0], 1)
            self.assertEqual(repo._db.execute(
                "SELECT state FROM write_operations WHERE operation_id=?",
                (intent.operation_id,),
            ).fetchone()[0], "COMMITTED")

            conflicting = RepoWriteIntent(
                operation_id=intent.operation_id,
                object_name=intent.object_name,
                generation=0,
                digest="ff" * 32,
                replication_factor=1,
            )
            with self.assertRaisesRegex(ValueError, "repo-operation-conflict"):
                repo._persist_object(manifest, payload, intent=conflicting)
            repo._db.close()

    def test_mutable_generation_compare_and_set_rejects_stale_writer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")

            def write(generation: int, expected: int, payload: bytes, operation_id: str):
                digest = hashlib.sha256(payload).hexdigest()
                intent = RepoWriteIntent(
                    operation_id=operation_id,
                    object_name="/publisher/alias",
                    generation=generation,
                    expected_generation=expected,
                    digest=digest,
                    replication_factor=1,
                )
                manifest = RepoObjectManifest(
                    object_name=intent.object_name,
                    object_type="mutable-alias",
                    object_class="mutable-alias",
                    sha256=digest,
                    size=len(payload),
                    generation=generation,
                    parent_generation=expected,
                    operation_id=operation_id,
                )
                return repo._persist_object(manifest, payload, intent=intent)

            write(0, -1, b"v0", "alias-op-0")
            write(1, 0, b"v1", "alias-op-1")
            with self.assertRaisesRegex(ValueError, "repo-generation-conflict"):
                write(2, 0, b"stale", "alias-op-stale")
            self.assertEqual(repo._load_manifest("/publisher/alias").generation, 1)
            repo._db.close()

    def test_receipt_validation_enforces_threshold_and_tuple(self) -> None:
        intent = RepoWriteIntent(
            operation_id="multi-op",
            object_name="/publisher/multi",
            generation=3,
            digest="aa" * 32,
            replication_factor=3,
            required_acks=2,
            consistency=WriteConsistency.QUORUM.value,
            selected_replicas=("/repo/A", "/repo/B", "/repo/C"),
        )
        receipts = [
            RepoWriteReceipt(
                operation_id=intent.operation_id,
                repo_node=repo,
                object_name=intent.object_name,
                generation=intent.generation,
                digest=intent.digest,
                persisted_bytes=10,
            )
            for repo in ("/repo/A", "/repo/B")
        ]
        self.assertEqual(validate_write_receipts(intent, receipts), tuple(receipts))
        with self.assertRaises(RepoIncompleteWriteError) as raised:
            validate_write_receipts(intent, receipts[:1], failures={"/repo/B": "timeout"})
        self.assertEqual(raised.exception.confirmed_replicas, ("/repo/A",))

    def test_packet_batches_require_explicit_complete_set_commit(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = make_repo(Path(tmp) / "repo.sqlite3")
            payload = b"packet-set" * 1000
            packets = make_segmented_data_packets(
                "/data/confirmed/v=1", payload,
                signing_identity="/test/repo/ha-publisher",
                max_segment_size=1500,
            )
            digest = hashlib.sha256(payload).hexdigest()
            batch_manifest = RepoObjectManifest(
                object_name="/publisher/packet-set",
                object_type="ndn-data-wire",
                sha256=digest,
                size=len(payload),
                segment_count=len(packets),
                packet_names=(),
                replica_nodes=(repo.repo_node,),
                operation_id="packet-batch",
                lifecycle_state="RUNNING",
            )
            batch_intent = RepoWriteIntent(
                operation_id="packet-batch",
                object_name=batch_manifest.object_name,
                generation=0,
                digest=digest,
                replication_factor=1,
                selected_replicas=(repo.repo_node,),
            )
            repo._persist_packets(batch_manifest, packets, intent=batch_intent)

            commit_manifest = RepoObjectManifest(
                **{
                    **batch_manifest.__dict__,
                    "packet_names": tuple(packet.name for packet in packets),
                    "operation_id": "packet-object-commit",
                }
            )
            commit_intent = RepoWriteIntent(
                operation_id="packet-object-commit",
                object_name=commit_manifest.object_name,
                generation=0,
                digest=digest,
                replication_factor=1,
                selected_replicas=(repo.repo_node,),
            )
            receipt = repo._commit_existing_packet_set(commit_manifest, commit_intent)
            self.assertEqual(receipt.state, "COMMITTED")
            self.assertEqual(receipt.persisted_bytes,
                             sum(len(packet.wire) for packet in packets))
            self.assertEqual(
                repo._load_manifest(commit_manifest.object_name).operation_id,
                commit_intent.operation_id,
            )
            repo._db.close()

    def test_existing_spec_076_database_upgrades_without_data_loss(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            database = Path(tmp) / "repo.sqlite3"
            db = sqlite3.connect(database)
            db.execute("""
                CREATE TABLE objects (
                    object_name TEXT PRIMARY KEY,
                    manifest_json TEXT NOT NULL,
                    payload BLOB,
                    payload_size INTEGER NOT NULL DEFAULT 0,
                    sha256 TEXT NOT NULL,
                    object_type TEXT NOT NULL,
                    updated_at REAL NOT NULL,
                    hit_count INTEGER NOT NULL DEFAULT 0
                )
            """)
            payload = b"legacy"
            manifest = {
                "objectName": "/publisher/legacy",
                "objectType": "artifact",
                "sha256": hashlib.sha256(payload).hexdigest(),
                "size": len(payload),
            }
            db.execute(
                "INSERT INTO objects VALUES (?, ?, ?, ?, ?, ?, ?, 0)",
                (manifest["objectName"], json.dumps(manifest), payload, len(payload),
                 manifest["sha256"], manifest["objectType"], 1.0),
            )
            db.commit()
            db.close()

            repo = make_repo(database)
            row = repo._db.execute(
                "SELECT payload FROM objects WHERE object_name=?",
                (manifest["objectName"],),
            ).fetchone()
            self.assertEqual(bytes(row[0]), payload)
            self.assertEqual(repo._load_manifest(manifest["objectName"]).generation, 0)
            repo._db.close()


if __name__ == "__main__":
    unittest.main()
