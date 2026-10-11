"""Actual source-linked C++ Name inspection and precommit Repo admission."""
import importlib.util
import os
import sqlite3
import json
from pathlib import Path
import sys
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


class RepoNameAdmission(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="spec201-repo-name-")
        cls.addClassCleanup(cls.directory.cleanup)
        previous = {key: os.environ.get(key) for key in ("NDN_CLIENT_PIB", "NDN_CLIENT_TPM")}
        def restore():
            for key, value in previous.items():
                if value is None: os.environ.pop(key, None)
                else: os.environ[key] = value
        cls.addClassCleanup(restore)
        os.environ["NDN_CLIENT_PIB"] = "pib-sqlite3:" + cls.directory.name + "/pib"
        os.environ["NDN_CLIENT_TPM"] = "tpm-file:" + cls.directory.name + "/tpm"
        setup_env = dict(os.environ)
        setup_env.pop("LD_PRELOAD", None)
        subprocess.run(["ndnsec", "key-gen", "-t", "r", "/spec201/repo-name-owner"],
                       env=setup_env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                       check=True, timeout=15)
        path = Path(os.environ["SPEC201_PYTHON_BINDING"]).resolve(strict=True)
        spec = importlib.util.spec_from_file_location("_ndnsf", path)
        native = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(native)
        sys.modules["_ndnsf"] = native
        sys.modules["ndnsf._ndnsf"] = native
        from ndnsf import inspect_data_name
        cls.inspect = staticmethod(inspect_data_name)

    def test_legacy_index_reopen_alias_cache_and_original_serving_wire(self):
        import hashlib
        from ndnsf import make_segmented_data_packets
        from test_ndnsf_repo_exact_packets import make_repo, make_manifest
        packets = make_segmented_data_packets(
            "/spec201/repo-name-owner/data%25label_", b"x" * 6000,
            signing_identity="/spec201/repo-name-owner", max_segment_size=4096)
        self.assertGreater(len(packets), 1)
        with tempfile.TemporaryDirectory() as root:
            database = Path(root) / "legacy.sqlite3"
            # Independent schema predates canonical_name; use the URI profile
            # opposite to the current decoder and preserve its physical keys.
            legacy = sqlite3.connect(database)
            legacy.execute("""CREATE TABLE data_packets (
                data_name TEXT PRIMARY KEY, wire BLOB NOT NULL,
                wire_sha256 TEXT NOT NULL, wire_size INTEGER NOT NULL,
                updated_at REAL NOT NULL, hit_count INTEGER NOT NULL DEFAULT 0)""")
            keys = [self.inspect(p.name)["alternate_name" if os.environ.get("NDN_NAME_ALT_URI") == "0"
                                         else "canonical_name"] for p in packets]
            legacy.executemany("INSERT INTO data_packets VALUES (?, ?, ?, ?, 0, 0)",
                               [(key, p.wire, hashlib.sha256(p.wire).hexdigest(), len(p.wire))
                                for key, p in zip(keys, packets)])
            legacy.commit()
            legacy.close()
            store = make_repo(database, budget=0)
            try:
                manifest = make_manifest("/original", b"x" * 6000, packets)
                store._persist_packets(manifest, packets)
                rows = store._db.execute("SELECT data_name, wire FROM data_packets ORDER BY data_name").fetchall()
                self.assertEqual(dict(rows), dict(zip(keys, [p.wire for p in packets])))
                for packet in packets:
                    info = self.inspect(packet.name)
                    for spelling in (info["canonical_name"], info["alternate_name"]):
                        self.assertTrue(store._sqlite_has_packet(spelling))
                        self.assertEqual(store._load_persisted_packet(spelling).wire, packet.wire)
                        self.assertEqual(store._lookup_data_plane_wire(spelling, False), packet.wire)
                        self.assertEqual({p.wire for p in store._load_persisted_packet_prefix(spelling)},
                                         {p.wire for p in packets})
                from py_repoclient import orchestration as module
                for spelling in (self.inspect(packets[0].name)["canonical_name"],
                                 self.inspect(packets[0].name)["alternate_name"]):
                    request = {"operation": "FETCH_PACKET_PREPARE", "dataName": spelling}
                    with mock.patch.object(module, "decode_repo_request", return_value=request), \
                            mock.patch.object(store, "_enforce_request_ownership"), \
                            mock.patch.object(store, "_serve_packets") as serve, \
                            mock.patch.object(store, "_serving_forwarding_hint", return_value="/repo/hint"):
                        result = store._handle(b"request")
                        self.assertTrue(result.status, result.error)
                        response = json.loads(result.payload)
                        self.assertEqual(response["wireSha256"], hashlib.sha256(packets[0].wire).hexdigest())
                        self.assertEqual(self.inspect(response["dataName"])["name_wire"],
                                         self.inspect(packets[0].name)["name_wire"])
                        self.assertEqual({p.wire for p in serve.call_args.args[0]}, {p.wire for p in packets})
                parent = self.inspect(packets[0].name)["segment_prefix"]
                self.assertIn(store._lookup_data_plane_wire(parent, True), [p.wire for p in packets])
                self.assertIsNone(store._lookup_data_plane_wire(parent + "-other", True))
                # Canonical cache keys also work when the decoded value retains
                # its environment-selected spelling.
                store._hot_cache.budget_bytes = 65536
                first = packets[0]
                store._hot_cache.put_packet(first)
                for spelling in (self.inspect(first.name)["canonical_name"], self.inspect(first.name)["alternate_name"]):
                    self.assertEqual(store._hot_cache.get_packet(spelling).wire, first.wire)
                store._hot_cache.invalidate_packet(self.inspect(first.name)["canonical_name"])
                self.assertIsNone(store._hot_cache.get_packet(first.name))
            finally:
                store._persistence.close()
            reopened = make_repo(database, budget=0)
            try:
                self.assertEqual(reopened._load_persisted_packet(keys[0]).wire, packets[0].wire)
                self.assertEqual(dict(reopened._db.execute("SELECT data_name, wire FROM data_packets")),
                                 dict(zip(keys, [p.wire for p in packets])))
            finally:
                reopened._persistence.close()

    def test_legacy_alias_wire_conflict_aborts_index_without_rewriting_wire(self):
        import hashlib
        from ndnsf import make_segmented_data_packets
        from test_ndnsf_repo_exact_packets import make_repo
        packet = make_segmented_data_packets(
            "/spec201/repo-name-owner/conflict", b"payload",
            signing_identity="/spec201/repo-name-owner", max_segment_size=4096)[0]
        info = self.inspect(packet.name)
        changed_wire = packet.wire[:-1] + bytes([packet.wire[-1] ^ 1])
        with tempfile.TemporaryDirectory() as root:
            database = Path(root) / "conflict.sqlite3"
            connection = sqlite3.connect(database)
            connection.execute("""CREATE TABLE data_packets (
                data_name TEXT PRIMARY KEY, wire BLOB NOT NULL,
                wire_sha256 TEXT NOT NULL, wire_size INTEGER NOT NULL,
                updated_at REAL NOT NULL, hit_count INTEGER NOT NULL DEFAULT 0)""")
            original_rows = [(info["canonical_name"], packet.wire), (info["alternate_name"], changed_wire)]
            connection.executemany("INSERT INTO data_packets VALUES (?, ?, ?, ?, 0, 0)",
                                   [(name, wire, hashlib.sha256(wire).hexdigest(), len(wire))
                                    for name, wire in original_rows])
            connection.commit()
            connection.close()
            from py_repoclient.orchestration import RepoNodeApp
            failed_store = RepoNodeApp.__new__(RepoNodeApp)
            failed_store.repo_node = "/repo/exact"
            failed_store._db = sqlite3.connect(database, check_same_thread=False)
            try:
                with self.assertRaisesRegex(ValueError, "repo-data-wire-conflict"):
                    failed_store._init_sqlite()
            finally:
                failed_store._persistence.close()
            with sqlite3.connect(database) as connection:
                self.assertEqual(dict(connection.execute("SELECT data_name, wire FROM data_packets")),
                                 dict(original_rows))

    def test_actual_signed_wire_admits_alias_and_rejects_duplicate_namewire(self):
        import base64
        import hashlib
        from ndnsf import DataPacket, make_segmented_data_packets
        from py_repoclient.orchestration import RepoNodeApp
        from test_ndnsf_repo_exact_packets import make_repo, make_manifest
        packets = make_segmented_data_packets(
            "/spec201/repo-name-owner/data", b"payload",
            signing_identity="/spec201/repo-name-owner", max_segment_size=4096)
        self.assertEqual(len(packets), 1)
        original = packets[0]
        names = self.inspect(original.name)
        repo = RepoNodeApp.__new__(RepoNodeApp)
        admitted = repo._decode_packet_object({
            "name": names["alternate_name"], "segmentName": names["canonical_name"],
            "segment": original.segment, "wireB64": base64.b64encode(original.wire).decode(),
            "wireSha256": hashlib.sha256(original.wire).hexdigest()}, "STORE_PACKETS")
        self.assertEqual(admitted.wire, original.wire)
        with tempfile.TemporaryDirectory() as root:
            store = make_repo(Path(root) / "repo.sqlite3")
            self.addCleanup(store._db.close)
            manifest = make_manifest("/original", b"payload", [original])
            alias = DataPacket(names["alternate_name"], original.segment, original.wire, original.content)
            store._persist_packets(manifest, [alias])
            self.assertEqual(store._load_persisted_packet(original.name).wire, original.wire)
            with self.assertRaisesRegex(ValueError, "duplicate packet"):
                store._persist_packets(manifest, [original, alias])
            self.assertEqual(store._load_persisted_packet(original.name).wire, original.wire)

    def test_uri_profiles_have_equal_wire_and_typed_segment_parent(self):
        alternate = self.inspect("/actual/producer/v=1/seg=1")
        canonical = self.inspect("/8=actual/8=producer/54=%01/50=%01")
        self.assertEqual(alternate, canonical)
        self.assertEqual(alternate["segment"], 1)
        self.assertEqual(alternate["segment_prefix"], "/8=actual/8=producer/54=%01")
        generic = self.inspect("/actual/producer/1")
        self.assertIsNone(generic["segment"])
        self.assertEqual(generic["segment_prefix"], generic["canonical_name"])
        with self.assertRaises(ValueError):
            self.inspect("/")

    def test_signed_pull_calls_each_replica_once_and_keeps_receipt_checks(self):
        import hashlib
        from ndnsf import make_segmented_data_packets
        from py_repoclient import orchestration as module
        payload = b"original" * 1100
        packets = make_segmented_data_packets(
            "/spec201/repo-name-owner/data", payload,
            signing_identity="/spec201/repo-name-owner", max_segment_size=4000)
        client = module.NetworkDistributedRepoClient.__new__(module.NetworkDistributedRepoClient)
        client.user = SimpleNamespace(user="/spec201/repo-name-owner")
        client.timeout_ms = 1000
        requests = []

        def request(**kwargs):
            obj = module.decode_repo_request(kwargs["payload"])
            requests.append((kwargs["repo_node"], obj))
            intent = obj["writeIntent"]
            receipt = module.RepoWriteReceipt(
                operation_id=intent["operationId"], repo_node=kwargs["repo_node"],
                object_name=intent["objectName"], generation=intent["generation"],
                digest=intent["digest"], persisted_bytes=len(payload))
            return SimpleNamespace(payload=json.dumps({"writeReceipt": receipt.to_dict()}).encode())

        options = dict(object_name=client.publisher_namespace + "/model", packets=packets,
                       object_type="model-artifact", object_size=len(payload),
                       object_sha256=hashlib.sha256(payload).hexdigest(), policy_epoch="epoch",
                       replication_factor=2, packet_manifest_name="/spec201/repo-name-owner/index/v=7",
                       packet_manifest_sha256="b" * 64)
        with mock.patch.object(client, "_select_repo_nodes", return_value=["/repo/A", "/repo/B"]), \
                mock.patch.object(client, "_request_specific_repo", side_effect=request), \
                mock.patch.object(client, "_packet_batches", side_effect=AssertionError("batch fallback")), \
                mock.patch.object(client, "_finalize_receipt_quorum", side_effect=lambda i, m, r: m):
            result = client.store_signed_packets(**options)
            self.assertEqual([obj["operation"] for _, obj in requests],
                             ["STORE_PACKET_PULL", "STORE_PACKET_PULL",
                              "COMMIT_PACKET_SET", "COMMIT_PACKET_SET"])
            for node, obj in requests[:2]:
                self.assertEqual(obj["packetManifestSha256"], "b" * 64)
                self.assertEqual(obj["packetManifestName"], options["packet_manifest_name"])
                self.assertNotIn("packets", obj)
                self.assertEqual(obj["manifest"]["packetNames"], [])
                self.assertEqual(obj["writeIntent"]["selectedReplicas"], [node])
            self.assertEqual(result.packet_names, tuple(p.name for p in packets))
            requests.clear()
            with self.assertRaisesRegex(ValueError, "index name and SHA256"):
                client.store_signed_packets(**dict(options, packet_manifest_sha256=""))
            self.assertEqual(requests, [])

    def test_client_preserves_version_and_checks_alias_prefix_before_selection(self):
        import hashlib
        from ndnsf import make_segmented_data_packets
        from py_repoclient.orchestration import NetworkDistributedRepoClient
        packets = make_segmented_data_packets(
            "/spec201/repo-name-owner/data", b"payload",
            signing_identity="/spec201/repo-name-owner", max_segment_size=4096)
        expected_parent = self.inspect(packets[0].name)["segment_prefix"]
        self.assertEqual(NetworkDistributedRepoClient._packet_data_name(packets), expected_parent)
        self.assertEqual(NetworkDistributedRepoClient._packet_versioned_data_name(packets), expected_parent)
        client = NetworkDistributedRepoClient.__new__(NetworkDistributedRepoClient)
        client.user = SimpleNamespace(user="/spec201/repo-name-owner")
        options = dict(
            object_name=self.inspect(client.publisher_namespace + "/model")["canonical_name"],
            packets=packets, object_type="ndn-data-packet-set", object_size=7,
            object_sha256=hashlib.sha256(b"payload").hexdigest(), policy_epoch="epoch")
        with mock.patch.object(client, "_select_repo_nodes",
                               side_effect=RuntimeError("selection reached")) as select:
            # A canonical declaration must admit an alternate encoded packet,
            # and vice versa; packet wire remains the original signed wire.
            for prefix in (expected_parent, self.inspect(expected_parent)["alternate_name"]):
                with self.assertRaisesRegex(RuntimeError, "selection reached"):
                    client.store_signed_packets(**options, data_name=prefix)
            self.assertEqual(select.call_count, 2)
            select.reset_mock()
            with self.assertRaisesRegex(ValueError, "outside the declared original"):
                client.store_signed_packets(**options, data_name="/spec201/repo-name-owner/data-other")
            select.assert_not_called()
            invalid_object = dict(options, object_name="/other/objects/model")
            with self.assertRaisesRegex(ValueError, "publisher namespace"):
                client.store_signed_packets(**invalid_object, data_name=expected_parent)
            select.assert_not_called()

    def test_repo_prefix_helper_uses_components_for_both_uri_profiles(self):
        from ndnsf import DataPacket
        from py_repoclient.orchestration import _packet_set_versioned_data_name
        packets = [DataPacket("/actual/producer/v=1/seg=0", 0, b""),
                   DataPacket("/8=actual/8=producer/54=%01/50=%01", 1, b"")]
        self.assertEqual(_packet_set_versioned_data_name(packets),
                         "/8=actual/8=producer/54=%01")
        with self.assertRaises(ValueError):
            _packet_set_versioned_data_name(packets + [
                DataPacket("/actual/other/seg=2", 2, b"")])

    def test_store_handler_rejects_invalid_prefix_before_persistence(self):
        from py_repoclient import orchestration as module
        from ndnsf import DataPacket
        repo = module.RepoNodeApp.__new__(module.RepoNodeApp)
        repo.repo_node = "/repo"
        manifest = module.RepoObjectManifest(object_name="/object", object_type="ndn-data-packet-set",
                                             sha256="a" * 64, size=2)
        packets = [DataPacket("/producer/one/seg=0", 0, b"one"),
                   DataPacket("/producer/two/seg=1", 1, b"two")]
        request = {"operation": "STORE_PACKETS", "manifest": manifest.to_dict(),
                   "packets": [{}, {}]}
        # Mock only wire/request admission and persistence boundaries; exercise
        # the actual handler and actual native-backed Name prefix validation.
        with mock.patch.object(module, "decode_repo_request", return_value=request), \
                mock.patch.object(repo, "_enforce_request_ownership"), \
                mock.patch.object(repo, "_admit_operation", return_value=None), \
                mock.patch.object(repo, "_write_intent_from_request", return_value=None), \
                mock.patch.object(repo, "_manifest_for_write_intent", return_value=manifest), \
                mock.patch.object(repo, "_decode_packet_object", side_effect=packets), \
                mock.patch.object(repo, "_persist_packets") as persist:
            result = repo._handle(b"request")
            self.assertFalse(result.status)
            self.assertIn("mixes Data prefixes", result.error)
            persist.assert_not_called()


if __name__ == "__main__":
    unittest.main()
