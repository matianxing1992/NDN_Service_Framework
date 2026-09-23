from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = (ROOT / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts"
          / "validate-local-sif-build-record.py")
SPEC = importlib.util.spec_from_file_location("spec170_sif_build_record", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def _digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _write_record(root: Path, sif: Path, *, schema: str = "ndnsf-local-sif-build-v3",
                  native: bool = True) -> Path:
    body = {
        "schemaVersion": schema,
        "status": "PASS",
        "buildInput": {"method": "local-apptainer-definition"},
        "sourceValidation": {"status": "PASS"},
        "hostRole": "apptainer-driver-only",
        "containerNativeBuild": {
            "status": "PASS",
            "schemaVersion": "spec170-sif-build-boundary-v2",
            "containerNativeBuild": native,
            "staleBaseArtifactsReplaced": True,
            "hostBinaryInputs": [],
        },
        "sif": {"path": str(sif), "sha256": _digest(sif),
                "bytes": sif.stat().st_size},
    }
    body["recordDigest"] = MODULE._record_digest(body)
    path = root / "build-record.json"
    path.write_text(json.dumps(body, sort_keys=True) + "\n", encoding="utf-8")
    return path


def test_v3_container_native_record_is_accepted(tmp_path: Path) -> None:
    sif = tmp_path / "runtime.sif"
    sif.write_bytes(b"candidate")
    record = _write_record(tmp_path, sif)
    assert MODULE.validate(record, sif, _digest(sif))["status"] == "PASS"


@pytest.mark.parametrize('clean,expected', [(True, True), (False, False), (None, False)])
def test_installed_runtime_requires_dependency_only_base(tmp_path: Path, clean, expected) -> None:
    sif = tmp_path / "runtime.sif"
    sif.write_bytes(b"fixture, not a real SIF")
    record = _write_record(tmp_path, sif)
    body = json.loads(record.read_text())
    body['containerNativeBuild'].update(runtimeLayout='installed-v1',
                                      staleBaseArtifactsReplaced=False,
                                      cleanDependencyBaseRequired=clean)
    body['recordDigest'] = MODULE._record_digest(body)
    record.write_text(json.dumps(body))
    if expected:
        assert MODULE.validate(record, sif, _digest(sif))['status'] == 'PASS'
    else:
        with pytest.raises(MODULE.BuildRecordError, match='BOUNDARY_INVALID'):
            MODULE.validate(record, sif, _digest(sif))


def test_metadata_only_record_check_skips_sif_read_but_keeps_size_contract(
    tmp_path: Path,
) -> None:
    sif = tmp_path / "runtime.sif"
    sif.write_bytes(b"candidate")
    record = _write_record(tmp_path, sif)
    result = MODULE.validate(record, sif, _digest(sif), verify_sif_hash=False)
    assert result["status"] == "PASS"
    assert result["sifSha256"] == _digest(sif)


def test_metadata_only_record_check_does_not_require_source_sif_to_be_statable(
    tmp_path: Path,
) -> None:
    sif = tmp_path / "runtime.sif"
    sif.write_bytes(b"candidate")
    digest = _digest(sif)
    record = _write_record(tmp_path, sif)
    sif.unlink()
    assert MODULE.validate(record, sif, digest, verify_sif_hash=False)["status"] == "PASS"


def test_r13_v2_record_is_rejected_before_execution(tmp_path: Path) -> None:
    sif = tmp_path / "runtime.sif"
    sif.write_bytes(b"old-r13")
    record = _write_record(tmp_path, sif, schema="ndnsf-local-sif-build-v2")
    with pytest.raises(MODULE.BuildRecordError, match="SCHEMA_MISMATCH"):
        MODULE.validate(record, sif, _digest(sif))


def test_host_binary_provenance_is_rejected_even_with_matching_hash(tmp_path: Path) -> None:
    sif = tmp_path / "runtime.sif"
    sif.write_bytes(b"candidate")
    record = _write_record(tmp_path, sif, native=False)
    with pytest.raises(MODULE.BuildRecordError, match="BOUNDARY_INVALID"):
        MODULE.validate(record, sif, _digest(sif))


def test_run_container_rejects_legacy_record_before_sif_staging(tmp_path: Path) -> None:
    sif = tmp_path / "runtime.sif"
    sif.write_bytes(b"old-r13")
    project = tmp_path / "project" / "ndnsf-di"
    for relative in ("releases", "models", "artifacts", "identities/provider", "evidence"):
        (project / relative).mkdir(parents=True, exist_ok=True)
    validator_copy = project / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts"
    validator_copy.mkdir(parents=True, exist_ok=True)
    shutil.copy2(SCRIPT, validator_copy / SCRIPT.name)
    record = _write_record(project / "releases", sif, schema="ndnsf-local-sif-build-v2")
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    invocation = tmp_path / "apptainer-invocation.log"
    fake = fake_bin / "apptainer"
    fake.write_text(f"#!/bin/sh\nprintf '%s\\n' \"$*\" >> {invocation}\n", encoding="utf-8")
    fake.chmod(0o755)
    scratch = Path("/tmp/ndnsf-di-99173")
    scratch.mkdir(parents=True, exist_ok=True)
    try:
        result = subprocess.run(
            [
                str(ROOT / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-container.sh"),
                "--sif", str(sif), "--sif-sha256", _digest(sif),
                "--build-record", str(record), "--project", str(project),
                "--scratch", str(scratch), "--identity", str(project / "identities/provider"),
                "--", "/bin/true",
            ],
            cwd=ROOT,
            env={**os.environ, "PATH": f"{fake_bin}:{os.environ['PATH']}",
                 "NDNSF_SPEC110_ALLOW_TEST_ROOT": "1", "SLURM_JOB_ID": "99173",
                 "NDNSF_SIF_CACHE_DIR": str(tmp_path / "cache")},
            check=False, capture_output=True, text=True,
        )
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    assert result.returncode == 4
    assert "SCHEMA_MISMATCH" in result.stderr
    assert not invocation.exists()


@pytest.mark.parametrize("scratch_suffix", ["", "-runA"])
def test_run_container_accepts_template_job_scoped_scratch_names(
    tmp_path: Path, scratch_suffix: str,
) -> None:
    sif = tmp_path / "runtime.sif"
    sif.write_bytes(b"candidate")
    project = tmp_path / "project" / "ndnsf-di"
    for relative in ("releases", "models", "artifacts", "identities/provider", "evidence"):
        (project / relative).mkdir(parents=True, exist_ok=True)
    validator_copy = project / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts"
    validator_copy.mkdir(parents=True, exist_ok=True)
    shutil.copy2(SCRIPT, validator_copy / SCRIPT.name)
    record = _write_record(project / "releases", sif)
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    invocation = tmp_path / "apptainer-invocation.log"
    fake = fake_bin / "apptainer"
    fake.write_text(
        f"#!/bin/sh\nprintf '%s\\n' \"$*\" >> {invocation}\n",
        encoding="utf-8",
    )
    fake.chmod(0o755)
    job = "99173"
    scratch = Path("/tmp") / f"ndnsf-di-{job}{scratch_suffix}"
    scratch.mkdir(parents=True, exist_ok=True)
    try:
        result = subprocess.run(
            [
                str(ROOT / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-container.sh"),
                "--sif", str(sif), "--sif-sha256", _digest(sif),
                "--build-record", str(record), "--project", str(project),
                "--scratch", str(scratch), "--identity", str(project / "identities/provider"),
                "--", "/bin/true",
            ],
            cwd=ROOT,
            env={**os.environ, "PATH": f"{fake_bin}:{os.environ['PATH']}",
                 "NDNSF_SPEC110_ALLOW_TEST_ROOT": "1", "SLURM_JOB_ID": job,
                 "NDNSF_SIF_CACHE_DIR": str(tmp_path / "cache")},
            check=False, capture_output=True, text=True,
        )
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    assert result.returncode == 0, result.stderr
    assert invocation.exists()
    assert (tmp_path / "cache" / f"u{os.getuid()}" / _digest(sif).split(":", 1)[1]).is_dir()


def test_run_container_rejects_scratch_from_another_job(tmp_path: Path) -> None:
    sif = tmp_path / "runtime.sif"
    sif.write_bytes(b"candidate")
    project = tmp_path / "project" / "ndnsf-di"
    for relative in ("releases", "models", "artifacts", "identities/provider", "evidence"):
        (project / relative).mkdir(parents=True, exist_ok=True)
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    invocation = tmp_path / "apptainer-invocation.log"
    fake = fake_bin / "apptainer"
    fake.write_text(f"#!/bin/sh\nprintf '%s\\n' \"$*\" >> {invocation}\n", encoding="utf-8")
    fake.chmod(0o755)
    scratch = Path("/tmp") / "ndnsf-di-99172-runA"
    scratch.mkdir(parents=True, exist_ok=True)
    try:
        result = subprocess.run(
            [
                str(ROOT / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-container.sh"),
                "--sif", str(sif), "--sif-sha256", _digest(sif),
                "--project", str(project), "--scratch", str(scratch),
                "--identity", str(project / "identities/provider"), "--", "/bin/true",
            ],
            cwd=ROOT,
            env={**os.environ, "PATH": f"{fake_bin}:{os.environ['PATH']}",
                 "NDNSF_SPEC110_ALLOW_TEST_ROOT": "1", "SLURM_JOB_ID": "99173",
                 "NDNSF_SIF_CACHE_DIR": str(tmp_path / "cache")},
            check=False, capture_output=True, text=True,
        )
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    assert result.returncode == 1
    assert "APPTAINER_SCRATCH_INVALID" in result.stderr
    assert not invocation.exists()


def test_run_container_rejects_job_named_scratch_symlink(tmp_path: Path) -> None:
    sif = tmp_path / "runtime.sif"
    sif.write_bytes(b"candidate")
    project = tmp_path / "project" / "ndnsf-di"
    for relative in ("releases", "models", "artifacts", "identities/provider", "evidence"):
        (project / relative).mkdir(parents=True, exist_ok=True)
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    invocation = tmp_path / "apptainer-invocation.log"
    fake = fake_bin / "apptainer"
    fake.write_text(f"#!/bin/sh\nprintf '%s\\n' \"$*\" >> {invocation}\n", encoding="utf-8")
    fake.chmod(0o755)
    target = Path(tempfile.mkdtemp(prefix="ndnsf-di-scratch-target-", dir="/tmp"))
    link = Path("/tmp/ndnsf-di-99174")
    link.unlink(missing_ok=True)
    link.symlink_to(target, target_is_directory=True)
    try:
        result = subprocess.run(
            [
                str(ROOT / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-container.sh"),
                "--sif", str(sif), "--sif-sha256", _digest(sif),
                "--project", str(project), "--scratch", str(link),
                "--identity", str(project / "identities/provider"), "--", "/bin/true",
            ],
            cwd=ROOT,
            env={**os.environ, "PATH": f"{fake_bin}:{os.environ['PATH']}",
                 "NDNSF_SPEC110_ALLOW_TEST_ROOT": "1", "SLURM_JOB_ID": "99174",
                 "NDNSF_SIF_CACHE_DIR": str(tmp_path / "cache")},
            check=False, capture_output=True, text=True,
        )
    finally:
        link.unlink(missing_ok=True)
        shutil.rmtree(target, ignore_errors=True)
    assert result.returncode == 1
    assert "APPTAINER_SCRATCH_SYMLINK_FORBIDDEN" in result.stderr
    assert not invocation.exists()
