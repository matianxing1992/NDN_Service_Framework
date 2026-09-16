"""Offline checks for the base-SIF plus external APP delivery contract."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
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
handoff_spec = importlib.util.spec_from_file_location(
    "development_handoff", SCRIPT_DIR / "prepare-development-handoff.py"
)
handoff = importlib.util.module_from_spec(handoff_spec)
handoff_spec.loader.exec_module(handoff)


def _digest(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def _fixture(tmp_path):
    app = tmp_path / "app"
    for folder in ("lib", "bin", "python/ndnsf", "python/ndnsf_distributed_inference",
                   "manifest", "replay"):
        (app / folder).mkdir(parents=True, exist_ok=True)
    payloads = {
        "lib/libndn-service-framework.so.0.1.0": b"framework",
        "lib/libndnsf-distributed-inference.so": b"di",
        "lib/libndn-svs.so.0.1.0": b"svs",
        "lib/libnac-abe.so": b"nac-abe",
        "lib/libndnsd.so.0.1.0": b"ndnsd",
        "lib/libopenabe.so": b"openabe",
        "lib/librelic.so": b"relic",
        "lib/librelic_ec.so": b"relic-ec",
        "bin/di-native-provider": b"provider",
        "bin/di-native-fault-provider": b"fault",
        "bin/App_ServiceController": b"controller",
        "bin/DI_NativeArtifactAuthority": b"authority",
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
        "schemaVersion": "ndnsf-sif-app-v2",
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
                                     "App_ServiceController", "DI_NativeArtifactAuthority")],
        "runtimeContract": {"cleanenv": True, "containall": True,
                             "appFallback": "forbidden", "modelMount": "/models:ro",
                             "artifactMount": "/artifacts:ro",
                             "appNativeLibraries": list(validator.RUNTIME_CONTRACT["appNativeLibraries"]),
                             "baseNativeLibraries": list(validator.RUNTIME_CONTRACT["baseNativeLibraries"]),
                             "baseRuntime": {
                                 "python": "/opt/venv/bin/python",
                                 "ndnBaseLib": "/opt/ndn-base/lib",
                                 "onnxRuntimeLib": "/opt/onnxruntime/lib",
                             }},
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
    assert result["files"] == 17


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


def test_handoff_rejects_symlinked_base_before_resolution(tmp_path):
    target = tmp_path / "missing-base.sif"
    link = tmp_path / "base.sif"
    link.symlink_to(target)
    with pytest.raises(ValueError, match="HANDOFF_BASE_SIF_PATH_SYMLINK"):
        handoff.reject_symlink_components(link, "HANDOFF_BASE_SIF_PATH")


def test_real_native_build_needed_names_are_in_app_allowlist():
    # ROOT is Experiments/TigerCluster; its second parent is the repository
    # root where the reusable native build trees live.
    repo = ROOT.parents[1]
    candidates = [
        repo / "build-spec185-b0c-normal/examples/di-native-provider",
        repo / "build-spec185-b0c-normal/examples/App_ServiceController",
        repo / "build-spec185-b0c-normal/libndnsf-distributed-inference.so",
    ]
    available = [path for path in candidates if path.is_file()]
    if not available:
        pytest.skip("no local native build candidate under " + str(repo))
    app_libraries = set(validator.RUNTIME_CONTRACT["appNativeLibraries"])
    # Derive the base side from the published runtime contract.  An unknown
    # NEEDED name must fail instead of disappearing when filtered to APP.
    base_libraries = set(validator.RUNTIME_CONTRACT["baseNativeLibraries"])
    names = set()
    for path in available:
        output = subprocess.check_output(["readelf", "-d", str(path)], text=True)
        names.update(re.findall(r"Shared library: \[([^]]+)\]", output))
    unknown = names - app_libraries - base_libraries
    assert not unknown, f"unclassified DT_NEEDED names: {sorted(unknown)}"

    # A container run can provide the exact APP/lib directory for a source
    # candidate.  In that mode check the actual ldd resolution for every APP
    # dependency.  Host-only builds commonly resolve through /usr/local/lib;
    # leave that boundary explicitly unqualified here and let the container
    # packager's mandatory ldd gate perform the runtime rejection.
    custom = names & app_libraries
    app_lib_root = os.environ.get("NDNSF_TEST_APP_LIB_ROOT")
    if not custom or not app_lib_root:
        pytest.skip("APP/lib origin requires a container runtime root")
    app_lib_root = str(Path(app_lib_root).resolve()) + "/"
    ldd_env = os.environ.copy()
    ldd_env["LD_LIBRARY_PATH"] = app_lib_root + ldd_env.get("LD_LIBRARY_PATH", "")
    for path in available:
        output = subprocess.check_output(["ldd", str(path)], text=True,
                                         stderr=subprocess.STDOUT, env=ldd_env)
        resolutions = {}
        for line in output.splitlines():
            fields = line.strip().split()
            if len(fields) >= 3 and fields[1] == "=>":
                resolutions[fields[0]] = fields[2]
        for name in sorted(custom):
            resolved = resolutions.get(name)
            assert resolved, f"ldd did not resolve APP dependency {name} for {path}"
            assert resolved.startswith(app_lib_root), (
                f"APP dependency {name} escaped APP/lib: {resolved}")


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
    assert "APPTAINER_PAIR_BASE_RUNTIME_CONTRACT_FAILED" in text
    assert "--nfd-socket PATH" in text
    assert "APPTAINER_PAIR_NFD_SOCKET_MISSING" in text
    assert "APPTAINER_PAIR_NFD_SOCKET_DIR_OWNER_MISMATCH" in text
    assert "APPTAINER_PAIR_NFD_SOCKET_DIR_NOT_PRIVATE" in text
    assert "APPTAINER_PAIR_NFD_SOCKET_NOT_JOB_SCOPED" in text
    assert "APPTAINER_PAIR_NFD_SOCKET_DIR_NOT_DEDICATED" in text
    assert "NDN_CLIENT_TRANSPORT=unix:///tmp/ndnsf-di-nfd/$nfd_socket_name" in text
    assert "NDN_CLIENT_PIB=pib-sqlite3:$home_target/.ndn" in text
    assert "NDN_CLIENT_TPM=tpm-file:$home_target/.ndn" in text
    assert "APPTAINER_PAIR_IDENTITY_TPM_MISSING" in text
    assert "-type f -name '*.privkey'" in text
    assert "UPDATE tpmInfo SET tpm_locator" in text
    assert "APPTAINER_PAIR_TPM_LOCATOR_REWRITE_FAILED" in text
    assert '--bind "/proc/self/fd/$nfd_socket_dir_fd:/tmp/ndnsf-di-nfd:ro"' in text
    assert 'test -S "$socket_path"' in text
    assert 'expected_identity="$2"' in text
    assert 'exec "$@"' in text
    assert 'cp -a --no-preserve=ownership' in text
    assert 'app_status=$?' in text
    assert 'exec env -u' not in text
    assert "--bind \"/proc/self/fd/$scratch_fd:/scratch:rw\"" in text
    assert "scratch_parent_fd" in text
    assert "scratch_created" in text
    assert "trap cleanup_scratch EXIT" in text


def test_build_driver_rejects_overwrite_and_uses_container_extraction():
    text = (SCRIPT_DIR / "build-sif-app.py").read_text(encoding="utf-8")
    assert 'f"{output}:/out:rw"' in text
    assert '"--cleanenv", "--containall"' in text
    assert "APP_OUTPUT_EXISTS" in text
    assert "candidateVerification" in text
    assert "APP_OUTPUT_BUSY" in text
    assert "APP_ELF_DEPENDENCY_FAILED" in text
    assert "APP_ELF_LIBRARY_ORIGIN_MISMATCH" in text
    assert "APP_ELF_FORBIDDEN_RESOLVED_PATH" in text
    assert "APP_MATERIALIZED_ELF_LIBRARY_ORIGIN_MISMATCH" in text
    assert "/usr/local/lib/" in text
    assert "APP_BASE_RUNTIME_CONTRACT_FAILED" in text
    assert "pinned_image" in text
    assert "APP_IMAGE_ARGUMENT_MISSING" in text
    assert "/opt/ndnsf-app" in text


def test_build_driver_rejects_forbidden_elf_runtime_path(tmp_path, monkeypatch):
    elf = tmp_path / "bin" / "provider"
    elf.parent.mkdir()
    elf.write_bytes(b"\x7fELF\x02\x01\x01")

    class Result:
        returncode = 0
        stdout = " 0x1 (RUNPATH) Library runpath: [/opt/ndnsf-di/current/lib]\\n"

    monkeypatch.setattr(builder.subprocess, "run", lambda *args, **kwargs: Result())
    with pytest.raises(builder.BuildSifAppError, match="APP_ELF_FORBIDDEN_RUNTIME_PATH"):
        builder.verify_elf_runtime_paths(tmp_path)


def test_publish_rejects_changed_base_before_publication_side_effects(tmp_path, monkeypatch):
    """A stale build record must not trigger locks or staging recovery."""
    base = tmp_path / "base.sif"
    candidate = tmp_path / "candidate.sif"
    base.write_bytes(b"current-base")
    candidate.write_bytes(b"candidate")
    apptainer = tmp_path / "apptainer"
    apptainer.write_bytes(b"apptainer")
    apptainer.chmod(0o755)
    record = tmp_path / "candidate-record.json"
    monkeypatch.setattr(builder, "load_build_record", lambda *args: {
        "buildInput": {"baseSif": {"sha256": _digest(b"old-base")}},
    })
    monkeypatch.setattr(builder, "apptainer_version", lambda path: "1.5.3")
    parent = tmp_path / "release"
    parent.mkdir()
    output = parent / "app"
    app_record = parent / "app-manifest.json"
    stale = parent / "app.tmp.stale"
    stale.mkdir()
    (stale / "marker").write_text("must remain", encoding="utf-8")

    def unexpected_lock(*args, **kwargs):
        raise AssertionError("publication lock acquired before input validation")

    monkeypatch.setattr(builder.fcntl, "flock", unexpected_lock)
    args = type("Args", (), {
        "base_sif": str(base), "candidate_sif": str(candidate),
        "candidate_record": str(record), "output": str(output),
        "record": str(app_record), "apptainer": str(apptainer),
        "expected_apptainer": "1.5.3",
    })()
    with pytest.raises(builder.BuildSifAppError, match="APP_BASE_DIGEST_MISMATCH"):
        builder.publish(args)
    assert not output.exists()
    assert not app_record.exists()
    assert (stale / "marker").read_text(encoding="utf-8") == "must remain"


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
