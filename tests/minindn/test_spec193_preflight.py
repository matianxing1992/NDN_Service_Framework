"""Mutation probes for Spec202's shared no-side-effect MiniNDN admission gate."""

import hashlib
import json
import subprocess
import sys
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
        self.subject = self.scratch / "candidate"
        self.receipt = self.subject / "spec202-source-build-receipt.json"
        self.topology = ROOT / "tests/minindn/six_host.conf"

    def fixture_receipt(self, source_sha, source_files):
        """Minimal deterministic parser input, separate from the real build check."""
        build = self.subject / "build"
        installed = self.subject / "prefix/lib"
        sdk = self.subject / "sdk"
        configuration = (build / "config.hpp", build / "c4che/_cache.py")
        paths = [*(build / name for name in preflight.BUILD_OUTPUTS), *configuration,
                 *(installed / name for name in preflight.INSTALLED_BUILD_OUTPUTS),
                 self.subject / "python/ndnsf/_ndnsf-test.so",
                 self.subject / "python/py_repoclient/_py_repoclient-test.so", sdk]
        for path in paths:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"synthetic receipt parser input")
        commands = preflight.candidate_build_commands(
            ROOT, build, None, installed, sys.executable)
        steps = []
        for index, (label, command, cwd) in enumerate(commands):
            log = self.subject / (label + ".log")
            log.write_text("synthetic build step\n")
            environment = preflight.candidate_build_environment(
                build, None, preflight.dependency_prefix("libndn-svs.so") / "lib",
                sdk, bindings=index >= 3)
            steps.append(dict(step=label, argv=command, cwd=str(cwd),
                              exit_status=0, environment=environment,
                              log=str(log), log_sha256=preflight.sha256(log)))
        artifacts = {str(path): preflight.sha256(path) for path in paths}
        receipt = {
            "spec": "202", "source_before": source_sha, "source_after": source_sha,
            "source_files": source_files, "steps": steps, "artifacts": artifacts,
            "configuration": {str(path): artifacts[str(path)] for path in configuration},
            "fresh_outputs": {str(build / name): artifacts[str(build / name)]
                              for name in preflight.BUILD_OUTPUTS},
            "runtime": {str(sdk): artifacts[str(sdk)]},
            "external_inputs": {str(sdk): artifacts[str(sdk)]},
            "compiler_dependencies": {str(sdk): {"sha256": artifacts[str(sdk)]}},
        }
        self.receipt.write_text(json.dumps(receipt))

    def check(self, **changes):
        source_sha, source_files = preflight.source_candidate(ROOT)
        if not self.receipt.exists():
            self.fixture_receipt(source_sha, source_files)
        sdk = self.subject / "sdk"
        sdk_digest = preflight.sha256(sdk)
        arguments = dict(root=ROOT, output=self.output, topology=self.topology,
                         source_sha=source_sha, source_files=source_files, roles=ROLES,
                         service_prefix=PREFIX, operations=OPERATIONS,
                         repo_users=USERS, repo_providers=PROVIDERS,
                         require_six=True, build_receipt=self.receipt)
        arguments.update(changes)
        with mock.patch("spec202_build_candidate.sdk_inputs", return_value={str(sdk): sdk_digest}), \
             mock.patch("spec202_build_candidate.compiler_dependencies",
                        return_value={str(sdk): {"sha256": sdk_digest}}), \
             mock.patch.object(preflight, "resolved_libraries",
                               return_value={str(sdk): sdk_digest}):
            return preflight.check(**arguments)

    def reject(self, failure_class, output_exists=False, **changes):
        with self.assertRaises(preflight.PreflightError) as caught:
            self.check(**changes)
        self.assertEqual(caught.exception.failure_class, failure_class)
        self.assertEqual(self.output.exists(), output_exists)

    def sdk_fixture(self, *, source_config=False):
        source = self.scratch / "sdk-source"
        build = self.scratch / "sdk-build"
        cxx = self.scratch / "ndn-cxx-source"
        config = source / "ndn-svs/config.hpp" if source_config else build / "config.hpp"
        for path in (source / "wscript", source / "ndn-svs/svspubsub.cpp",
                     build / "libndn-svs.so", config, cxx / "ndn-cxx/data.hpp",
                     cxx / "ndn-cxx/security/pib/impl/pib-sqlite3.hpp"):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"synthetic SDK inventory input {path.name}\n")
        return source, build, cxx

    def test_good_candidate_and_bad_lock(self):
        state = self.check()
        self.assertEqual(len(state["roles"]), 6)
        self.assertFalse(self.output.exists())
        lock = self.scratch / "wrong-lock.json"
        for label, field in (("source", "source_candidate_sha256"),
                             ("binary", str(self.subject / "build/repo-client")),
                             ("harness", "tests/minindn/run_spec192_real_cpp_repo.py"),
                             ("historical-design", "specs/202-distributedrepo-convergence/spec.md")):
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
        self.assertIn("tests/fixtures/import-lock.hpp", names)
        self.assertIn("tests/minindn/packet_oracle.py", names)
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
                     "tests/unit-tests/ack-window.t.cpp",
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
        source, build, cxx = self.sdk_fixture()
        before = sdk_inputs(source, build, cxx)
        actual_hash = preflight.sha256
        actual_file = Path.is_file
        for path in (source / "ndn-svs/svspubsub.cpp", source / "wscript"):
            with self.subTest(path=path):
                self.assertIn(str(path), before)
                with mock.patch.object(preflight, "sha256", side_effect=lambda candidate:
                        "00" * 32 if candidate == path else actual_hash(candidate)):
                    self.assertNotEqual(before, sdk_inputs(source, build, cxx))
                with mock.patch.object(Path, "is_file", lambda candidate:
                        False if candidate == path else actual_file(candidate)):
                    if path.suffix == ".cpp":
                        self.assertNotEqual(before, sdk_inputs(source, build, cxx))
                    else:
                        with self.assertRaises(preflight.PreflightError) as caught:
                            sdk_inputs(source, build, cxx)
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
        self.check()
        actual = Path.is_file

        def missing(path):
            return False if path.name == "spec202-source-build-receipt.json" else actual(path)

        with mock.patch.object(Path, "is_file", missing):
            self.reject("MISSING_BUILD_RECEIPT")
        explicit = self.scratch / "selected-build-receipt.json"
        with self.assertRaises(preflight.PreflightError) as caught:
            self.check(build_receipt=explicit)
        self.assertEqual(caught.exception.failure_class, "MISSING_BUILD_RECEIPT")
        self.assertIn(str(explicit), str(caught.exception))
        self.assertFalse(self.output.exists())

    def test_compiler_header_content_changes_invalidate_dependencies(self):
        from spec202_build_candidate import compiler_dependencies
        fresh = Path(self.temp.name) / "fresh"
        sdk = Path(self.temp.name) / "sdk"
        fresh.mkdir()
        (sdk / "ndn-svs").mkdir(parents=True)
        svs_header = sdk / "ndn-svs/example.hpp"
        external_header = Path(self.temp.name) / "external.hpp"
        for path in (svs_header, external_header):
            path.write_text("original\n")
        (fresh / "example.o").write_bytes(b"")
        dep = fresh / "example.d"
        dep.write_text("example.o: " + str(svs_header) + " " + str(external_header) + "\n")
        before = compiler_dependencies(fresh, sdk, sdk / "build")
        self.assertIn(str(external_header), before[str(dep)]["inputs"])
        external_header.write_text("changed\n")
        after = compiler_dependencies(fresh, sdk, sdk / "build")
        self.assertEqual(before[str(dep)]["sha256"], after[str(dep)]["sha256"])
        self.assertNotEqual(before, after)
        external_header.unlink()
        with self.assertRaises(preflight.PreflightError):
            compiler_dependencies(fresh, sdk, sdk / "build")
        self.assertFalse(self.output.exists())

    def test_repo_binding_parent_objects_keep_compiler_working_directory(self):
        import spec202_build_candidate as builder
        root = Path(self.temp.name)
        fresh = root / "fresh"
        (fresh / "src").mkdir(parents=True)
        (root / "NDNSF-DistributedRepo/pythonWrapper").mkdir(parents=True)
        source = root / "NDNSF-DistributedRepo/src/example.cpp"
        source.parent.mkdir()
        source.write_text("original\n")
        sdk = root / "sdk"
        (sdk / "ndn-svs").mkdir(parents=True)
        header = sdk / "ndn-svs/example.hpp"
        header.write_text("original\n")
        (fresh / "src/example.o").write_bytes(b"")
        dep = fresh / "src/example.d"
        dep.write_text(str(fresh / "python-repo/../src/example.o") +
                       ": ../src/example.cpp " + str(header) + "\n")
        with mock.patch.object(builder, "ROOT", root):
            result = builder.compiler_dependencies(fresh, sdk, sdk / "build")
        self.assertIn(str(source), result[str(dep)]["inputs"])
        self.assertFalse(self.output.exists())

    def test_local_ndn_cxx_headers_are_bound_before_output(self):
        from spec202_build_candidate import sdk_inputs
        source, build, cxx = self.sdk_fixture(source_config=True)
        before = sdk_inputs(source, build, cxx)
        actual_hash = preflight.sha256
        actual_file = Path.is_file
        for relative in ("security/pib/impl/pib-sqlite3.hpp", "data.hpp"):
            path = cxx / "ndn-cxx" / relative
            with self.subTest(path=path):
                self.assertIn(str(path), before)
                with mock.patch.object(preflight, "sha256", side_effect=lambda candidate:
                        "00" * 32 if candidate == path else actual_hash(candidate)):
                    self.assertNotEqual(before, sdk_inputs(source, build, cxx))
                with mock.patch.object(Path, "is_file", lambda candidate:
                        False if candidate == path else actual_file(candidate)):
                    if relative == "data.hpp":
                        self.assertNotEqual(before, sdk_inputs(source, build, cxx))
                    else:
                        with self.assertRaises(preflight.PreflightError) as caught:
                            sdk_inputs(source, build, cxx)
                        self.assertEqual(caught.exception.failure_class, "MISSING_BUILD_INPUT")
                self.assertFalse(self.output.exists())

    def test_actual_pinned_svs_config_path_is_in_sdk_inventory(self):
        from spec202_build_candidate import sdk_inputs
        source, build, cxx = self.sdk_fixture()
        inputs = sdk_inputs(source, build, cxx)
        self.assertIn(str(build / "config.hpp"), inputs)
        self.assertNotIn(str(build / "ndn-svs/config.hpp"), inputs)
        actual = Path.is_file

        def missing(path):
            if path in (build / "config.hpp", source / "ndn-svs/config.hpp"):
                return False
            return actual(path)

        with mock.patch.object(Path, "is_file", missing):
            with self.assertRaises(preflight.PreflightError) as caught:
                sdk_inputs(source, build, cxx)
        self.assertEqual(caught.exception.failure_class, "MISSING_BUILD_INPUT")
        self.assertFalse(self.output.exists())

    def test_build_receipt_mutations_fail_before_output(self):
        self.check()
        path = self.receipt
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
                    changed["artifacts"][str(self.subject / "build/repo-client")] = "wrong"
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

    def test_runtime_source_admission_needs_no_local_spec(self):
        actual_open = Path.open

        def open_file(path, *args, **kwargs):
            if ROOT / ".specify" in path.parents or ROOT / "specs" in path.parents:
                raise AssertionError(f"runtime admission read a local Spec input: {path}")
            return actual_open(path, *args, **kwargs)

        with mock.patch.object(Path, "open", open_file):
            self.reject("SOURCE_CANDIDATE_MISMATCH", source_sha="0" * 64)
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
        options = dict(topology=ROOT / "tests/minindn/seven_host.conf",
                       roles=roles, repo_users=users, repo_providers=providers,
                       require_six=False, require_seven=True)
        state = self.check(**options)
        self.assertEqual(len(state["roles"]), 7)
        self.assertFalse(self.output.exists())
        self.reject("SEVEN_HOST_ROLE", **dict(options, roles=ROLES))

    def test_missing_client_server_extension(self):
        self.check()
        actual = Path.is_file
        for kind, expected in (("client", "MISSING_CLIENT"),
                               ("server", "MISSING_SERVER"),
                               ("extension", "MISSING_EXTENSION"),
                               ("base_extension", "MISSING_BASE_EXTENSION"),
                               ("object_server", "MISSING_OBJECT_SERVER")):
            with self.subTest(kind=kind):
                def missing(path, kind=kind):
                    name = str(path)
                    if kind == "client" and name.endswith("/build/repo-client"):
                        return False
                    if kind == "server" and name.endswith((
                            "/build/NDNSF-DistributedRepo/DistributedRepoServer",
                            "/build/DistributedRepoServer")):
                        return False
                    if kind == "extension" and "_py_repoclient" in name and name.endswith(".so"):
                        return False
                    if kind == "base_extension" and "_ndnsf" in name and name.endswith(".so"):
                        return False
                    if kind == "object_server" and name.endswith("/build/object-repo-server"):
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
        binary = ROOT / "build/object-repo-server"
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
