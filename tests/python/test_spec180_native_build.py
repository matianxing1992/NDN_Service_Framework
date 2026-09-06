"""Host-local identity guard: temp files and mocked processes only."""
from __future__ import annotations

import copy
import importlib.util
import json
import os
from pathlib import Path
import runpy
import subprocess
from types import SimpleNamespace

import pytest


SCRIPT = Path(__file__).resolve().parents[2] / "scripts/spec180_native_build.py"
SPEC = importlib.util.spec_from_file_location("spec180_native_build", SCRIPT)
native = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(native)


def test_runtime_probe_isolates_keychains_before_exec_without_repairing_paths(tmp_path, monkeypatch):
    original = {"PYTHONPATH": "/chosen/python", "LD_LIBRARY_PATH": "/chosen/lib",
                "NDN_CLIENT_PIB": "pib-sqlite3:/operator/pib"}
    captured = {}
    def fake_run(command, *, cwd, env, **kwargs):
        captured.update(env)
        private = Path(env["NDN_CLIENT_PIB"].split(":", 1)[1]).parent
        assert private.is_dir() and private.stat().st_mode & 0o777 == 0o700
        assert env["NDN_CLIENT_TPM"] == "tpm-file:" + str(private / "tpm")
        assert env["NDN_CLIENT_TRANSPORT"] == "unix://" + str(private / "unused.sock")
        assert not (private / "unused.sock").exists()
        assert command[0] == "/selected/python"
        return 'SPEC180_NATIVE_PROBE={"ok":true}\n'
    monkeypatch.setattr(native, "run", fake_run)
    assert native.probe_runtime("/selected/python", tmp_path, original) == {"ok": True}
    assert captured["PYTHONPATH"] == original["PYTHONPATH"]
    assert captured["LD_LIBRARY_PATH"] == original["LD_LIBRARY_PATH"]
    assert original["NDN_CLIENT_PIB"] == "pib-sqlite3:/operator/pib"
    assert not Path(captured["NDN_CLIENT_PIB"].split(":", 1)[1]).parent.exists()


def test_runtime_probe_normalizes_uid_lookup_before_native_import(monkeypatch, capsys):
    calls = []
    monkeypatch.setattr(native.pwd, "getpwuid", lambda uid: calls.append("uid"))
    def imported(name):
        calls.append("native")
        return SimpleNamespace(__file__="/fixture/_ndnsf" +
                               native.sysconfig.get_config_var("EXT_SUFFIX"))
    monkeypatch.setattr(native.importlib, "import_module", imported)
    monkeypatch.setattr(native, "file_identity", lambda path: {"path": str(path)})
    monkeypatch.setattr(native, "mapped_shared_libraries", lambda maps: {})
    native.emit_runtime_probe()
    assert calls == ["uid", "native"]
    assert "SPEC180_NATIVE_PROBE=" in capsys.readouterr().out


@pytest.fixture
def local(tmp_path, monkeypatch):
    root = tmp_path / "checkout with spaces"
    build_dir = root / "build-system-j2"

    def put(relative, contents=b"fixture\n"):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    for name in native.SOURCE_FILES:
        put(name)
    put("waf", b'#!/usr/bin/env python3\nVERSION="2.0.24"\nREVISION="fixture"\nINSTALL=""\n')
    waf_dir = root / ".waf3-2.0.24-fixture"
    put(waf_dir / "waflib/__init__.py", b"# fixture Waf\n")
    put(waf_dir / "waflib/Scripting.py", b"# fixture build entry\n")
    for name in native.CONFIG_FILES:
        put("build-system-j2/" + name)
    for name in ("ndn-service-framework/ServiceController.cpp",
                 "ndn-service-framework/ServiceController.hpp",
                 "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp",
                 "pythonWrapper/src/ndnsf/_ndnsf.cpp",
                 "pythonWrapper/ndnsf/service.py"):
        put(name)
    core = put("build-system-j2/" + native.LIBRARY + ".0.1.0", b"core-v1")
    (build_dir / native.LIBRARY).symlink_to(core.name)
    extension = put("pythonWrapper/ndnsf/_ndnsf.fixture.so", b"extension-v1")
    provider = put("build-system-j2/examples/di-native-provider", b"provider-v1")
    provider.chmod(0o755)
    dependency = put("system/libdependency.so", b"dependency-v1")
    svs_source = tmp_path / "selected-svs"
    svs_build = svs_source / "build"
    put(svs_source / "ndn-svs/svspubsub.hpp", b"selected header")
    put(svs_source / "ndn-svs/detail.hpp", b"selected internal header")
    put(svs_build / "config.hpp", b"selected generated configuration")
    svs_library = put(svs_build / native.SVS_LIBRARY, b"selected svs library")
    toolchain_root = tmp_path / "system-bin"
    for name in ("gcc", "g++", "ld"):
        put(toolchain_root / name, ("system tool " + name).encode())
    monkeypatch.setattr(native, "SETUP_TOOLCHAIN_ROOT", toolchain_root)
    put("build-system-j2/c4che/_cache.py", (
        native.SVS_SOURCE_ENV + " = " + repr(str(svs_source)) + "\n" +
        native.SVS_BUILD_ENV + " = " + repr(str(svs_build)) + "\n").encode())
    python = put("system/python", b"python-v1")
    python.chmod(0o755)
    (python.parent / "python3").symlink_to(python.name)
    manifest = root / native.DEFAULT_MANIFEST
    calls = []
    state = {"framework": core, "extension": extension,
             "provider_dependency": dependency, "svs": svs_library,
             "provider_svs": svs_library, "linker": toolchain_root / "ld"}
    env = {"PATH": str(python.parent), "PYTHONPATH": str(root / "pythonWrapper"),
           "LD_LIBRARY_PATH": str(root / "system")}

    def runtime():
        return {
            "python": {"executable": native.file_identity(python),
                       "prefix": str(root / "system"), "version": "fixture",
                       "soabi": "fixture", "ext_suffix": ".fixture.so"},
            "extension": native.file_identity(state["extension"]),
            "mapped_libraries": {
                str(p.resolve()): native.file_identity(p)
                for p in (state["framework"], state["extension"], state["svs"], dependency)},
        }

    def fake_run(command, *, cwd, env, capture=False, timeout=None):
        calls.append({"command": command, "cwd": cwd, "env": dict(env)})
        if command[0] == str(root / "waf"):
            return None
        if "-print-prog-name=ld" in command:
            assert timeout == 10
            return str(state["linker"]) + "\n"
        if "setup.py" in command:
            extension.write_bytes(b"extension-built")
            return None
        if "-c" in command:
            assert timeout == 60
            return "ambient import log\nSPEC180_NATIVE_PROBE=" + json.dumps(runtime())
        if command[0] == "ldd":
            return ("libdependency.so => {} (0x1234)\n".format(state["provider_dependency"]) +
                    "libndn-svs.so => {} (0x5678)\n".format(state["provider_svs"]))
        pytest.fail("Unexpected subprocess (no real builds allowed): " + repr(command))

    monkeypatch.setattr(native, "run", fake_run)
    return {"root": root, "build_dir": build_dir, "manifest": manifest,
            "python": str(python), "env": env, "calls": calls, "state": state,
            "core": core, "extension": extension, "provider": provider,
            "dependency": dependency, "put": put, "runtime": runtime,
            "svs_source": svs_source, "svs_build": svs_build, "svs_library": svs_library,
            "toolchain_root": toolchain_root, "waf_dir": waf_dir}


def build(local, **kwargs):
    return native.build(*(local[k] for k in
                          ("root", "build_dir", "manifest", "python", "env")), **kwargs)


def verify(local, **kwargs):
    return native.verify_local_runtime(local["root"], python_executable=local["python"],
                                       environ=local["env"], **kwargs)


def test_unified_build_targets_and_binding_cwd(local):
    original_env = copy.deepcopy(local["env"])
    data = build(local)
    waf, setup, probe, ldd = [c for c in local["calls"]
                            if "-print-prog-name=ld" not in c["command"]]
    assert waf["command"] == [str(local["root"] / "waf"), "-o",
                              str(local["build_dir"]), "build", "-j1",
                              "--targets=ndn-service-framework,di-native-provider"]
    assert setup["command"] == [local["python"], "setup.py", "build_ext", "--inplace", "--force"]
    assert setup["cwd"] == local["root"] / "pythonWrapper"
    assert setup["env"]["NDNSF_LIBRARY_DIR"] == str(local["build_dir"])
    assert setup["env"][native.SVS_SOURCE_ENV] == str(local["svs_source"])
    assert setup["env"][native.SVS_BUILD_ENV] == str(local["svs_build"])
    assert {key: probe["env"][key] for key in original_env} == original_env == local["env"]
    assert ldd["command"] == ["ldd", str(local["provider"])]
    assert data["scope"] == "HOST_LOCAL_ONLY"
    assert "scripts/spec180_native_build.py" in data["sources"]
    assert data["runtime"]["extension"]["realpath"] == str(local["extension"])
    assert data["framework"]["realpath"] == str(local["core"])
    assert data["ndn_svs"]["library"] == native.file_identity(local["svs_library"])
    assert data == json.loads(local["manifest"].read_text())
    assert verify(local) == data


def test_auto_reuses_only_previously_bound_unchanged_extension(local):
    build(local)
    local["calls"].clear()
    data = build(local)
    assert data["binding_reused"] is True
    assert not any("setup.py" in c["command"] for c in local["calls"])
    assert local["calls"][0]["command"][-1] == "--targets=" + native.TARGETS
    assert verify(local) == data


@pytest.mark.parametrize("name", [
    "ndn-service-framework/ServiceController.cpp",
    "ndn-service-framework/ServiceController.hpp",
    "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp",
    "pythonWrapper/src/ndnsf/_ndnsf.cpp", "pythonWrapper/setup.py",
    "wscript", "examples/wscript", "scripts/spec180_native_build.py",
])
def test_source_hash_rejects_changes_even_with_preserved_timestamp(local, name):
    build(local)
    path = local["root"] / name
    st = path.stat()
    path.write_bytes(b"changed bytes\n")
    os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))
    local["calls"].clear()
    with pytest.raises(native.IdentityError, match="STALE_SOURCES"):
        verify(local)
    assert local["calls"] == []  # Fail even before a Python import or ldd.


@pytest.mark.parametrize("operation", ["add", "delete"])
def test_inventory_detects_added_or_deleted_headers(local, operation):
    build(local)
    if operation == "add":
        local["put"]("ndn-service-framework/NewHeader.hpp")
    else:
        (local["root"] / "ndn-service-framework/ServiceController.hpp").unlink()
    with pytest.raises(native.IdentityError, match="STALE_SOURCES"):
        verify(local)


def test_loaded_test_wscript_is_bound_even_for_native_targets(local):
    local["put"]("tests/wscript")
    build(local)
    path = local["root"] / "tests/wscript"
    before = path.stat()
    path.write_bytes(b"changed Waf build graph\n")
    os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
    local["calls"].clear()
    with pytest.raises(native.IdentityError, match="STALE_SOURCES"):
        verify(local)
    assert local["calls"] == []


def test_irrelevant_temp_logs_do_not_invalidate_deterministic_inventory(local):
    first = native.source_fingerprints(local["root"])
    local["put"](".workspace-tmp/foreign/ServiceController.cpp")
    local["put"]("results/native.log")
    local["put"]("pythonWrapper/ndnsf/__pycache__/cached.py")
    second = native.source_fingerprints(local["root"])
    assert first == second
    assert list(second) == sorted(second)


@pytest.mark.parametrize("kind", ["framework", "extension"])
def test_wrong_import_resolution_rejected_even_for_identical_bytes(local, kind):
    build(local)
    correct = local["state"][kind]
    wrong = local["put"]("wrong-prefix/" + correct.name, correct.read_bytes())
    local["state"][kind] = wrong
    error = "WRONG_FRAMEWORK_LIBRARY" if kind == "framework" else "WRONG_IMPORTED_EXTENSION"
    with pytest.raises(native.IdentityError, match=error):
        verify(local)


def test_extension_timestamp_does_not_excuse_replaced_core(local):
    build(local)
    unchanged_mtime = local["extension"].stat().st_mtime_ns
    local["core"].write_bytes(b"unexpected core bytes")
    with pytest.raises(native.IdentityError, match="FRAMEWORK_CHANGED"):
        verify(local)
    assert local["extension"].stat().st_mtime_ns == unchanged_mtime


def test_build_cannot_certify_wrong_python_framework(local):
    local["state"]["framework"] = local["put"](
        "system/" + native.LIBRARY, local["core"].read_bytes())
    with pytest.raises(native.IdentityError, match="WRONG_FRAMEWORK_LIBRARY"):
        build(local)
    assert not local["manifest"].exists()


def test_runner_provider_override_cannot_escape_bound_artifact(local):
    build(local)
    local["env"]["SPEC180_NATIVE_PROVIDER_BINARY"] = str(local["provider"])
    verify(local)
    local["env"]["SPEC180_NATIVE_PROVIDER_BINARY"] = str(local["put"](
        "another-build/di-native-provider", local["provider"].read_bytes()))
    with pytest.raises(native.IdentityError, match="WRONG_PROVIDER_OVERRIDE"):
        verify(local)


def test_provider_dependency_resolution_changes_rejected(local):
    build(local)
    local["state"]["provider_dependency"] = local["put"](
        "another-prefix/libdependency.so", local["dependency"].read_bytes())
    with pytest.raises(native.IdentityError, match="PROVIDER_LINKAGE_CHANGED"):
        verify(local)


def test_provider_resolving_wrong_framework_is_rejected(local, monkeypatch):
    wrong = local["put"]("wrong-prefix/" + native.LIBRARY, b"old core")
    original = native.run

    def wrong_ldd(command, **kwargs):
        if command[0] == "ldd":
            return "{} => {} (0x1234)\n".format(native.LIBRARY, wrong)
        return original(command, **kwargs)

    monkeypatch.setattr(native, "run", wrong_ldd)
    with pytest.raises(native.IdentityError, match="WRONG_PROVIDER_FRAMEWORK_LIBRARY"):
        build(local)
    assert not local["manifest"].exists()


@pytest.mark.parametrize("filename", native.CONFIG_FILES)
def test_configuration_changes_rejected(local, filename):
    build(local)
    (local["build_dir"] / filename).write_text("configuration changed")
    with pytest.raises(native.IdentityError, match="STALE_CONFIGURATION"):
        verify(local)


def test_core_header_change_forces_binding_rebuild(local):
    build(local)
    local["put"]("ndn-service-framework/ServiceController.hpp", b"new ABI")
    local["calls"].clear()
    data = build(local)
    assert not data["binding_reused"]
    assert any("--force" in c["command"] for c in local["calls"])


def test_always_binding_mode_forces_rebuild(local):
    build(local)
    local["calls"].clear()
    assert not build(local, binding="always")["binding_reused"]
    assert any("--force" in c["command"] for c in local["calls"])


def test_failed_build_preserves_previous_manifest_and_stops(local, monkeypatch):
    build(local)
    previous = local["manifest"].read_bytes()
    calls = []

    def fail(command, **kwargs):
        calls.append(command)
        raise subprocess.CalledProcessError(1, command)

    monkeypatch.setattr(native, "run", fail)
    with pytest.raises(subprocess.CalledProcessError):
        build(local)
    assert len(calls) == 1
    assert local["manifest"].read_bytes() == previous


def test_concurrent_source_change_during_build_cannot_publish_manifest(local, monkeypatch):
    original = native.run

    def edit_during_build(command, **kwargs):
        result = original(command, **kwargs)
        if "setup.py" in command:
            local["put"]("ndn-service-framework/ServiceController.hpp", b"concurrent edit")
        return result

    monkeypatch.setattr(native, "run", edit_during_build)
    with pytest.raises(native.IdentityError, match="SOURCES_CHANGED_DURING_BUILD"):
        build(local)
    assert not local["manifest"].exists()


def test_source_change_during_verification_is_rejected(local, monkeypatch):
    build(local)
    original = native.run

    def edit_after_probe(command, **kwargs):
        result = original(command, **kwargs)
        if command[0] == "ldd":
            local["put"]("ndn-service-framework/ServiceController.cpp", b"concurrent edit")
        return result

    monkeypatch.setattr(native, "run", edit_after_probe)
    with pytest.raises(native.IdentityError, match="SOURCES_CHANGED_DURING_VERIFY"):
        verify(local)


def test_absent_manifest_fails_without_processes_or_file_creation(local):
    before = sorted(local["root"].rglob("*"))
    with pytest.raises(native.IdentityError, match="LOCAL_MANIFEST_MISSING"):
        verify(local)
    assert not local["calls"]
    assert sorted(local["root"].rglob("*")) == before


def test_override_precedence_and_read_only_verification(local):
    data = build(local)
    saved = local["put"]("receipts/explicit.json", local["manifest"].read_bytes())
    local["env"][native.MANIFEST_ENV] = "receipts/explicit.json"
    local["manifest"].unlink()
    before = {p: p.read_bytes() for p in local["root"].rglob("*") if p.is_file()}
    assert verify(local) == data
    local["env"][native.MANIFEST_ENV] = "absent.json"
    assert verify(local, manifest_path=saved) == data
    assert before == {p: p.read_bytes() for p in local["root"].rglob("*") if p.is_file()}


def test_malformed_manifest_is_normalized_to_identity_error(local):
    local["put"](native.DEFAULT_MANIFEST, b'{"schema":')
    with pytest.raises(native.IdentityError, match="LOCAL_IDENTITY_INVALID"):
        verify(local)


def test_non_object_manifest_is_normalized_to_identity_error(local):
    local["put"](native.DEFAULT_MANIFEST, b'[]')
    with pytest.raises(native.IdentityError, match="WRONG_MANIFEST_SCOPE"):
        verify(local)


def test_ldd_missing_dependency_fails_closed():
    with pytest.raises(native.IdentityError, match="UNRESOLVED_PROVIDER_LIBRARY"):
        native.parse_ldd("libndn-service-framework.so => not found\n")


def test_proc_maps_paths_and_inode_identity(tmp_path):
    library = tmp_path / "path with space" / "libndn-service-framework.so.0.1.0"
    library.parent.mkdir()
    library.write_bytes(b"ELF fixture")
    st = library.stat()
    fields = "1000-2000 r-xp 0000 {:x}:{:x} {} ".format(
        os.major(st.st_dev), os.minor(st.st_dev), st.st_ino)
    line = fields + str(library).replace(" ", r"\040")
    assert native.mapped_shared_libraries(line) == {str(library): native.file_identity(library)}
    with pytest.raises(native.IdentityError, match="MAPPED_LIBRARY_REPLACED"):
        native.mapped_shared_libraries(line.replace(str(st.st_ino) + " ", "1 ", 1))
    with pytest.raises(native.IdentityError, match="MAPPED_LIBRARY_DELETED"):
        native.mapped_shared_libraries(line + " (deleted)")


def test_entrypoint_lock_refuses_concurrent_build(local):
    with native.local_lock(local["build_dir"]):
        with pytest.raises(native.IdentityError, match="LOCAL_BUILD_BUSY"):
            with native.local_lock(local["build_dir"]):
                pytest.fail("second build acquired the lock")


def test_cli_verify_missing_manifest_returns_failure(local, capsys):
    before = sorted(local["root"].rglob("*"))
    code = native.main(["verify", "--root", str(local["root"]),
                        "--python", "python3"])
    assert code == 1
    assert "SPEC180_NATIVE_IDENTITY_REJECTED" in capsys.readouterr().err
    assert local["calls"] == []
    assert sorted(local["root"].rglob("*")) == before


def test_cli_verify_detects_wrong_core_linkage(local, capsys, monkeypatch):
    build(local)
    monkeypatch.setenv("PATH", local["env"]["PATH"])
    local["state"]["framework"] = local["put"](
        "old-build/" + native.LIBRARY, b"old core")
    code = native.main(["verify", "--root", str(local["root"]),
                        "--python", "python3"])
    assert code == 1
    assert "WRONG_FRAMEWORK_LIBRARY" in capsys.readouterr().err


@pytest.mark.parametrize("kind", ["svs", "provider_svs"])
def test_reject_installed_svs_even_with_identical_library_bytes(local, kind):
    build(local)
    local["state"][kind] = local["put"](
        "installed/libndn-svs.so", local["svs_library"].read_bytes())
    error = "WRONG_RUNTIME_NDN_SVS" if kind == "svs" else "WRONG_PROVIDER_NDN_SVS"
    with pytest.raises(native.IdentityError, match=error):
        verify(local)


def test_build_cannot_bind_wrong_runtime_svs(local):
    local["state"]["svs"] = local["put"]("installed/libndn-svs.so", b"old SVS")
    with pytest.raises(native.IdentityError, match="WRONG_RUNTIME_NDN_SVS"):
        build(local)
    assert not local["manifest"].exists()


@pytest.mark.parametrize("relative", ["ndn-svs/svspubsub.hpp", "ndn-svs/detail.hpp",
                                      "build/config.hpp", "build/libndn-svs.so"])
def test_svs_input_change_rejected_before_import(local, relative):
    build(local)
    (local["svs_source"] / relative).write_bytes(b"changed selected SVS input")
    local["calls"].clear()
    with pytest.raises(native.IdentityError, match="STALE_NDN_SVS_INPUTS"):
        verify(local)
    assert local["calls"] == []


def test_svs_header_change_forces_binding_rebuild(local):
    build(local)
    (local["svs_source"] / "ndn-svs/detail.hpp").write_bytes(b"new selected SVS ABI")
    local["calls"].clear()
    data = build(local)
    assert not data["binding_reused"]
    assert any("--force" in c["command"] for c in local["calls"])


def test_helper_uses_waf_pair_instead_of_ambient_pair(local):
    local["env"].update({native.SVS_SOURCE_ENV: "/wrong/source",
                         native.SVS_BUILD_ENV: "/wrong/build"})
    build(local)
    setup = next(c for c in local["calls"] if "setup.py" in c["command"])
    assert setup["env"][native.SVS_SOURCE_ENV] == str(local["svs_source"])
    assert setup["env"][native.SVS_BUILD_ENV] == str(local["svs_build"])
    assert local["env"][native.SVS_SOURCE_ENV] == "/wrong/source"


def test_helper_rejects_missing_waf_pair_before_build(local):
    (local["build_dir"] / "c4che/_cache.py").write_text("CXX = ['/usr/bin/g++']\n")
    with pytest.raises(native.IdentityError, match="WAF_SVS_PAIR_REQUIRED"):
        build(local)
    assert local["calls"] == []


def test_waf_cache_is_parsed_without_executing_code(local):
    sentinel = local["root"] / "must-not-exist"
    (local["build_dir"] / "c4che/_cache.py").write_text(
        native.SVS_SOURCE_ENV + " = __import__('pathlib').Path(" + repr(str(sentinel)) +
        ").write_text('executed')\n")
    with pytest.raises(native.IdentityError, match="WAF_SVS_PAIR_INVALID_LITERAL"):
        build(local)
    assert not sentinel.exists()
    assert local["calls"] == []


def test_pre_svs_manifest_is_rejected(local):
    data = build(local)
    del data["ndn_svs"]
    local["manifest"].write_text(json.dumps(data))
    with pytest.raises(native.IdentityError, match="INVALID_MANIFEST_FIELD: ndn_svs"):
        verify(local)


def capture_setup(local, monkeypatch):
    import setuptools
    captured = {}
    calls = []
    monkeypatch.setattr(setuptools, "setup", lambda **kw: captured.update(kw))

    def pkg_config(command, **kwargs):
        calls.append(command)
        # Include a transitive SVS library flag to prove the absolute selected
        # library wins even if other .pc files mention the installed SVS.
        return "-I/usr/local/include -L/usr/local/lib -lndn-cxx -lndn-svs -lnac-abe"

    monkeypatch.setattr(subprocess, "check_output", pkg_config)
    monkeypatch.setenv("NDNSF_LIBRARY_DIR", str(local["build_dir"]))
    runpy.run_path(str(SCRIPT.parents[1] / "pythonWrapper/setup.py"), run_name="__main__")
    return captured["ext_modules"][0], calls


def test_setup_explicit_svs_pair_binds_headers_exact_library_and_runpath(local, monkeypatch):
    monkeypatch.setenv(native.SVS_SOURCE_ENV, str(local["svs_source"]))
    monkeypatch.setenv(native.SVS_BUILD_ENV, str(local["svs_build"]))
    extension, calls = capture_setup(local, monkeypatch)
    assert extension.include_dirs[:2] == [str(local["svs_source"]), str(local["svs_build"])]
    assert extension.extra_objects == [str(local["svs_library"])]
    assert "ndn-svs" not in extension.libraries
    assert all("libndn-svs" not in command for command in calls)
    assert extension.extra_link_args[0] == "-Wl,-rpath," + str(local["svs_build"])
    assert "-Wl,-rpath," + str(local["build_dir"]) in extension.extra_link_args
    assert extension.library_dirs[0] == str(local["build_dir"])
    assert "ndn-service-framework" in extension.libraries


def test_setup_without_pair_preserves_pkg_config_and_ndnsf_library_dir(local, monkeypatch):
    monkeypatch.delenv(native.SVS_SOURCE_ENV, raising=False)
    monkeypatch.delenv(native.SVS_BUILD_ENV, raising=False)
    extension, calls = capture_setup(local, monkeypatch)
    assert extension.extra_objects == []
    assert "libndn-svs" in calls[0]
    assert "ndn-svs" in extension.libraries
    assert extension.library_dirs[0] == str(local["build_dir"])
    assert extension.extra_link_args[0] == "-Wl,-rpath," + str(local["build_dir"])


@pytest.mark.parametrize("missing", [native.SVS_SOURCE_ENV, native.SVS_BUILD_ENV])
def test_setup_rejects_half_pair(local, monkeypatch, missing):
    monkeypatch.setenv(native.SVS_SOURCE_ENV, str(local["svs_source"]))
    monkeypatch.setenv(native.SVS_BUILD_ENV, str(local["svs_build"]))
    monkeypatch.delenv(missing)
    with pytest.raises(RuntimeError, match="must be supplied together"):
        capture_setup(local, monkeypatch)


@pytest.mark.parametrize("missing", ["ndn-svs/svspubsub.hpp", "build/config.hpp", "build/libndn-svs.so"])
def test_setup_rejects_missing_pair_files(local, monkeypatch, missing):
    monkeypatch.setenv(native.SVS_SOURCE_ENV, str(local["svs_source"]))
    monkeypatch.setenv(native.SVS_BUILD_ENV, str(local["svs_build"]))
    (local["svs_source"] / missing).unlink()
    with pytest.raises(RuntimeError, match="missing required file"):
        capture_setup(local, monkeypatch)


def test_default_setup_toolchain_is_explicit_system_gcc():
    assert native.setup_toolchain_environment() == {
        "CC": "/usr/bin/gcc -B/usr/bin",
        "CXX": "/usr/bin/g++ -B/usr/bin",
        "LDSHARED": "/usr/bin/g++ -B/usr/bin -shared",
    }


def test_setup_pins_and_records_drivers_without_changing_parent_environment(local):
    local["env"].update({"CC": "/brew/gcc", "CXX": "/brew/g++",
                         "LDSHARED": "/brew/g++ -shared", "PATH": "/brew/bin:/usr/bin"})
    original = dict(local["env"])
    data = build(local)
    overrides = native.setup_toolchain_environment()
    setup = next(c for c in local["calls"] if "setup.py" in c["command"])
    receipt = next(c for c in data["commands"] if "setup.py" in c["argv"])
    for name, expected in overrides.items():
        assert setup["env"][name] == expected == receipt[name]
        assert data["setup_toolchain"]["environment"][name] == expected
        assert data["setup_toolchain"]["drivers"][name]["linker"] == native.file_identity(
            local["toolchain_root"] / "ld")
    assert local["env"] == original
    assert local["calls"][0]["env"] == dict(original, WAFDIR=str(local["waf_dir"]))
    assert all({key: c["env"][key] for key in original} == original
               for c in local["calls"] if "-c" in c["command"])


def test_brew_linker_resolution_rejected_before_setup(local):
    local["state"]["linker"] = local["put"]("linuxbrew/bin/ld", b"wrong linker")
    with pytest.raises(native.IdentityError, match="WRONG_SETUP_LINKER"):
        build(local)
    assert not any("setup.py" in c["command"] for c in local["calls"])
    assert not local["manifest"].exists()


def test_verify_rejects_new_brew_linker_selection(local):
    build(local)
    local["state"]["linker"] = local["put"]("linuxbrew/bin/ld", b"wrong linker")
    local["calls"].clear()
    with pytest.raises(native.IdentityError, match="WRONG_SETUP_LINKER"):
        verify(local)
    assert not any("-c" in c["command"] for c in local["calls"])


@pytest.mark.parametrize("tool", ["gcc", "g++", "ld"])
def test_verify_rejects_changed_toolchain_binary(local, tool):
    build(local)
    (local["toolchain_root"] / tool).write_bytes(b"replaced tool")
    with pytest.raises(native.IdentityError, match="STALE_SETUP_TOOLCHAIN"):
        verify(local)


def test_build_flags_are_inherited_recorded_and_do_not_change_waf(local):
    local["env"].update({"CFLAGS": "-O0 -g0", "CXXFLAGS": "-O0 -g0"})
    before_config = native.config_fingerprints(local["build_dir"])
    data = build(local)
    setup = next(c for c in local["calls"] if "setup.py" in c["command"])
    receipt = next(c for c in data["commands"] if "setup.py" in c["argv"])
    for name in ("CFLAGS", "CXXFLAGS"):
        assert setup["env"][name] == receipt[name] == data["setup_build_flags"][name] == "-O0 -g0"
        assert name not in local["calls"][0]["env"]
    assert native.config_fingerprints(local["build_dir"]) == before_config
    # Build-only flags need not be exported into runtime verification.
    del local["env"]["CFLAGS"]
    del local["env"]["CXXFLAGS"]
    assert verify(local) == data


def test_changed_build_flags_prevent_extension_reuse(local):
    build(local)
    local["env"].update({"CFLAGS": "-O0 -g0", "CXXFLAGS": "-O0 -g0"})
    local["calls"].clear()
    assert not build(local)["binding_reused"]
    assert any("setup.py" in c["command"] for c in local["calls"])


def test_unset_flags_do_not_change_default_optimization(local):
    data = build(local)
    setup = next(c for c in local["calls"] if "setup.py" in c["command"])
    assert data["setup_build_flags"] == {name: None for name in native.SETUP_FLAG_NAMES}
    assert not any(name in setup["env"] for name in native.SETUP_FLAG_NAMES)


def test_old_manifest_without_toolchain_receipt_is_rejected(local):
    data = build(local)
    del data["setup_toolchain"]
    local["manifest"].write_text(json.dumps(data))
    with pytest.raises(native.IdentityError, match="INVALID_MANIFEST_FIELD: setup_toolchain"):
        verify(local)


@pytest.mark.parametrize("mutation", ["edit", "add", "delete", "override", "python"])
def test_waf_runtime_identity_drift_rejected_before_native_import(local, mutation):
    build(local)
    source = local["waf_dir"] / "waflib/Scripting.py"
    if mutation == "edit":
        source.write_text("# changed build implementation\n")
    elif mutation == "add":
        source.with_name("new_tool.py").write_text("# added tool\n")
    elif mutation == "delete":
        source.unlink()
    elif mutation == "override":
        other = local["root"] / "alternate-waf"
        local["put"](other / "waflib/__init__.py")
        local["put"](other / "waflib/Scripting.py")
        local["env"]["WAFDIR"] = str(other)
    else:
        Path(local["python"]).write_bytes(b"changed Waf interpreter")
    local["calls"].clear()
    with pytest.raises(native.IdentityError, match="WAF_TOOL_CHANGED"):
        verify(local)
    assert not local["calls"]


def test_waf_build_pins_selected_directory_without_mutating_parent(local):
    original = dict(local["env"])
    result = build(local)
    assert local["env"] == original
    assert local["calls"][0]["env"]["WAFDIR"] == str(local["waf_dir"])
    assert result["waf_tool"]["directory"] == str(local["waf_dir"])
    assert result["waf_tool"]["files"]["waflib/Scripting.py"] == native.file_identity(
        local["waf_dir"] / "waflib/Scripting.py")
    assert result["commands"][0]["WAFDIR"] == str(local["waf_dir"])


@pytest.mark.parametrize("during", ["build", "verify"])
def test_waf_change_during_operation_preserves_previous_receipt(local, monkeypatch, during):
    build(local)
    previous = local["manifest"].read_bytes()
    original = native.run
    def change_waf(command, **kwargs):
        result = original(command, **kwargs)
        if (during == "build" and command[0] == str(local["root"] / "waf")) or (
                during == "verify" and "-c" in command):
            (local["waf_dir"] / "waflib/Scripting.py").write_text("# drift\n")
        return result
    monkeypatch.setattr(native, "run", change_waf)
    with pytest.raises(native.IdentityError, match="WAF_TOOL_CHANGED_DURING_" + during.upper()):
        (build if during == "build" else verify)(local)
    assert local["manifest"].read_bytes() == previous


def test_waf_relative_path_is_resolved_in_build_working_directory(local):
    env = dict(local["env"], PATH="system", WAFDIR=local["waf_dir"].name)
    result = native.waf_tool_identity(local["root"], env)
    assert result["python"] == native.file_identity(local["root"] / "system/python3")
    assert result["directory"] == str(local["waf_dir"])


def test_old_receipt_without_waf_identity_fails_before_probe(local):
    result = build(local)
    del result["waf_tool"]
    local["manifest"].write_text(json.dumps(result))
    local["calls"].clear()
    with pytest.raises(native.IdentityError, match="INVALID_MANIFEST_FIELD: waf_tool"):
        verify(local)
    assert not local["calls"]
