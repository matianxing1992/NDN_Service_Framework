"""Offline checks for the base-SIF plus external APP delivery contract."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT_DIR = ROOT / "adapters/slurm-apptainer/scripts"
spec = importlib.util.spec_from_file_location("sif_app_validator", SCRIPT_DIR / "validate-sif-app.py")
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)
builder_spec = importlib.util.spec_from_file_location(
    "sif_app_builder", SCRIPT_DIR / "build-sif-app.py"
)
builder = importlib.util.module_from_spec(builder_spec)
builder_spec.loader.exec_module(builder)


def _digest(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def _fixture(tmp_path):
    app = tmp_path / "app"
    for folder in ("lib", "bin", "python/ndnsf", "python/ndnsf_distributed_inference",
                   "manifest", "replay"):
        (app / folder).mkdir(parents=True, exist_ok=True)
    payloads = {
        "lib/libndnsf-distributed-inference.so": b"di",
        "bin/di-native-provider": b"provider",
        "bin/di-native-fault-provider": b"fault",
        "bin/App_ServiceController": b"controller",
        "python/ndnsf/__init__.py": b"",
        "python/ndnsf_distributed_inference/__init__.py": b"",
    }
    for relative, payload in payloads.items():
        (app / relative).write_bytes(payload)
    base = tmp_path / "base.sif"
    base.write_bytes(b"base")
    candidate_record = {
        "schemaVersion": "ndnsf-local-sif-build-v3",
        "status": "PASS",
        "buildInput": {"baseSif": {"sha256": validator.digest(base)}},
        "sif": {"sha256": _digest(b"candidate")},
    }
    candidate_record["recordDigest"] = "sha256:" + hashlib.sha256(
        json.dumps(candidate_record, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    candidate_record_bytes = (json.dumps(candidate_record, sort_keys=True) + "\n").encode()
    (app / "manifest/candidate-build-record.json").write_bytes(candidate_record_bytes)
    (app / "manifest/app-runtime.lock.json").write_bytes(b"app-lock")
    (app / "manifest/source-seal.json").write_bytes(b"source-seal")
    rows = [{"path": relative, "bytes": len(payload), "sha256": _digest(payload)}
            for relative, payload in payloads.items()]
    rows.extend([
        {"path": "manifest/candidate-build-record.json", "bytes": len(candidate_record_bytes),
         "sha256": _digest(candidate_record_bytes)},
        {"path": "manifest/app-runtime.lock.json", "bytes": len(b"app-lock"),
         "sha256": _digest(b"app-lock")},
        {"path": "manifest/source-seal.json", "bytes": len(b"source-seal"),
         "sha256": _digest(b"source-seal")},
    ])
    record = {
        "schemaVersion": "ndnsf-sif-app-v1",
        "status": "PASS",
        "buildBoundary": "container-runtime-in-sif-extracted",
        "candidateLayout": "opt/ndnsf-app",
        "baseSif": {"path": "/host/base.sif", "sha256": validator.digest(base)},
        "candidateSif": {"path": "/host/candidate.sif", "sha256": _digest(b"candidate")},
        "candidateBuildRecord": {"path": "/host/build-record.json",
                                  "sha256": _digest(candidate_record_bytes),
                                  "recordDigest": candidate_record["recordDigest"]},
        "candidateVerification": "PASS",
        "apptainer": {"version": "1.5.3", "expectedVersion": "1.5.3",
                       "path": "/usr/local/bin/apptainer", "sha256": _digest(b"apptainer")},
        "mounts": [{"source": source, "target": target}
                   for source, target in validator.MOUNTS.items()],
        "files": rows,
        "entrypoints": ["/opt/ndnsf-di/app/bin/" + name
                        for name in ("di-native-provider", "di-native-fault-provider",
                                     "App_ServiceController")],
        "runtimeContract": {"cleanenv": True, "containall": True,
                             "appFallback": "forbidden", "modelMount": "/models:ro",
                             "artifactMount": "/artifacts:ro"},
        "tigerAction": "verify-pair-and-execute-only",
    }
    record["appDigest"] = validator._record_digest(record)
    manifest = tmp_path / "app-manifest.json"
    manifest.write_text(json.dumps(record), encoding="utf-8")
    for path in sorted(app.rglob("*"), reverse=True):
        if not path.is_symlink():
            path.chmod(0o555 if path.is_dir() else 0o444)
    app.chmod(0o555)
    return app, base, manifest


def test_pair_manifest_verifies_all_bytes_and_identity(tmp_path):
    app, base, manifest = _fixture(tmp_path)
    result = validator.validate(manifest, app, base)
    assert result["status"] == "PASS"
    assert result["files"] == 9


@pytest.mark.parametrize("mutation", ["changed", "extra", "escaped"])
def test_pair_manifest_rejects_changed_or_escaped_app(tmp_path, mutation):
    app, base, manifest = _fixture(tmp_path)
    for path in sorted(app.rglob("*"), reverse=True):
        if not path.is_symlink():
            path.chmod(0o777 if path.is_dir() else 0o666)
    app.chmod(0o777)
    if mutation == "changed":
        (app / "bin/di-native-provider").write_bytes(b"changed")
    elif mutation == "extra":
        (app / "bin/unlisted").write_bytes(b"extra")
    else:
        (app / "bin/escape").symlink_to("/etc/passwd")
        record = json.loads(manifest.read_text())
        record["files"].append({"path": "bin/escape", "bytes": 1147,
                                 "sha256": _digest(Path("/etc/passwd").read_bytes())})
        record["appDigest"] = validator._record_digest(record)
        manifest.write_text(json.dumps(record))
    with pytest.raises(validator.SifAppError):
        validator.validate(manifest, app, base)


def test_pair_manifest_rejects_mutable_app(tmp_path):
    app, base, manifest = _fixture(tmp_path)
    (app / "bin/di-native-provider").chmod(0o755)
    with pytest.raises(validator.SifAppError, match="APP_BUNDLE_MUTABLE"):
        validator.validate(manifest, app, base)


def test_pair_manifest_rejects_special_file(tmp_path):
    app, base, manifest = _fixture(tmp_path)
    app_bin = app / "bin"
    app_bin.chmod(0o755)
    os.mkfifo(app_bin / "unexpected-fifo")
    app_bin.chmod(0o555)
    with pytest.raises(validator.SifAppError, match="APP_SPECIAL_FILE"):
        validator.validate(manifest, app, base)


def test_delivery_scripts_have_isolated_runtime_contract():
    build = SCRIPT_DIR / "build-sif-app.sh"
    run = SCRIPT_DIR / "run-sif-app.sh"
    subprocess.run(["bash", "-n", str(build)], check=True)
    subprocess.run(["bash", "-n", str(run)], check=True)
    text = run.read_text(encoding="utf-8")
    assert "--cleanenv --containall" in text
    assert "--bind \"/proc/self/fd/$app_lib_fd:/opt/ndnsf-di/app/lib:ro\"" in text
    assert "PATH=/opt/ndnsf-di/app/bin" in text
    assert "LD_LIBRARY_PATH=/opt/ndnsf-di/app/lib" in text
    assert "/opt/ndnsf-di/current/bin" not in text
    assert "/opt/ndnsf-di/current/lib" not in text
    assert "PYTHONPATH=/opt/ndnsf-di/app/python" in text
    assert "--export=ALL" not in text
    assert "SINGULARITYENV_HOME" in text
    assert "APPTAINER_PAIR_BIND_PATH_INVALID" in text
    assert "pin_dir" in text
    assert "/proc/self/fd/$models_fd:/models:ro" in text
    assert "APPTAINER_PAIR_APPTAINER_NOT_REGULAR" in text
    assert "APPTAINER_PAIR_BIND_PATH_INVALID" in text


def test_build_driver_rejects_overwrite_and_uses_container_extraction():
    text = (SCRIPT_DIR / "build-sif-app.py").read_text(encoding="utf-8")
    assert 'f"{output}:/out:rw"' in text
    assert '"--cleanenv", "--containall"' in text
    assert "APP_OUTPUT_EXISTS" in text
    assert "candidateVerification" in text
    assert "APP_OUTPUT_BUSY" in text
    assert "APP_ELF_DEPENDENCY_FAILED" in text
    assert "/opt/ndnsf-app" in text


def test_staging_token_is_atomic_and_immutable_cleanup_restores_write_bits(tmp_path):
    parent = tmp_path / "release"
    parent.mkdir()
    temp = parent / "app.tmp.crash"
    temp.mkdir()
    temp_record = parent / "manifest.json.tmp.crash"
    temp_record.write_text("partial\n", encoding="utf-8")
    builder.write_staging_token(temp, parent / "app", parent / "manifest.json",
                                temp_record, builder.marker_identity(temp_record))
    assert (temp / builder.STAGING_TOKEN).is_file()
    assert not list(temp.glob(builder.STAGING_TOKEN + ".tmp.*"))

    nested = temp / "nested"
    nested.mkdir()
    (nested / "payload").write_bytes(b"payload")
    (nested / "payload").chmod(0o444)
    nested.chmod(0o555)
    (temp / builder.STAGING_TOKEN).chmod(0o444)
    temp.chmod(0o555)
    parent_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        builder.remove_tree_at(temp.name, parent_fd)
    finally:
        os.close(parent_fd)
    assert not temp.exists()


def test_orphan_atomic_token_temp_is_removed_only_when_directory_is_known(tmp_path):
    parent = tmp_path / "release"
    parent.mkdir()
    output = parent / "app"
    record = parent / "manifest.json"
    orphan = parent / "app.tmp.crash"
    orphan.mkdir()
    token_temp = orphan / (builder.STAGING_TOKEN + ".tmp.123")
    token_temp.write_text("partial", encoding="utf-8")
    token_temp.chmod(0o444)
    orphan.chmod(0o555)
    parent_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        builder.recover_orphan_staging(parent, output, record, parent_fd,
                                       builder.identity(parent))
    finally:
        os.close(parent_fd)
    assert not orphan.exists()


def test_intent_token_recovers_placeholder_record_after_creation_gap(tmp_path):
    parent = tmp_path / "release"
    parent.mkdir()
    output = parent / "app"
    record = parent / "manifest.json"
    orphan = parent / "app.tmp.123"
    orphan.mkdir()
    placeholder = parent / "manifest.json.tmp.123"
    builder.write_staging_token(orphan, output, record, placeholder, None)
    placeholder.touch()
    placeholder.chmod(0o600)
    parent_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        builder.recover_orphan_staging(parent, output, record, parent_fd,
                                       builder.identity(parent))
    finally:
        os.close(parent_fd)
    assert not orphan.exists()
    assert not placeholder.exists()


def test_temp_record_write_rejects_replaced_symlink(tmp_path):
    parent = tmp_path / "release"
    parent.mkdir()
    target = tmp_path / "outside.json"
    target.write_text("keep\n", encoding="utf-8")
    temp_record = parent / "manifest.json.tmp.1"
    temp_record.symlink_to(target)
    parent_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        with pytest.raises(builder.BuildSifAppError, match="APP_TEMP_RECORD_OPEN_FAILED"):
            builder.write_temp_record(temp_record, {"status": "PASS"}, parent_fd,
                                      builder.identity(target))
    finally:
        os.close(parent_fd)
    assert target.read_text(encoding="utf-8") == "keep\n"


def test_temp_record_write_checks_inode_before_truncation(tmp_path):
    parent = tmp_path / "release"
    parent.mkdir()
    temp_record = parent / "manifest.json.tmp.1"
    temp_record.write_text("keep\n", encoding="utf-8")
    unrelated = tmp_path / "unrelated"
    unrelated.write_text("other\n", encoding="utf-8")
    parent_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        with pytest.raises(builder.BuildSifAppError, match="APP_TEMP_RECORD_CHANGED"):
            builder.write_temp_record(temp_record, {"status": "PASS"}, parent_fd,
                                      builder.identity(unrelated))
    finally:
        os.close(parent_fd)
    assert temp_record.read_text(encoding="utf-8") == "keep\n"


def test_marker_fifo_is_rejected_without_blocking(tmp_path):
    parent = tmp_path / "release"
    parent.mkdir()
    output = parent / "app"
    record = parent / "manifest.json"
    marker = builder.publish_marker_path(output, record)
    os.mkfifo(marker)
    parent_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        with pytest.raises(builder.BuildSifAppError, match="APP_PUBLISH_RECOVERY_REQUIRED"):
            builder.recover_publish_marker(marker, output, record, parent_fd,
                                           builder.identity(parent))
    finally:
        os.close(parent_fd)


def test_orphan_marker_temp_is_removed_with_parent_fd_identity(tmp_path):
    parent = tmp_path / "release"
    parent.mkdir()
    marker = parent / ".ndnsf-sif-app-publish-abc.json"
    marker_temp = parent / (marker.name + ".tmp.123")
    marker_temp.write_text("partial", encoding="utf-8")
    parent_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        builder.recover_orphan_marker_temps(parent, marker, parent_fd,
                                            builder.identity(parent))
    finally:
        os.close(parent_fd)
    assert not marker_temp.exists()


def test_publish_rename_rejects_source_inode_change(tmp_path):
    parent = tmp_path / "release"
    parent.mkdir()
    source = parent / "app.tmp.1"
    target = parent / "app"
    source.mkdir()
    replacement = tmp_path / "replacement"
    replacement.mkdir()
    source.rmdir()
    source.symlink_to(replacement, target_is_directory=True)
    parent_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        with pytest.raises(builder.BuildSifAppError, match="APP_PUBLISH_RENAME_FAILED"):
            builder.replace_in_publish_parent(source, target, parent, parent_fd,
                                              builder.identity(parent), (1, 2))
    finally:
        os.close(parent_fd)
    assert source.is_symlink()
