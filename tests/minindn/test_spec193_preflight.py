"""Mutation probes for Spec202's shared no-side-effect MiniNDN admission gate."""

import hashlib
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

    def test_retired_cohort_inputs_cannot_change_or_disappear(self):
        before, names = preflight.source_candidate(ROOT)
        actual = preflight.sha256
        for name in ("tests/python/test_spec164_migration_rollback.py",
                     "tests/python/retired_repo_fixture.py",
                     "tests/python/test_ndnsf_core_boundary_imports.py"):
            with self.subTest(name=name):
                self.assertIn(name, names)
                with mock.patch.object(preflight, "sha256", side_effect=lambda path:
                        "00" * 32 if path == ROOT / name else actual(path)):
                    changed, same_names = preflight.source_candidate(ROOT)
                self.assertEqual(names, same_names)
                self.assertNotEqual(before, changed)
        self.assertFalse(self.output.exists())

    def test_core_conformance_inputs_cannot_change_or_disappear(self):
        before, names = preflight.source_candidate(ROOT)
        actual_hash = preflight.sha256
        actual_file = Path.is_file
        for name in ("tests/boost-test.hpp",
                     "tests/unit-tests/spec190-ack-window.t.cpp",
                     "tests/unit-tests/generic-dynamic-api-fixture.hpp"):
            with self.subTest(name=name):
                self.assertIn(name, names)
                with mock.patch.object(preflight, "sha256", side_effect=lambda path:
                        "00" * 32 if path == ROOT / name else actual_hash(path)):
                    changed, same_names = preflight.source_candidate(ROOT)
                self.assertEqual(names, same_names)
                self.assertNotEqual(before, changed)
                with mock.patch.object(Path, "is_file", lambda path:
                        False if path == ROOT / name else actual_file(path)):
                    with self.assertRaises(preflight.PreflightError) as caught:
                        preflight.source_candidate(ROOT)
                self.assertEqual(caught.exception.failure_class, "MISSING_SOURCE")
        self.assertFalse(self.output.exists())

    def test_svs_implementation_changes_invalidate_sdk_inventory(self):
        from spec202_build_candidate import sdk_inputs
        source = Path("/usr/local/src/spec192-ndn-svs-9f2d8a4")
        build = source / "build"
        before = sdk_inputs(source, build)
        actual_hash = preflight.sha256
        actual_file = Path.is_file
        for path in (source / "ndn-svs/svspubsub.cpp", source / "wscript"):
            with self.subTest(path=path):
                self.assertIn(str(path), before)
                with mock.patch.object(preflight, "sha256", side_effect=lambda candidate:
                        "00" * 32 if candidate == path else actual_hash(candidate)):
                    self.assertNotEqual(before, sdk_inputs(source, build))
                with mock.patch.object(Path, "is_file", lambda candidate:
                        False if candidate == path else actual_file(candidate)):
                    if path.suffix == ".cpp":
                        self.assertNotEqual(before, sdk_inputs(source, build))
                    else:
                        with self.assertRaises(preflight.PreflightError) as caught:
                            sdk_inputs(source, build)
                        self.assertEqual(caught.exception.failure_class, "MISSING_BUILD_INPUT")
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

    def test_missing_build_receipt_is_rejected(self):
        actual = Path.is_file

        def missing(path):
            return False if path.name == "spec202-source-build-receipt.json" else actual(path)

        with mock.patch.object(Path, "is_file", missing):
            self.reject("MISSING_BUILD_RECEIPT")

    def test_actual_pinned_svs_config_path_is_in_sdk_inventory(self):
        from spec202_build_candidate import sdk_inputs
        source = Path("/usr/local/src/spec192-ndn-svs-9f2d8a4")
        build = source / "build"
        inputs = sdk_inputs(source, build)
        self.assertIn(str(build / "config.hpp"), inputs)
        self.assertNotIn(str(build / "ndn-svs/config.hpp"), inputs)
        actual = Path.is_file

        def missing(path):
            if path in (build / "config.hpp", source / "ndn-svs/config.hpp"):
                return False
            return actual(path)

        with mock.patch.object(Path, "is_file", missing):
            with self.assertRaises(preflight.PreflightError) as caught:
                sdk_inputs(source, build)
        self.assertEqual(caught.exception.failure_class, "MISSING_BUILD_INPUT")
        self.assertFalse(self.output.exists())

    def test_build_receipt_mutations_fail_before_output(self):
        path = ROOT / "build/spec202-source-build-receipt.json"
        original = json.loads(path.read_text())
        actual = Path.read_text
        for label, expected in (("source", "STALE_BUILD_RECEIPT"),
                                ("artifact", "STALE_BUILD_RECEIPT"),
                                ("sdk", "STALE_BUILD_RECEIPT"),
                                ("targets", "INVALID_BUILD_RECEIPT"),
                                ("fresh-output", "INVALID_BUILD_RECEIPT"),
                                ("log", "STALE_BUILD_RECEIPT"),
                                ("binding-force", "INVALID_BUILD_RECEIPT"),
                                ("binding-owner", "INVALID_BUILD_RECEIPT"),
                                ("install-target", "INVALID_BUILD_RECEIPT"),
                                ("runtime-interpreter", "INVALID_BUILD_RECEIPT"),
                                ("runtime-path", "INVALID_BUILD_RECEIPT"),
                                ("compiler-dependencies", "STALE_BUILD_RECEIPT")):
            with self.subTest(label=label):
                changed = json.loads(json.dumps(original))
                if label == "source":
                    changed["source_before"] = "wrong"
                elif label == "artifact":
                    changed["artifacts"][str(ROOT / "build/spec192-repo-client")] = "wrong"
                elif label == "sdk":
                    changed["external_inputs"].pop(next(iter(changed["external_inputs"])))
                elif label == "targets":
                    changed["steps"][1]["argv"] = ["waf", "build", "--targets=DistributedRepoServer"]
                elif label == "fresh-output":
                    changed["fresh_outputs"].pop(next(iter(changed["fresh_outputs"])))
                elif label == "binding-force":
                    changed["steps"][4]["argv"].remove("--force")
                elif label == "binding-owner":
                    changed["steps"][3]["cwd"] = "/tmp/other-wrapper"
                elif label == "install-target":
                    changed["steps"][2]["argv"][-1] = "--targets=other-library"
                elif label == "runtime-interpreter":
                    changed["steps"][5]["argv"][0] = "/tmp/other-python"
                elif label == "runtime-path":
                    changed["steps"][5]["environment"]["PYTHONPATH"] = "/tmp/other-wrapper"
                elif label == "compiler-dependencies":
                    changed["compiler_dependencies"].pop(next(iter(changed["compiler_dependencies"])))
                else:
                    changed["steps"][0]["log_sha256"] = "wrong"

                def mutated(file, *args, **kwargs):
                    return json.dumps(changed) if file == path else actual(file, *args, **kwargs)

                with mock.patch.object(Path, "read_text", mutated):
                    self.reject(expected)

    def test_progress_only_git_checkpoint_preserves_candidate_identity(self):
        state = self.check()
        lock = self.scratch / "unchanged-composition-lock.json"
        lock.write_text(json.dumps(state))
        actual = preflight.subprocess.check_output

        def new_revision(command, **kwargs):
            if command == ["git", "rev-parse", "HEAD"]:
                return "f" * 40 + "\n"
            return actual(command, **kwargs)

        with mock.patch.object(preflight.subprocess, "check_output", new_revision):
            replay = self.check(lock_path=lock)
        self.assertNotEqual(state["source_revision"], replay["source_revision"])
        self.assertEqual(state["candidate_lock_sha256"], replay["candidate_lock_sha256"])
        self.assertFalse(self.output.exists())

    def test_progress_replay_preserves_lock_but_acceptance_change_rejects(self):
        state = self.check()
        lock = self.scratch / "progress-replay-lock.json"
        lock.write_text(json.dumps(state))
        feature = ROOT / "specs/202-distributedrepo-convergence"
        tasks = feature / "tasks.md"
        matrix = feature / "validation-matrix.md"
        originals = {path: path.read_text() for path in (tasks, matrix)}
        changed = {
            tasks: originals[tasks].replace("| T009 | PARTIAL |", "| T009 | DONE |")
                .replace("- [ ] T010", "- [x] T010"),
            matrix: originals[matrix].replace("**Status**: NOT_RUN", "**Status**: PASS")
                .replace(" | NOT_RUN | NOT_RUN |", " | PASS | FAIL |"),
        }
        for path in changed:
            self.assertNotEqual(originals[path], changed[path])
        actual_read = Path.read_text
        actual_hash = preflight.sha256

        def read(path, *args, **kwargs):
            return changed[path] if path in changed else actual_read(path, *args, **kwargs)

        def digest(path):
            return hashlib.sha256(changed[path].encode()).hexdigest() \
                if path in changed else actual_hash(path)

        with mock.patch.object(Path, "read_text", read), \
                mock.patch.object(preflight, "sha256", digest):
            replay = self.check(lock_path=lock)
            self.assertEqual(state["candidate_lock_sha256"], replay["candidate_lock_sha256"])
            self.assertEqual(state["files"], replay["files"])
            for path in changed:
                name = str(path.relative_to(ROOT))
                self.assertNotEqual(state["progress_provenance"][name],
                                    replay["progress_provenance"][name])
            old = "59case及网络子场景两轮"
            self.assertIn(old, changed[tasks])
            changed[tasks] = changed[tasks].replace(old, "59case及网络子场景一轮")
            self.reject("CANDIDATE_MISMATCH", lock_path=lock)
            changed[tasks] = originals[tasks]
            old = "完整 SHA-256"
            self.assertIn(old, changed[matrix])
            changed[matrix] = changed[matrix].replace(old, "部分 SHA-256")
            self.reject("CANDIDATE_MISMATCH", lock_path=lock)
        self.assertFalse(self.output.exists())

    def test_deleted_tracked_source_is_rejected(self):
        actual = preflight.subprocess.check_output
        for name, registration in (
                ("tests/integration-tests/spec192-removed-fixture.cpp", "index"),
                ("tests/unit-tests/distributed-repo-removed.t.cpp", "index"),
                ("tests/python/test_spec164_removed.py", "index"),
                ("tests/python/test_spec164_removed.py", "HEAD"),
                ("tests/python/retired_repo_removed.py", "index"),
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


class BuildSourceClosureTests(unittest.TestCase):
    def test_build_entry_inputs_cannot_change_or_disappear(self):
        before, names = preflight.source_candidate(ROOT)
        actual_hash = preflight.sha256
        actual_file = Path.is_file
        for name in ("scripts/configure_dependencies.py", "examples/wscript",
                     "examples/trust-any.conf",
                     "pythonWrapper/pyproject.toml", "pythonWrapper/README.md",
                     "NDNSF-DistributedRepo/pythonWrapper/pyproject.toml",
                     "NDNSF-DistributedRepo/pythonWrapper/README.md",
                     "NDNSF-DistributedRepo/pythonWrapper/py_repoclient/py.typed"):
            with self.subTest(name=name):
                self.assertIn(name, names)
                with mock.patch.object(preflight, "sha256", side_effect=lambda path:
                        "00" * 32 if path == ROOT / name else actual_hash(path)):
                    changed, same_names = preflight.source_candidate(ROOT)
                self.assertEqual(names, same_names)
                self.assertNotEqual(before, changed)
                with mock.patch.object(Path, "is_file", lambda path:
                        False if path == ROOT / name else actual_file(path)):
                    with self.assertRaises(preflight.PreflightError) as caught:
                        preflight.source_candidate(ROOT)
                self.assertEqual(caught.exception.failure_class, "MISSING_SOURCE")


if __name__ == "__main__":
    unittest.main()
