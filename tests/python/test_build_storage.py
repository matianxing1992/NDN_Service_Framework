import hashlib
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
from types import SimpleNamespace

import pytest


SCRIPT = Path(__file__).resolve().parents[2] / "scripts/build-storage.py"
INSTALLER = SCRIPT.parent.parent / "install_ndnsf_stack.sh"


def call(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)],
                          capture_output=True, text=True, timeout=30)


def test_reuses_waf_configuration_only_for_matching_identity(tmp_path):
    build = tmp_path / "build"
    cache = build / "c4che"
    cache.mkdir(parents=True)
    source = tmp_path / "wscript"
    source.write_text("configuration")
    signature = hashlib.md5(repr((0, source.read_bytes())).encode()).digest()
    lock = {"out_dir": str(build), "top_dir": str(tmp_path), "files": [str(source)],
            "hash": signature, "argv": ["./waf", "configure", "--prefix", "/usr/local",
                                        "--with-tests", "--components=core"]}
    (build / ".lock-waf_linux_build").write_text(
        "".join(f"{key} = {value!r}\n" for key, value in lock.items()))
    (cache / "_cache.py").write_text("CXX = ['/usr/bin/g++']\nsanitize_di = False\n")
    (build / "config.hpp").touch()
    args = ["configured", "--build-dir", build, "--cxx", "/usr/bin/g++", "--"]
    assert call(*args, "--prefix=/usr/local", "--components", "core").returncode == 0
    assert call(*args, "--prefix=/tmp/other").returncode == 1
    source.write_text("configuration changed")
    assert call(*args, "--prefix=/usr/local").returncode == 1


def test_disk_denial_groups_build_scratch_stage_and_prefix_before_launch(tmp_path):
    build = tmp_path / "build"
    scratch = tmp_path / "scratch"
    build.mkdir()
    scratch.mkdir()
    sentinel = build / "compiler-started"
    result = call("run", "--build-dir", build, "--temporary-dir", scratch,
                  "--prefix", tmp_path / "prefix", "--build-bytes", 10,
                  "--install-bytes", 20, "--jobs", 1, "--reserve", 2**63,
                  "--", sys.executable, "-c", f"open({str(sentinel)!r}, 'w').close()")
    assert result.returncode == 75
    assert result.stdout.count("NDNSF_DISK device=") == 1
    assert f"estimated={128 * 1024**2 + 70}" in result.stdout
    assert "compiler scratch" in result.stderr
    assert "prefix promotion" in result.stderr
    assert not sentinel.exists()
    assert not (build / "install-stage").exists()
    assert not (tmp_path / "prefix").exists()


def test_prior_measured_peak_takes_priority_over_the_initial_estimate(tmp_path, monkeypatch):
    run = runpy.run_path(str(SCRIPT))["run"]
    gib = 1024**3
    samples = iter([10 * gib, 10 * gib])
    monkeypatch.setitem(run.__globals__, "available", lambda _: next(samples, 6 * gib))
    args = SimpleNamespace(build_dir=str(tmp_path), temporary_dir=str(tmp_path),
                           targets="focused", profile="test", jobs=1, prefix=None,
                           reserve=0, build_bytes=0, install_bytes=0,
                           command=[sys.executable, "-c", "pass"])
    assert run(args) == 0
    monkeypatch.setitem(run.__globals__, "available", lambda _: 3 * gib)
    args.command = []
    with pytest.raises(ValueError, match="estimated allocation"):
        run(args)


def test_stage_restarts_in_place_and_preserves_unregistered_directory(tmp_path):
    build = tmp_path / "build"
    build.mkdir()
    stage = build / "install-stage"
    stage.mkdir(mode=0o700)
    evidence = stage / "existing"
    evidence.write_text("unregistered")
    assert call("stage", "--build-dir", build, "prepare").returncode == 75
    assert evidence.read_text() == "unregistered"
    evidence.unlink()
    stage.rmdir()
    assert call("stage", "--build-dir", build, "prepare").returncode == 0
    (stage / "interrupted-output").touch()
    worker = subprocess.Popen(["sleep", "30"], cwd=stage)
    try:
        assert call("stage", "--build-dir", build, "prepare").returncode == 75
        assert (stage / "interrupted-output").exists()
    finally:
        worker.terminate()
        worker.wait(timeout=5)
    assert call("stage", "--build-dir", build, "prepare").returncode == 0
    assert len(list(build.glob("install-stage*"))) == 1
    assert not (stage / "interrupted-output").exists()


def test_verified_entry_runs_outside_checkout_before_stage_retires(tmp_path):
    build = tmp_path / "build"
    prefix = tmp_path / "prefix"
    build.mkdir()
    assert call("stage", "--build-dir", build, "prepare").returncode == 0
    deposited = build / "install-stage" / prefix.relative_to("/")
    staged = deposited / "bin" / "DI_NativeRequester"
    staged.parent.mkdir(parents=True)
    staged.write_text('#!/bin/sh\n[ "$1" = --help ] && [ "$PWD" = "$HOME" ] && '
                      '[ "$LD_LIBRARY_PATH" = "${0%/bin/*}/lib" ]\n')
    staged.chmod(0o755)
    identity = staged.with_name("ndnsf-identity-check")
    identity.write_text('#!/bin/sh\n[ -d "$1" ] && [ "$1" != "$HOME" ] || exit 2\n'
                        'echo "existing credential PIB and TPM" >&2\nexit 1\n')
    identity.chmod(0o755)
    shutil.copytree(deposited, prefix)
    assert call("promotion-budget", "--build-dir", build, "--prefix", prefix,
                "--reserve", 0).returncode == 0
    target = prefix / "bin" / staged.name
    target.write_text("different")
    assert call("verify-install", "--build-dir", build, "--prefix", prefix).returncode == 75
    assert staged.exists()
    shutil.copy2(staged, target)
    assert call("verify-install", "--build-dir", build, "--prefix", prefix).returncode == 0
    assert call("stage", "--build-dir", build, "retire").returncode == 0
    assert not deposited.exists()
    assert os.access(target, os.X_OK)


def test_interrupted_promotion_reuses_recorded_copy_and_keeps_original_backup(tmp_path):
    shell = INSTALLER.read_text()
    ledger = shell.split("<<'NDNSF_INSTALL_MANIFEST_PY'\n", 1)[1].split(
        "\nNDNSF_INSTALL_MANIFEST_PY", 1)[0]
    prefix, stage = tmp_path / "prefix", tmp_path / "stage"
    prefix.mkdir()
    stage.mkdir()
    target, staged = prefix / "payload", stage / "payload"
    target.write_text("original")
    staged.write_text("first install")
    manifest = prefix / "share" / "stack-install-manifest.json"
    base = [sys.executable, "-c"]
    args = ["install-tree", "--manifest", str(manifest), "--stage", str(stage),
            "--prefix", str(prefix), "--component", "ndnsf-native"]
    env = dict(os.environ, NDNSF_RUNTIME_PREFIX=str(prefix))

    inject = """
import os
replace = os.replace
def kill_once_copied(source, destination):
    if os.path.basename(source).startswith(".ndnsf-install-"):
        os._exit(47)
    replace(source, destination)
os.replace = kill_once_copied
"""
    backup_fault = inject.replace('startswith(".ndnsf-install-")', 'endswith(".pending")')
    faulted = ledger.replace("SCHEMA = 1", backup_fault + "\nSCHEMA = 1", 1)
    assert subprocess.run(base + [faulted] + args, env=env, timeout=10).returncode == 47
    assert target.read_text() == "original"
    assert len(list(manifest.parent.rglob("*.pending"))) == 1
    subprocess.run(base + [ledger] + args, env=env, check=True, timeout=10)
    assert not list(manifest.parent.rglob("*.pending"))

    staged.write_text("replacement")
    faulted = ledger.replace("SCHEMA = 1", inject + "\nSCHEMA = 1", 1)
    assert subprocess.run(base + [faulted] + args, env=env, timeout=10).returncode == 47
    assert target.read_text() == "first install"
    assert len(list(prefix.glob(".ndnsf-install-*"))) == 1
    subprocess.run(base + [ledger] + args, env=env, check=True, timeout=10)
    assert target.read_text() == "replacement"
    assert not list(prefix.glob(".ndnsf-install-*"))
    subprocess.run(base + [ledger, "uninstall", "--manifest", str(manifest)],
                   env=env, check=True, timeout=10)
    assert target.read_text() == "original"
