from __future__ import annotations

import os
import hashlib
import json
import runpy
from pathlib import Path

import setuptools
import pytest


ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(autouse=True)
def isolated_build_environment(monkeypatch):
    for name in ("NDNSF_LIBRARY_DIR", "NDNSF_NAC_ABE_PREFIX",
                 "NDNSF_NDN_SVS_SOURCE_TREE", "NDNSF_NDN_SVS_BUILD_TREE",
                 "NDNSF_RUNTIME_RPATH", "NDNSF_CONTAINER_BUILD"):
        monkeypatch.delenv(name, raising=False)
    native_libs = {
        name: hashlib.sha256((Path("/usr/local/lib") / name).read_bytes()).hexdigest()
        for name in ("libndn-service-framework.so", "libndnsf-distributed-inference.so")
        if (Path("/usr/local/lib") / name).is_file()
    }
    if len(native_libs) == 2:
        monkeypatch.setenv("NDNSF_GLOBAL_NATIVE_DIGESTS",
                          json.dumps(native_libs, sort_keys=True))
    else:
        monkeypatch.delenv("NDNSF_GLOBAL_NATIVE_DIGESTS", raising=False)


@pytest.fixture(params=["pythonWrapper/setup.py", "NDNSF-DistributedRepo/pythonWrapper/setup.py"])
def setup_path(request):
    return ROOT / request.param


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
    monkeypatch.setitem(build.__globals__, "pkg_config", lambda *args: (
        ["/stale/include"], ["/stale/lib"], ["nac-abe", "ndn-cxx"], []))
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
        pkg_config = lambda *args: ([str(local / "include")], [str(local)],
                                     ["ndn-cxx"], [])
    elif bad_kind == "rpath":
        pkg_config = lambda *args: (["/usr/local/include"], ["/usr/local/lib"],
                                     ["ndn-cxx"], [f"-Wl,-rpath,{local}"])
    else:
        pkg_config = lambda *args: (["/tmp/ndnsf/include"], ["/usr/local/lib"],
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
    namespace["build_extension"].__globals__["pkg_config"] = lambda *args: (
        ["/usr/local/include"], ["/usr/local/lib"], ["ndn-cxx"],
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
    root = tmp_path / "global"
    root.mkdir()
    (root / "libndn-service-framework.so").symlink_to(outside)
    (root / "libndnsf-distributed-inference.so").write_bytes(b"di")
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    monkeypatch.setenv("NDNSF_LIBRARY_DIR", "/usr/local/lib")
    namespace = runpy.run_path(str(setup_path), run_name="__main__")
    namespace["HOST_NATIVE_LIBRARY_DIR"] = root
    namespace["dependency_roots"] = lambda: (root,)
    monkeypatch.setenv("NDNSF_LIBRARY_DIR", str(root))
    with pytest.raises(RuntimeError, match="resolved to undeclared dependency root"):
        namespace["native_library_dirs"]()


def test_host_native_library_dir_cannot_select_another_system_root(
        monkeypatch, setup_path):
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: None)
    monkeypatch.setenv("NDNSF_LIBRARY_DIR", "/usr/lib")
    with pytest.raises(RuntimeError, match="canonical host install root"):
        runpy.run_path(str(setup_path), run_name="__main__")
