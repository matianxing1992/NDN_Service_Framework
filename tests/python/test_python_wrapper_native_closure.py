from __future__ import annotations

import os
import hashlib
import json
import runpy
from pathlib import Path

import setuptools
import pytest


ROOT = Path(__file__).resolve().parents[2]


def pkg_config_result(setup_path, includes, directories, libraries, link_args):
    if "NDNSF-DistributedRepo" in setup_path.parts:
        return includes, directories, libraries, [], link_args
    return includes, directories, libraries, link_args


@pytest.fixture(autouse=True)
def isolated_build_environment(monkeypatch):
    for name in ("NDNSF_LIBRARY_DIR", "NDNSF_NAC_ABE_PREFIX",
                 "NDNSF_NDN_SVS_SOURCE_TREE", "NDNSF_NDN_SVS_BUILD_TREE",
                 "NDNSF_RUNTIME_RPATH", "NDNSF_CONTAINER_BUILD", "NDNSF_RUNTIME_PREFIX"):
        monkeypatch.delenv(name, raising=False)
    native_libs = {
        name: hashlib.sha256((Path("/usr/local/lib") / name).read_bytes()).hexdigest()
        for name in ("libndn-service-framework.so", "libndnsf-distributed-inference.so",
                     "libndnsf-distributed-repo.a")
        if (Path("/usr/local/lib") / name).is_file()
    }
    if len(native_libs) == 3:
        monkeypatch.setenv("NDNSF_GLOBAL_NATIVE_DIGESTS",
                          json.dumps(native_libs, sort_keys=True))
    else:
        monkeypatch.delenv("NDNSF_GLOBAL_NATIVE_DIGESTS", raising=False)


@pytest.fixture(params=["pythonWrapper/setup.py", "NDNSF-DistributedRepo/pythonWrapper/setup.py"])
def setup_path(request):
    return ROOT / request.param


def test_declared_runtime_prefix_requires_matching_native_receipt(monkeypatch, tmp_path, setup_path):
    prefix = tmp_path / "runtime"
    library = prefix / "lib"
    library.mkdir(parents=True)
    names = ("libndn-service-framework.so", "libndnsf-distributed-inference.so",
             "libndnsf-distributed-repo.a")
    for name in names:
        (library / name).write_bytes(name.encode())
    receipt = {name: hashlib.sha256(name.encode()).hexdigest() for name in names}
    monkeypatch.setenv("NDNSF_RUNTIME_PREFIX", str(prefix))
    monkeypatch.setenv("NDNSF_LIBRARY_DIR", str(library))
    monkeypatch.setenv("NDNSF_GLOBAL_NATIVE_DIGESTS", json.dumps(receipt))
    captured = {}
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: captured.update(kwargs))
    namespace = runpy.run_path(str(setup_path))
    extension, = captured["ext_modules"]
    assert str(library) in extension.library_dirs
    assert "-Wl,-rpath," + str(library) in extension.extra_link_args
    reject = namespace["reject_non_global_dependency_paths"]
    reject([str(prefix / "include")], "runtime SDK")
    with pytest.raises(RuntimeError, match="undeclared dependency root"):
        reject([str(tmp_path / "other/include")], "unselected SDK")
    (library / "libndn-service-framework.so").write_bytes(b"replaced")
    with pytest.raises(RuntimeError, match="digest mismatch"):
        namespace["build_extension"]()
    monkeypatch.setenv("NDNSF_LIBRARY_DIR", "/usr/local/lib")
    with pytest.raises(RuntimeError, match="must match the declared host runtime root"):
        namespace["native_library_dirs"]()


@pytest.mark.parametrize("bad", ["relative", "/", "/dev/shm/runtime",
                                 "/opt/ndnsf runtime", "/opt/ndnsf\truntime",
                                 "/opt/ndnsf,runtime", "/opt/ndnsf'", '/opt/ndnsf"',
                                 "/opt/ndnsf\\runtime", "/opt/ndnsf#runtime"])
def test_invalid_runtime_prefix_rejected(monkeypatch, setup_path, bad):
    monkeypatch.setenv("NDNSF_RUNTIME_PREFIX", bad)
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    with pytest.raises(RuntimeError, match="NDNSF_RUNTIME_PREFIX"):
        runpy.run_path(str(setup_path))


def test_runtime_prefix_cannot_be_an_existing_file(monkeypatch, tmp_path, setup_path):
    prefix = tmp_path / "file"
    prefix.write_text("preserved")
    monkeypatch.setenv("NDNSF_RUNTIME_PREFIX", str(prefix))
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    with pytest.raises(RuntimeError, match="invalid NDNSF_RUNTIME_PREFIX"):
        runpy.run_path(str(setup_path))
    assert prefix.read_text() == "preserved"


@pytest.mark.parametrize("missing", [None, "include/nac-abe/consumer.hpp", "lib/libnac-abe.so"])
def test_explicit_nac_prefix_prevents_mixed_headers_and_library(monkeypatch, tmp_path, missing, setup_path):
    prefix = tmp_path / "nac"
    for name in ("include/nac-abe/consumer.hpp", "lib/libnac-abe.so"):
        path = prefix / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if name != missing:
            path.touch()
    monkeypatch.setenv("NDNSF_NAC_ABE_PREFIX", str(prefix))
    monkeypatch.delenv("NDNSF_LIBRARY_DIR", raising=False)
    monkeypatch.delenv("NDNSF_NDN_SVS_SOURCE_TREE", raising=False)
    monkeypatch.delenv("NDNSF_NDN_SVS_BUILD_TREE", raising=False)
    captured = {}
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: captured.update(kwargs))
    if missing is None:
        with pytest.raises(RuntimeError, match="resolved to undeclared dependency root"):
            runpy.run_path(str(setup_path))
        assert not captured
        return
    namespace = None
    if missing:
        with pytest.raises(RuntimeError, match="NDNSF_NAC_ABE_PREFIX is missing"):
            runpy.run_path(str(setup_path))
        assert not captured
        return
    build = namespace["build_extension"]
    monkeypatch.setitem(build.__globals__, "pkg_config", lambda *args: pkg_config_result(
        setup_path, ["/stale/include"], ["/stale/lib"], ["nac-abe", "ndn-cxx"], []))
    extension = build()
    assert extension.include_dirs[0] == str(prefix / "include")
    assert extension.library_dirs[0] == str(prefix / "lib")
    assert str(prefix / "lib/libnac-abe.so") in extension.extra_objects
    assert "nac-abe" not in extension.libraries
    assert f"-Wl,-rpath,{prefix / 'lib'}" in extension.extra_link_args


@pytest.mark.parametrize("bad_kind", ["library", "rpath", "undeclared"])
def test_historical_local_ndn_cxx_pkg_config_is_rejected(monkeypatch, setup_path, bad_kind):
    """A stale same-SONAME tree must not silently enter either binding."""
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    namespace = runpy.run_path(str(setup_path), run_name="__main__")
    local = ROOT / ".local-boost171" / "lib"
    if bad_kind == "library":
        pkg_config = lambda *args: pkg_config_result(setup_path, [str(local / "include")], [str(local)],
                                     ["ndn-cxx"], [])
    elif bad_kind == "rpath":
        pkg_config = lambda *args: pkg_config_result(setup_path, ["/usr/local/include"], ["/usr/local/lib"],
                                     ["ndn-cxx"], [f"-Wl,-rpath,{local}"])
    else:
        pkg_config = lambda *args: pkg_config_result(setup_path, ["/tmp/ndnsf/include"], ["/usr/local/lib"],
                                     ["ndn-cxx"], [])
    namespace["build_extension"].__globals__["pkg_config"] = pkg_config
    expected = ("resolved to undeclared dependency root"
                if bad_kind == "undeclared" else "resolved to retired")
    with pytest.raises(RuntimeError, match=expected):
        namespace["build_extension"]()


def test_container_stage_dependency_root_requires_explicit_marker(monkeypatch, setup_path):
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    namespace = runpy.run_path(str(setup_path), run_name="__main__")
    checker = namespace["reject_non_global_dependency_paths"]
    with pytest.raises(RuntimeError, match="undeclared dependency root"):
        checker(["/opt/ndnsf-stage"], "container prefix")
    monkeypatch.setenv("NDNSF_CONTAINER_BUILD", "1")
    checker(["/opt/ndnsf-stage"], "container prefix")


def test_container_base_dependency_root_requires_explicit_marker(monkeypatch, setup_path):
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    namespace = runpy.run_path(str(setup_path), run_name="__main__")
    checker = namespace["reject_non_global_dependency_paths"]
    with pytest.raises(RuntimeError, match="undeclared dependency root"):
        checker(["/opt/ndn-base"], "container base")
    monkeypatch.setenv("NDNSF_CONTAINER_BUILD", "1")
    checker(["/opt/ndn-base"], "container base")


def test_historical_local_runtime_rpath_is_rejected(monkeypatch, setup_path):
    """A runtime-only override must not reintroduce the retired tree."""
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    namespace = runpy.run_path(str(setup_path), run_name="__main__")
    local = ROOT / ".local-boost171" / "lib"
    monkeypatch.setenv("NDNSF_RUNTIME_RPATH", str(local))
    with pytest.raises(RuntimeError, match="resolved to retired"):
        namespace["build_extension"]()


def test_historical_local_symlink_rpath_is_rejected(monkeypatch, tmp_path, setup_path):
    """An alias to the retired tree must not bypass RPATH validation."""
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    namespace = runpy.run_path(str(setup_path), run_name="__main__")
    alias = tmp_path / "ndn-cxx-alias"
    alias.symlink_to(ROOT / ".local-boost171")
    namespace["build_extension"].__globals__["pkg_config"] = lambda *args: pkg_config_result(
        setup_path, ["/usr/local/include"], ["/usr/local/lib"], ["ndn-cxx"],
        [f"-Wl,-rpath,{alias / 'lib'}"])
    with pytest.raises(RuntimeError, match="resolved to retired"):
        namespace["build_extension"]()


def test_explicit_ndnsf_library_dir_is_an_exclusive_runtime_closure(
        monkeypatch, tmp_path, setup_path):
    """A checkout or per-run candidate must never become a runtime root."""

    candidate = (tmp_path / "candidate-lib").resolve()
    candidate.mkdir()
    (candidate / "libndn-service-framework.so").touch()
    (candidate / "libndnsf-distributed-inference.so").touch()
    monkeypatch.setenv("NDNSF_LIBRARY_DIR", str(candidate))
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    with pytest.raises(RuntimeError, match="resolved to undeclared dependency root"):
        runpy.run_path(str(setup_path), run_name="__main__")


def test_explicit_ndnsf_library_dir_rejects_silent_linker_fallback(
        monkeypatch, tmp_path, setup_path):
    captured: dict[str, object] = {}
    missing = tmp_path / "missing-candidate"
    monkeypatch.setenv("NDNSF_LIBRARY_DIR", str(missing))
    monkeypatch.setattr(
        setuptools,
        "setup",
        lambda **kwargs: captured.update(kwargs),
    )

    with pytest.raises(RuntimeError, match="resolved to undeclared dependency root"):
        runpy.run_path(str(setup_path), run_name="__main__")
    assert not captured


def test_runtime_rpath_override_targets_pair_locations(monkeypatch, tmp_path, setup_path):
    candidate = (tmp_path / "candidate-lib").resolve()
    candidate.mkdir()
    (candidate / "libndn-service-framework.so").touch()
    (candidate / "libndnsf-distributed-inference.so").touch()
    captured: dict[str, object] = {}
    monkeypatch.setenv("NDNSF_LIBRARY_DIR", str(candidate))
    monkeypatch.setenv("NDNSF_CONTAINER_BUILD", "1")
    monkeypatch.setenv("NDNSF_RUNTIME_RPATH", "$ORIGIN/../../lib:/opt/ndn-base/lib")
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: captured.update(kwargs))

    with pytest.raises(RuntimeError, match="resolved to undeclared dependency root"):
        runpy.run_path(str(setup_path), run_name="__main__")
    assert not captured


def test_host_runtime_rpath_rejects_temp_root(monkeypatch, tmp_path, setup_path):
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    namespace = runpy.run_path(str(setup_path), run_name="__main__")
    monkeypatch.setenv("NDNSF_RUNTIME_RPATH", str(tmp_path))
    with pytest.raises(RuntimeError, match="undeclared dependency root"):
        namespace["build_extension"]()


def test_host_origin_runtime_rpath_requires_container_marker(monkeypatch, setup_path):
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    namespace = runpy.run_path(str(setup_path), run_name="__main__")
    monkeypatch.setenv("NDNSF_RUNTIME_RPATH", "$ORIGIN/../../lib:/opt/ndn-base/lib")
    with pytest.raises(RuntimeError, match="NDNSF_CONTAINER_BUILD=1"):
        namespace["build_extension"]()


def test_host_pkg_config_path_rejects_temp_root(monkeypatch, tmp_path, setup_path):
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    namespace = runpy.run_path(str(setup_path), run_name="__main__")
    pkgconfig = tmp_path / "pkgconfig"
    pkgconfig.mkdir()
    monkeypatch.setenv("PKG_CONFIG_PATH", str(pkgconfig))
    with pytest.raises(RuntimeError, match="undeclared dependency root"):
        namespace["pkg_config"]("libndn-cxx")


def test_host_pkg_config_libdir_rejects_temp_root(monkeypatch, tmp_path, setup_path):
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    namespace = runpy.run_path(str(setup_path), run_name="__main__")
    pkgconfig = tmp_path / "pkgconfig-libdir"
    pkgconfig.mkdir()
    monkeypatch.setenv("PKG_CONFIG_LIBDIR", str(pkgconfig))
    with pytest.raises(RuntimeError, match="undeclared dependency root"):
        namespace["pkg_config"]("libndn-cxx")


def test_repo_pkg_config_preserves_compile_and_link_flags(monkeypatch):
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    namespace = runpy.run_path(str(ROOT / "NDNSF-DistributedRepo/pythonWrapper/setup.py"))
    calls = []

    def query(command, **kwargs):
        calls.append(command)
        if "--cflags" in command:
            return "-I/usr/local/include -DBOOST_LOG_DYN_LINK -pthread"
        return "-L/usr/local/lib -lndn-cxx -lboost_log -pthread"

    monkeypatch.setattr(namespace["subprocess"], "check_output", query)
    extension = namespace["build_extension"]()
    assert "-DBOOST_LOG_DYN_LINK" in extension.extra_compile_args
    assert "-DBOOST_LOG_DYN_LINK" not in extension.extra_link_args
    assert "-pthread" in extension.extra_compile_args
    assert "-pthread" in extension.extra_link_args
    assert "boost_log" in extension.libraries
    assert all(not ("--cflags" in command and "--libs" in command) for command in calls)


@pytest.mark.parametrize("candidate", [os.pathsep, os.pathsep * 2])
def test_explicit_ndnsf_library_dir_rejects_empty_candidate_list(
        monkeypatch, setup_path, candidate):
    captured: dict[str, object] = {}
    monkeypatch.setenv("NDNSF_LIBRARY_DIR", candidate)
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: captured.update(kwargs))

    with pytest.raises(RuntimeError, match="NDNSF_LIBRARY_DIR contains no library directory"):
        runpy.run_path(str(setup_path), run_name="__main__")

    assert not captured


@pytest.mark.parametrize("payload, expected", [
    ("{}", "digest mismatch"),
    ("not-json", "invalid JSON"),
])
def test_host_binding_requires_matching_global_native_digest(
        monkeypatch, setup_path, payload, expected):
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    monkeypatch.setenv("NDNSF_LIBRARY_DIR", "/usr/local/lib")
    monkeypatch.setenv("NDNSF_GLOBAL_NATIVE_DIGESTS", payload)
    with pytest.raises(RuntimeError, match=expected):
        runpy.run_path(str(setup_path), run_name="__main__")


def test_global_native_symlink_target_cannot_escape_root(
        monkeypatch, tmp_path, setup_path):
    outside = tmp_path / "outside.so"
    outside.write_bytes(b"outside")
    root = tmp_path / "runtime/lib"
    root.mkdir(parents=True)
    (root / "libndn-service-framework.so").symlink_to(outside)
    (root / "libndnsf-distributed-inference.so").write_bytes(b"di")
    (root / "libndnsf-distributed-repo.a").write_bytes(b"repo")
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    monkeypatch.setenv("NDNSF_RUNTIME_PREFIX", str(root.parent))
    monkeypatch.setenv("NDNSF_LIBRARY_DIR", str(root))
    with pytest.raises(RuntimeError, match="resolved to undeclared dependency root"):
        runpy.run_path(str(setup_path), run_name="__main__")


def test_host_native_library_dir_cannot_select_another_system_root(
        monkeypatch, setup_path):
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    monkeypatch.setenv("NDNSF_LIBRARY_DIR", "/usr/lib")
    with pytest.raises(RuntimeError, match="declared host runtime root"):
        runpy.run_path(str(setup_path), run_name="__main__")


def test_selected_nac_sdk_precedes_default_library_root(monkeypatch, tmp_path, setup_path):
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    namespace = runpy.run_path(str(setup_path), run_name="__main__")
    prefix = tmp_path / "nac-sdk"
    for relative in ("include/nac-abe/consumer.hpp", "lib/libnac-abe.so"):
        path = prefix / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
    monkeypatch.setenv("NDNSF_NAC_ABE_PREFIX", str(prefix))
    build = namespace["build_extension"]
    # This fixture exercises ordering; production path admission is covered above.
    monkeypatch.setitem(build.__globals__, "reject_non_global_dependency_paths",
                        lambda *args, **kwargs: None)
    extension = build()
    rpaths = [flag for flag in extension.extra_link_args if flag.startswith("-Wl,-rpath,")]
    assert rpaths[0] == "-Wl,-rpath," + str(prefix / "lib")
