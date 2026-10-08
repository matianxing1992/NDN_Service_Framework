"""Mutation probes for Spec202's shared no-side-effect MiniNDN admission gate."""

import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import spec193_preflight as preflight


ROOT = Path(__file__).resolve().parents[2]
OPERATIONS = (
    "STORE", "STORE_RANGE", "COMMIT_RANGES", "ABORT_RANGES", "INSERT",
    "STORE_MANIFEST", "FETCH", "FETCH_RANGE", "MANIFEST", "INVENTORY",
    "CAPABILITY", "CACHE_STATUS", "STATUS", "CATALOG_STATUS",
    "CATALOG_SNAPSHOT", "CATALOG_DELTA", "CATALOG_LOOKUP", "DELETE",
)
PREFIX = "/NDNSF/DistributedRepo/Object/v1"
ROLES = {"memphis": "controller", "neu": "app", "ucla": "repoA",
         "wustl": "repoB", "arizona": "consumer", "seattle": "repoC"}
USERS = ["/example/repo/user", "/example/repo/consumer",
         "/example/repo/provider/repoA", "/example/repo/provider/repoB",
         "/example/repo/provider/repoC", "/example/repo/provider/spec192-di",
         "/example/repo/di-authorized-reader"]
PROVIDERS = ["/example/repo/provider/repoA", "/example/repo/provider/repoB",
             "/example/repo/provider/repoC"]


class PreflightMutationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="spec193-preflight-")
        self.addCleanup(self.temp.cleanup)
        self.scratch = Path(self.temp.name)
        self.output = self.scratch / "must-not-exist"
        self.topology = ROOT / "tests/minindn/spec193_six_host.conf"

    def check(self, **changes):
        source_sha, source_files = preflight.source_candidate(ROOT)
        arguments = dict(root=ROOT, output=self.output, topology=self.topology,
                         source_sha=source_sha, source_files=source_files, roles=ROLES,
                         service_prefix=PREFIX, operations=OPERATIONS,
                         repo_users=USERS, repo_providers=PROVIDERS,
                         require_six=True)
        arguments.update(changes)
        return preflight.check(**arguments)

    def reject(self, failure_class, output_exists=False, **changes):
        with self.assertRaises(preflight.PreflightError) as caught:
            self.check(**changes)
        self.assertEqual(caught.exception.failure_class, failure_class)
        self.assertEqual(self.output.exists(), output_exists)

    def test_good_candidate_and_bad_lock(self):
        state = self.check()
        self.assertEqual(len(state["roles"]), 6)
        self.assertFalse(self.output.exists())
        lock = self.scratch / "wrong-lock.json"
        for label, field in (("source", "source_candidate_sha256"),
                             ("binary", str(ROOT / "build/spec192-repo-client")),
                             ("design", "specs/202-distributedrepo-convergence/spec.md")):
            with self.subTest(label=label):
                changed = json.loads(json.dumps(state))
                if label == "source":
                    changed[field] = "wrong"
                else:
                    changed["files"][field] = "wrong"
                lock.write_text(json.dumps(changed), encoding="utf-8")
                self.reject("CANDIDATE_MISMATCH", lock_path=lock)

    def test_clean_source_inputs_remain_in_candidate(self):
        before, names = preflight.source_candidate(ROOT)
        self.assertIn("tests/main.cpp", names)
        self.assertIn("NDNSF-DistributedRepo/src/LegacyRepoImporter.cpp", names)
        self.assertIn("tests/fixtures/spec202-import-lock.hpp", names)
        self.assertIn("tests/minindn/spec193_packet_oracle.py", names)
        self.assertFalse(any("__pycache__" in name or "/evidence/" in name
                             for name in names))
        actual = preflight.sha256

        def changed(path):
            return "00" * 32 if path == ROOT / "tests/main.cpp" else actual(path)

        with mock.patch.object(preflight, "sha256", changed):
            after, same_names = preflight.source_candidate(ROOT)
        self.assertEqual(names, same_names)
        self.assertNotEqual(before, after)
        self.assertFalse(self.output.exists())

    def test_missing_source_is_rejected_before_output(self):
        self.reject("MISSING_SOURCE", source_files=["missing-native-input.cpp"])
        actual = Path.is_file

        def missing(path):
            return False if path == ROOT / "tests/main.cpp" else actual(path)

        with mock.patch.object(Path, "is_file", missing):
            with self.assertRaises(preflight.PreflightError) as caught:
                preflight.source_candidate(ROOT)
        self.assertEqual(caught.exception.failure_class, "MISSING_SOURCE")
        self.assertFalse(self.output.exists())

    def test_source_hash_and_inventory_cannot_be_omitted(self):
        self.reject("SOURCE_CANDIDATE_MISMATCH", source_sha="stale-source")
        self.reject("SOURCE_CANDIDATE_MISMATCH", source_files=[])

    def test_deleted_tracked_source_is_rejected(self):
        actual = preflight.subprocess.check_output
        for name, registration in (
                ("tests/integration-tests/spec192-removed-fixture.cpp", "index"),
                ("tests/unit-tests/distributed-repo-removed.t.cpp", "index"),
                ("ndn-service-framework/staged-removed.cpp", "HEAD")):
            with self.subTest(name=name, registration=registration):
                def registered(command, **kwargs):
                    if registration == "HEAD" and command[1] == "ls-tree":
                        return (name + "\0").encode()
                    if registration == "index" and command[1] == "ls-files":
                        return (name + "\0").encode()
                    return actual(command, **kwargs)

                with mock.patch.object(preflight.subprocess, "check_output", registered):
                    with self.assertRaises(preflight.PreflightError) as caught:
                        preflight.source_candidate(ROOT)
                self.assertEqual(caught.exception.failure_class, "MISSING_SOURCE")
                self.assertFalse(self.output.exists())

    def test_seven_host_unselected_repo_role(self):
        roles = dict(ROLES, stanford="repoD")
        users = USERS[:5] + ["/example/repo/provider/repoD"] + USERS[5:]
        providers = PROVIDERS + ["/example/repo/provider/repoD"]
        options = dict(topology=ROOT / "tests/minindn/spec193_seven_host.conf",
                       roles=roles, repo_users=users, repo_providers=providers,
                       require_six=False, require_seven=True)
        state = self.check(**options)
        self.assertEqual(len(state["roles"]), 7)
        self.assertFalse(self.output.exists())
        self.reject("SEVEN_HOST_ROLE", **dict(options, roles=ROLES))

    def test_missing_client_server_extension(self):
        actual = Path.is_file
        for kind, expected in (("client", "MISSING_CLIENT"),
                               ("server", "MISSING_SERVER"),
                               ("extension", "MISSING_EXTENSION"),
                               ("base_extension", "MISSING_BASE_EXTENSION"),
                               ("object_server", "MISSING_OBJECT_SERVER")):
            with self.subTest(kind=kind):
                def missing(path, kind=kind):
                    name = str(path)
                    if kind == "client" and name.endswith("/build/spec192-repo-client"):
                        return False
                    if kind == "server" and name.endswith((
                            "/build/NDNSF-DistributedRepo/DistributedRepoServer",
                            "/build/DistributedRepoServer")):
                        return False
                    if kind == "extension" and "_py_repoclient" in name and name.endswith(".so"):
                        return False
                    if kind == "base_extension" and "_ndnsf" in name and name.endswith(".so"):
                        return False
                    if kind == "object_server" and name.endswith("/build/spec193-object-repo-server"):
                        return False
                    return actual(path)
                with mock.patch.object(Path, "is_file", missing):
                    self.reject(expected, object_mode=kind == "object_server")

    def test_topology_role_and_link_rejections(self):
        self.reject("DUPLICATE_ROLE", roles=dict(ROLES, seattle="repoB"))
        topology = self.scratch / "wrong.conf"
        topology.write_text(self.topology.read_text().replace("seattle:\n", ""),
                            encoding="utf-8")
        self.reject("UNKNOWN_LINK_NODE", topology=topology)
        topology.write_text(self.topology.read_text().replace(
            "memphis:seattle", "memphis:unknown"), encoding="utf-8")
        self.reject("UNKNOWN_LINK_NODE", topology=topology)

    def test_acl_and_output_rejections(self):
        services = [PREFIX + "/" + operation for operation in OPERATIONS[:-1]]
        self.reject("ACL_SERVICES", acl_services=services)
        self.output.mkdir()
        self.reject("OUTPUT_EXISTS", output_exists=True)

    def test_malformed_policy_template(self):
        actual = Path.read_text

        def malformed(path, *args, **kwargs):
            if str(path).endswith("/generic_object_store/repo_policy.yaml"):
                return "[]"
            return actual(path, *args, **kwargs)

        with mock.patch.object(Path, "read_text", malformed):
            self.reject("POLICY_TEMPLATE")

    def test_missing_packet_oracle_module(self):
        actual = preflight.importlib.util.find_spec

        def missing(name):
            return None if name == "cryptography" else actual(name)

        with mock.patch.object(preflight.importlib.util, "find_spec", missing):
            self.reject("MISSING_PYTHON_MODULE")

    def test_object_server_rejects_invalid_mode_before_state(self):
        target = self.scratch / "must-not-create"
        binary = ROOT / "build/spec193-object-repo-server"
        result = subprocess.run([
            str(binary), "--repo-node", "/example/repo/node/repoA",
            "--database", str(target / "repo.sqlite"),
            "--group-prefix", "/example/repo/group",
            "--provider-identity", "/example/repo/provider/repoA",
            "--authority-identity", "/example/repo/controller",
            "--trust-schema", str(ROOT / "examples/trust-any.conf"),
            "--repo-mode", "bogus",
        ], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(result.returncode, 2)
        self.assertIn("rejects mode: bogus", result.stderr)
        self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()
