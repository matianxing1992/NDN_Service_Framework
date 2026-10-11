"""Original protected sets exported by the production-linked Core fixture."""
import hashlib
import base64
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest import mock

from test_spec201_repo_name_admission import RepoNameAdmission


class ProtectedRepoGroups(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.addClassCleanup(RepoNameAdmission.doClassCleanups)
        RepoNameAdmission.setUpClass()
        directory = tempfile.TemporaryDirectory(prefix="spec201-protected-groups-")
        cls.addClassCleanup(directory.cleanup)
        cls.oracle = Path(directory.name)
        env = dict(os.environ, SPEC201_PUBLICATION_PACKET_ORACLE=str(cls.oracle))
        env.pop("LD_PRELOAD", None)  # Native fixture is already sanitizer-linked.
        binary = os.environ.get("SPEC201_PUBLICATION_ORACLE_BINARY",
                                "/var/tmp/spec200-asan/spec201-original-packet-publication")
        result = subprocess.run([binary, "--run_test=Spec201OriginalPacketPublication/"
                                 "SemanticManifestCategoriesKeepOriginalBlobPackets"],
                                env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=45)
        if result.returncode:
            raise AssertionError(result.stdout)
        from ndnsf import decode_data_packet
        cls.groups = []
        for group in sorted(cls.oracle.iterdir(), key=lambda path: int(path.name)):
            files = [group / "root.wire"] + sorted(
                (path for path in group.glob("*.wire") if path.stem.isdigit()),
                key=lambda path: int(path.stem))
            cls.groups.append([decode_data_packet(path.read_bytes()) for path in files])
        if len(cls.groups) != 12:
            raise AssertionError(f"expected twelve actual Core groups, got {len(cls.groups)}")

    def test_all_core_groups_commit_lookup_and_prepare_without_wire_changes(self):
        from ndnsf import inspect_data_name, inspect_original_data_packet
        from py_repoclient import orchestration as module
        from test_ndnsf_repo_exact_packets import make_repo, make_manifest
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / "repo.sqlite3"
            repo = make_repo(database, budget=0)
            try:
                for index, packets in enumerate(self.groups):
                    with self.subTest(group=index):
                        description = inspect_original_data_packet(packets[0].wire)["protected_manifest"]
                        self.assertEqual(description["segment_count"], len(packets) - 1)
                        prefix = module._packet_set_versioned_data_name(list(reversed(packets)))
                        self.assertEqual(prefix, description["serving_prefix"])
                        self.assertTrue(prefix.startswith(description["producer"]))
                        manifest = make_manifest(f"/original/group/{index}", b"\x42" * 40000, packets)
                        repo._persist_packets(manifest, list(reversed(packets)))
                        _, retained = repo._load_persisted_packets(manifest.object_name)
                        self.assertEqual([p.wire for p in retained], [p.wire for p in packets])
                        for packet in (packets[0], packets[1], packets[-1]):
                            info = inspect_data_name(packet.name)
                            for spelling in (info["canonical_name"], info["alternate_name"]):
                                fetched = repo._load_persisted_packet_prefix(spelling)
                                self.assertEqual([p.wire for p in fetched], [p.wire for p in packets])
                                self.assertEqual(repo._lookup_data_plane_wire(spelling, False), packet.wire)
                                request = {"operation": "FETCH_PACKET_PREPARE", "dataName": spelling}
                                with mock.patch.object(module, "decode_repo_request", return_value=request), \
                                        mock.patch.object(repo, "_enforce_request_ownership"), \
                                        mock.patch.object(repo, "_serve_packets") as serve, \
                                        mock.patch.object(repo, "_serving_forwarding_hint", return_value="/repo/hint"):
                                    response = repo._handle(b"request")
                                    self.assertTrue(response.status, response.error)
                                    self.assertEqual(json.loads(response.payload)["wireSha256"],
                                                     hashlib.sha256(packet.wire).hexdigest())
                                    self.assertEqual([p.wire for p in serve.call_args.args[0]],
                                                     [p.wire for p in packets])
            finally:
                repo._persistence.close()
            reopened = make_repo(database, budget=0)
            try:
                for index, packets in enumerate(self.groups):
                    _, retained = reopened._load_persisted_packets(f"/original/group/{index}")
                    self.assertEqual([p.wire for p in retained], [p.wire for p in packets])
                    self.assertEqual([p.wire for p in reopened._load_persisted_packet_prefix(packets[0].name)],
                                     [p.wire for p in packets])
            finally:
                reopened._persistence.close()

    def test_missing_root_segment_duplicate_and_mixed_sets_never_commit(self):
        from ndnsf import DataPacket, decode_data_packet
        from py_repoclient.orchestration import _packet_set_versioned_data_name
        from test_ndnsf_repo_exact_packets import make_repo, make_manifest
        packets = self.groups[0]
        variants = {
            "root-only": packets[:1], "no-root": packets[1:],
            "missing-segment": packets[:-1], "duplicate-root": packets + packets[:1],
            "duplicate-segment": packets + packets[1:2],
            "mixed-object": packets[:-1] + self.groups[1][-1:],
            "wrong-final": packets[:1] + [decode_data_packet((self.oracle / "0/invalid-final.wire").read_bytes())] + packets[2:],
            "wrong-size": packets[:1] + [decode_data_packet((self.oracle / "0/invalid-size.wire").read_bytes())] + packets[2:],
            "malformed-root": [decode_data_packet((self.oracle / "0/invalid-root.wire").read_bytes())] + packets[1:],
            "trailing-root-wire": [DataPacket(packets[0].name, 0, packets[0].wire + b"\x00")] + packets[1:],
        }
        with tempfile.TemporaryDirectory() as directory:
            repo = make_repo(Path(directory) / "repo.sqlite3", budget=0)
            try:
                trailing = packets[0].wire + b"\x00"
                with self.assertRaisesRegex(ValueError, "original packet framing"):
                    repo._decode_packet_object({"name": packets[0].name, "segment": 0,
                        "wireB64": base64.b64encode(trailing).decode(),
                        "wireSha256": hashlib.sha256(trailing).hexdigest()}, "STORE_PACKETS")
                for label, variant in variants.items():
                    with self.subTest(case=label):
                        with self.assertRaises(ValueError):
                            _packet_set_versioned_data_name(variant)
                        manifest = make_manifest("/invalid/" + label, b"\x42" * 40000, variant)
                        with self.assertRaises(ValueError):
                            repo._persist_packets(manifest, variant)
                        self.assertFalse(repo._sqlite_has_object(manifest.object_name))
                        self.assertEqual(repo._db.execute("SELECT COUNT(*) FROM data_packets").fetchone()[0], 0)
            finally:
                repo._persistence.close()

    def test_actual_pull_handler_fetches_root_then_segments_and_rejects_name_substitution(self):
        from ndnsf import inspect_data_name
        from py_repoclient import orchestration as module
        from test_ndnsf_repo_exact_packets import make_repo, make_manifest
        packets = self.groups[1]
        with tempfile.TemporaryDirectory() as directory:
            repo = make_repo(Path(directory) / "repo.sqlite3", budget=0)
            repo._catalog_lock = threading.RLock()
            manifest = make_manifest("/original/pull", b"\x42" * 40000, packets)
            repo._global_catalog = {manifest.object_name: {repo.repo_node: {"objectName": manifest.object_name}}}
            original_persist = repo._persist_packets
            def persist(*args, **kwargs):
                original_persist(*args, **kwargs)
                # Receipt encoding is outside packet geometry; keep the actual
                # SQL commit and original-wire/index path beneath this boundary.
                return SimpleNamespace(to_dict=lambda: {})
            entries = [{"name": inspect_data_name(p.name)["alternate_name"], "segment": p.segment,
                        "wireSha256": hashlib.sha256(p.wire).hexdigest()} for p in packets]
            request = {"operation": "STORE_PACKET_PULL", "manifest": manifest.to_dict(),
                       "packetManifestName": "/source/packet-index", "sourceName": "/source"}
            try:
                with mock.patch.object(module, "decode_repo_request", return_value=request), \
                        mock.patch.object(repo, "_enforce_request_ownership"), \
                        mock.patch.object(repo, "_write_intent_from_request", return_value=None), \
                        mock.patch.object(repo, "_manifest_for_write_intent", return_value=manifest), \
                        mock.patch.object(module, "fetch_segmented_object", return_value=json.dumps({"packets": entries}).encode()), \
                        mock.patch.object(module, "fetch_exact_data_packet", return_value=packets[0]) as root_fetch, \
                        mock.patch.object(module, "fetch_segmented_data_packets", return_value=packets[1:]) as data_fetch, \
                        mock.patch.object(repo, "_serve_packets"), \
                        mock.patch.object(repo, "_persist_packets", side_effect=persist) as commit:
                    response = repo._handle(b"request")
                    self.assertTrue(response.status, response.error)
                    self.assertEqual(root_fetch.call_args.args[0], entries[0]["name"])
                    self.assertEqual(data_fetch.call_args.args[0], inspect_data_name(packets[1].name)["segment_prefix"])
                    commit.assert_called_once()
                    _, retained = repo._load_persisted_packets(manifest.object_name)
                    self.assertEqual([p.wire for p in retained], [p.wire for p in packets])
                    commit.reset_mock()
                    original_index = json.dumps({"packets": entries}).encode()
                    request["packetManifestSha256"] = "0" * 64
                    response = repo._handle(b"request")
                    self.assertFalse(response.status)
                    self.assertIn("packet manifest hash mismatch", response.error)
                    commit.assert_not_called()
                    request["packetManifestSha256"] = hashlib.sha256(original_index).hexdigest()
                    entries[1]["wireSha256"] = "0" * 64
                    tampered_index = json.dumps({"packets": entries}).encode()
                    request["packetManifestSha256"] = hashlib.sha256(tampered_index).hexdigest()
                    module.fetch_segmented_object.return_value = tampered_index
                    response = repo._handle(b"request")
                    self.assertFalse(response.status)
                    self.assertIn("wire hash mismatch", response.error)
                    commit.assert_not_called()
                    entries[1]["wireSha256"] = hashlib.sha256(packets[1].wire).hexdigest()
                    request.pop("packetManifestSha256")
                    # Same ordinal and exact same hash are insufficient when
                    # the index declares another original Producer Name.
                    for entry in entries[1:]:
                        entry["name"] = "/foreign" + entry["name"]
                    module.fetch_segmented_object.return_value = json.dumps({"packets": entries}).encode()
                    response = repo._handle(b"request")
                    self.assertFalse(response.status)
                    self.assertIn("unexpected original Data name", response.error)
                    commit.assert_not_called()
                    _, retained = repo._load_persisted_packets(manifest.object_name)
                    self.assertEqual([p.wire for p in retained], [p.wire for p in packets])
            finally:
                repo._persistence.close()

    def test_exact_client_accepts_prepared_name_alias_and_rejects_foreign_name(self):
        from ndnsf import inspect_data_name
        from py_repoclient import orchestration as module
        packet = self.groups[0][0]
        client = module.NetworkDistributedRepoClient.__new__(module.NetworkDistributedRepoClient)
        client.timeout_ms = 1000
        prepared = {"dataName": inspect_data_name(packet.name)["canonical_name"],
                    "wireSha256": hashlib.sha256(packet.wire).hexdigest(), "forwardingHints": ["/repo/hint"]}
        with mock.patch.object(client, "_request_specific_repo",
                               return_value=SimpleNamespace(payload=json.dumps(prepared).encode())) as control, \
                mock.patch.object(module, "fetch_exact_data_packet", return_value=packet) as fetch:
            self.assertEqual(client.fetch_packet("/repo/exact", inspect_data_name(packet.name)["alternate_name"]).wire,
                             packet.wire)
            fetch.assert_called_once()
            fetch.reset_mock()
            prepared["dataName"] = "/foreign" + prepared["dataName"]
            control.return_value = SimpleNamespace(payload=json.dumps(prepared).encode())
            with self.assertRaisesRegex(ValueError, "preparation name mismatch"):
                client.fetch_packet("/repo/exact", packet.name)
            fetch.assert_not_called()


if __name__ == "__main__":
    unittest.main()
