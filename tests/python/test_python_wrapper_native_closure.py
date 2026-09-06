from __future__ import annotations

import os
import runpy
from pathlib import Path

import setuptools
import pytest


ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(autouse=True)
def isolated_build_environment(monkeypatch):
    for name in ("NDNSF_LIBRARY_DIR", "NDNSF_NAC_ABE_PREFIX",
                 "NDNSF_NDN_SVS_SOURCE_TREE", "NDNSF_NDN_SVS_BUILD_TREE"):
        monkeypatch.delenv(name, raising=False)


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
    namespace = runpy.run_path(str(setup_path)) if missing is None else None
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


def test_explicit_ndnsf_library_dir_is_an_exclusive_runtime_closure(
        monkeypatch, tmp_path, setup_path):
    """An explicit candidate must not inherit stale developer build RPATHs."""

    candidate = (tmp_path / "candidate-lib").resolve()
    candidate.mkdir()
    (candidate / "libndn-service-framework.so").touch()
    captured: dict[str, object] = {}

    monkeypatch.setenv("NDNSF_LIBRARY_DIR", str(candidate))
    monkeypatch.setattr(
        setuptools,
        "setup",
        lambda **kwargs: captured.update(kwargs),
    )

    runpy.run_path(str(setup_path), run_name="__main__")
    extension = captured["ext_modules"][0]

    assert extension.library_dirs[0] == str(candidate)
    assert str(ROOT / "build") not in extension.library_dirs
    assert str(ROOT / ".local-boost171" / "lib") not in extension.library_dirs
    assert str(ROOT.parent / "ndn-svs" / "build") not in extension.library_dirs

    runpaths = [
        value[len("-Wl,-rpath,"):]
        for value in extension.extra_link_args
        if value.startswith("-Wl,-rpath,")
    ]
    assert runpaths[0] == str(candidate)
    assert str(ROOT / "build") not in runpaths
    assert str(ROOT / ".local-boost171" / "lib") not in runpaths
    assert str(ROOT.parent / "ndn-svs" / "build") not in runpaths


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

    try:
        runpy.run_path(
            str(setup_path), run_name="__main__")
    except RuntimeError as exc:
        assert "NDNSF_LIBRARY_DIR does not exist" in str(exc)
    else:
        raise AssertionError("missing candidate directory was accepted")
    assert not captured


@pytest.mark.parametrize("candidate", [os.pathsep, os.pathsep * 2])
def test_explicit_ndnsf_library_dir_rejects_empty_candidate_list(
        monkeypatch, setup_path, candidate):
    captured: dict[str, object] = {}
    monkeypatch.setenv("NDNSF_LIBRARY_DIR", candidate)
    monkeypatch.setattr(setuptools, "setup", lambda **kwargs: captured.update(kwargs))

    with pytest.raises(RuntimeError, match="NDNSF_LIBRARY_DIR contains no library directory"):
        runpy.run_path(str(setup_path), run_name="__main__")

    assert not captured
