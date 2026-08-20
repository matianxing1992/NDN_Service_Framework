from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
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
