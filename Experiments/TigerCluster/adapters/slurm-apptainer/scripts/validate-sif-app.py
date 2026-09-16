#!/usr/bin/env python3
"""Validate an NDNSF base-SIF plus external application bundle.

The validator is deliberately independent of Apptainer and of the host
checkout.  It verifies the immutable bundle bytes and the composite identity
record produced by ``build-sif-app.sh`` before a runner is allowed to mount it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import stat
from pathlib import Path
from typing import Any


SCHEMA = "ndnsf-sif-app-v2"
APP_LAYOUT = "opt/ndnsf-app"
DIGEST = re.compile(r"^sha256:[0-9a-f]{64}$")
VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.]+)?$")
MOUNTS = {
    "lib": "/opt/ndnsf-di/app/lib",
    "bin": "/opt/ndnsf-di/app/bin",
    "python": "/opt/ndnsf-di/app/python",
    "manifest": "/opt/ndnsf-di/app/manifest",
    "replay": "/opt/ndnsf-di/app/replay",
}
REQUIRED_FILES = {
    "lib/libndn-service-framework.so.0.1.0",
    "lib/libndnsf-distributed-inference.so",
    "lib/libndn-svs.so.0.1.0",
    "lib/libnac-abe.so",
    "lib/libndnsd.so.0.1.0",
    "lib/libopenabe.so",
    "lib/librelic.so",
    "lib/librelic_ec.so",
    "bin/di-native-provider",
    "bin/di-native-fault-provider",
    "bin/App_ServiceController",
    "bin/DI_NativeArtifactAuthority",
    "python/ndnsf_distributed_inference/__init__.py",
    "manifest/app-runtime.lock.json",
    "manifest/source-seal.json",
    "manifest/candidate-build-record.json",
}
BASE_NATIVE_LIBRARIES = [
    "libonnxruntime.so.1",
    "libndn-cxx.so.0.9.0",
    "libboost_log.so.1.71.0",
    "libboost_stacktrace_backtrace.so.1.71.0",
    "libboost_chrono.so.1.71.0",
    "libboost_thread.so.1.71.0",
    "libboost_system.so.1.71.0",
    "libboost_filesystem.so.1.71.0",
    "libboost_program_options.so.1.71.0",
    "libboost_regex.so.1.71.0",
    "libboost_serialization.so.1.71.0",
    "libboost_iostreams.so.1.71.0",
    "libboost_date_time.so.1.71.0",
    "libboost_atomic.so.1.71.0",
    "libboost_log_setup.so.1.71.0",
    "libprotobuf.so.17",
    "libsqlite3.so.0",
    "libgmp.so.10",
    "libpcap.so.0.8",
    "libssl.so.1.1",
    "libcrypto.so.1.1",
    "libpthread.so.0",
    "libdl.so.2",
    "librt.so.1",
    "libstdc++.so.6",
    "libm.so.6",
    "libgcc_s.so.1",
    "libc.so.6",
    "ld-linux-x86-64.so.2",
]
RUNTIME_CONTRACT = {
    "cleanenv": True,
    "containall": True,
    "appFallback": "forbidden",
    "modelMount": "/models:ro",
    "artifactMount": "/artifacts:ro",
    "appNativeLibraries": [
        "libndn-service-framework.so.0.1.0",
        "libndnsf-distributed-inference.so",
        "libndn-svs.so.0.1.0",
        "libnac-abe.so",
        "libndnsd.so.0.1.0",
        "libopenabe.so",
        "librelic.so",
        "librelic_ec.so",
    ],
    "baseNativeLibraries": BASE_NATIVE_LIBRARIES,
    "baseRuntime": {
        "python": "/opt/venv/bin/python",
        "ndnBaseLib": "/opt/ndn-base/lib",
        "onnxRuntimeLib": "/opt/onnxruntime/lib",
    },
}


class SifAppError(ValueError):
    """The SIF/app pair is incomplete or has changed after publication."""


def _fail(code: str, detail: object = "") -> None:
    suffix = f":{detail}" if detail != "" else ""
    raise SifAppError(code + suffix)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            value.update(chunk)
    return "sha256:" + value.hexdigest()


def _safe_relative(value: object, code: str = "APP_FILE_PATH") -> Path:
    if not isinstance(value, str):
        _fail(code)
    path = Path(value)
    if path.is_absolute() or not value or ".." in path.parts:
        _fail(code, value)
    return path


def _canonical(record: dict[str, Any]) -> dict[str, Any]:
    """Remove operational paths before checking the portable app identity."""
    body = json.loads(json.dumps(record, sort_keys=True))
    body.pop("appDigest", None)
    for field in ("baseSif", "candidateSif", "candidateBuildRecord"):
        row = body.get(field)
        if isinstance(row, dict):
            row.pop("path", None)
    apptainer = body.get("apptainer")
    if isinstance(apptainer, dict):
        apptainer.pop("path", None)
    return body


def _record_digest(record: dict[str, Any]) -> str:
    return "sha256:" + hashlib.sha256(
        json.dumps(_canonical(record), sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


def _iter_entries(root: Path) -> set[str]:
    entries: set[str] = set()
    for path in root.rglob("*"):
        if path.is_symlink():
            entries.add(path.relative_to(root).as_posix())
        elif path.is_file():
            entries.add(path.relative_to(root).as_posix())
    return entries


def _verify_symlink(root: Path, path: Path) -> None:
    if not path.is_symlink():
        return
    try:
        path.resolve(strict=True).relative_to(root)
    except (OSError, ValueError):
        _fail("APP_SYMLINK_TARGET_OUTSIDE_BUNDLE", path.relative_to(root))


def _verify_regular_file(path: Path, key: object) -> None:
    try:
        mode = path.stat().st_mode
    except OSError as error:
        _fail("APP_STAT_FAILED", error)
    if not stat.S_ISREG(mode):
        _fail("APP_SPECIAL_FILE", key)


def _verify_immutable(root: Path) -> None:
    """Reject a mutable tree before it is used as a runtime identity."""
    for path in (root, *root.rglob("*")):
        if path.is_symlink():
            _fail("APP_SYMLINK_FORBIDDEN", path.relative_to(root))
        try:
            mode = path.stat().st_mode
        except OSError as error:
            _fail("APP_STAT_FAILED", error)
        if not path.is_dir() and not path.is_file():
            _fail("APP_SPECIAL_FILE", path.relative_to(root) if path != root else ".")
        if mode & 0o222:
            _fail("APP_BUNDLE_MUTABLE", path.relative_to(root) if path != root else ".")


def _reject_symlink_components(path: Path, code: str) -> None:
    if not path.is_absolute():
        _fail(code, path)
    current = Path(path.anchor)
    for component in path.parts[1:]:
        current /= component
        if current.is_symlink():
            _fail(code, current)


def _build_record_digest(record: dict[str, Any]) -> str:
    unsigned = dict(record)
    unsigned.pop("recordDigest", None)
    return "sha256:" + hashlib.sha256(
        json.dumps(unsigned, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


def validate(manifest_path: Path | str, app_root: Path | str,
             base_sif: Path | str | None = None,
             *, verify_base_hash: bool = True) -> dict[str, Any]:
    raw_manifest = Path(manifest_path)
    raw_root = Path(app_root)
    _reject_symlink_components(raw_manifest, "APP_MANIFEST_SYMLINK_FORBIDDEN")
    _reject_symlink_components(raw_root, "APP_ROOT_SYMLINK_FORBIDDEN")
    manifest_file = raw_manifest.resolve()
    root = raw_root.resolve()
    if not manifest_file.is_file():
        _fail("APP_MANIFEST_MISSING", manifest_file)
    if not root.is_dir() or root.is_symlink():
        _fail("APP_ROOT_MISSING", root)
    _verify_immutable(root)
    try:
        record = json.loads(manifest_file.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        _fail("APP_MANIFEST_INVALID", type(error).__name__)
    if not isinstance(record, dict) or record.get("schemaVersion") != SCHEMA:
        _fail("APP_MANIFEST_SCHEMA")
    if record.get("status") != "PASS":
        _fail("APP_MANIFEST_NOT_PASS")
    if record.get("buildBoundary") != "container-runtime-in-sif-extracted":
        _fail("APP_BUILD_BOUNDARY")
    if record.get("candidateLayout") != APP_LAYOUT:
        _fail("APP_CANDIDATE_LAYOUT")
    if record.get("candidateVerification") != "PASS":
        _fail("APP_CANDIDATE_VERIFICATION")
    if record.get("tigerAction") != "verify-pair-and-execute-only":
        _fail("APP_TIGER_ACTION")
    if record.get("runtimeContract") != RUNTIME_CONTRACT:
        _fail("APP_RUNTIME_CONTRACT")
    if not DIGEST.fullmatch(str(record.get("appDigest", ""))):
        _fail("APP_DIGEST_MISSING")
    if record["appDigest"] != _record_digest(record):
        _fail("APP_MANIFEST_DIGEST_MISMATCH")

    for field in ("baseSif", "candidateSif", "candidateBuildRecord"):
        row = record.get(field)
        if not isinstance(row, dict) or not DIGEST.fullmatch(str(row.get("sha256", ""))):
            _fail("APP_IDENTITY_MISSING", field)
        if not isinstance(row.get("path"), str) or not Path(row["path"]).is_absolute():
            _fail("APP_IDENTITY_PATH", field)
    build_record = record["candidateBuildRecord"]
    if not DIGEST.fullmatch(str(build_record.get("recordDigest", ""))):
        _fail("APP_BUILD_RECORD_DIGEST_MISSING")
    if record["baseSif"]["sha256"] == record["candidateSif"]["sha256"]:
        _fail("APP_BASE_CANDIDATE_MUST_DIFFER")
    build_record_file = root / "manifest/candidate-build-record.json"
    if build_record_file.is_symlink():
        _fail("APP_BUILD_RECORD_ENTITY_SYMLINK")
    try:
        candidate_record = json.loads(build_record_file.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        _fail("APP_BUILD_RECORD_ENTITY_INVALID", type(error).__name__)
    if (not isinstance(candidate_record, dict)
            or candidate_record.get("schemaVersion") != "ndnsf-local-sif-build-v3"
            or candidate_record.get("status") != "PASS"
            or candidate_record.get("recordDigest") != _build_record_digest(candidate_record)
            or candidate_record.get("recordDigest") != build_record["recordDigest"]):
        _fail("APP_BUILD_RECORD_ENTITY_MISMATCH")
    if digest(build_record_file) != build_record["sha256"]:
        _fail("APP_BUILD_RECORD_ENTITY_DIGEST_MISMATCH")
    recorded_sif = candidate_record.get("sif")
    if not isinstance(recorded_sif, dict) or recorded_sif.get("sha256") != record["candidateSif"]["sha256"]:
        _fail("APP_BUILD_RECORD_CANDIDATE_MISMATCH")
    recorded_base = candidate_record.get("buildInput", {}).get("baseSif")
    if not isinstance(recorded_base, dict) or recorded_base.get("sha256") != record["baseSif"]["sha256"]:
        _fail("APP_BUILD_RECORD_BASE_MISMATCH")
    if base_sif is not None:
        raw_base = Path(base_sif)
        _reject_symlink_components(raw_base, "APP_BASE_SIF_SYMLINK_FORBIDDEN")
        base = raw_base.resolve()
        if not base.is_file():
            _fail("APP_BASE_SIF_MISSING", base)
        if verify_base_hash and digest(base) != record["baseSif"]["sha256"]:
            _fail("APP_BASE_SIF_DIGEST_MISMATCH", digest(base))

    apptainer = record.get("apptainer")
    if not isinstance(apptainer, dict):
        _fail("APP_APPTAINER_IDENTITY_MISSING")
    version = apptainer.get("version")
    expected_version = apptainer.get("expectedVersion")
    if (not isinstance(version, str) or not VERSION.fullmatch(version)
            or version != expected_version):
        _fail("APP_APPTAINER_VERSION")
    if not isinstance(apptainer.get("path"), str) or not Path(apptainer["path"]).is_absolute():
        _fail("APP_APPTAINER_PATH")
    if not DIGEST.fullmatch(str(apptainer.get("sha256", ""))):
        _fail("APP_APPTAINER_DIGEST")

    mounts = record.get("mounts")
    if not isinstance(mounts, list) or {row.get("source") for row in mounts if isinstance(row, dict)} != set(MOUNTS):
        _fail("APP_MOUNT_SET")
    observed_targets = set()
    for row in mounts:
        if not isinstance(row, dict):
            _fail("APP_MOUNT_ROW")
        source = _safe_relative(row.get("source"), "APP_MOUNT_SOURCE")
        target = row.get("target")
        if source.as_posix() not in MOUNTS or target != MOUNTS[source.as_posix()]:
            _fail("APP_MOUNT_CONTRACT", source)
        if target in observed_targets:
            _fail("APP_MOUNT_DUPLICATE", target)
        observed_targets.add(target)
        if not (root / source).is_dir():
            _fail("APP_MOUNT_SOURCE_MISSING", source)

    rows = record.get("files")
    if not isinstance(rows, list) or not rows:
        _fail("APP_FILE_MANIFEST_EMPTY")
    expected: dict[str, dict[str, Any]] = {}
    for row in rows:
        if not isinstance(row, dict):
            _fail("APP_FILE_ROW")
        relative = _safe_relative(row.get("path"))
        key = relative.as_posix()
        if key in expected:
            _fail("APP_FILE_DUPLICATE", key)
        if not DIGEST.fullmatch(str(row.get("sha256", ""))):
            _fail("APP_FILE_DIGEST", key)
        if type(row.get("bytes")) is not int or row["bytes"] < 0:
            _fail("APP_FILE_SIZE", key)
        path = root / relative
        try:
            path.resolve(strict=True).relative_to(root)
        except (OSError, ValueError):
            _fail("APP_FILE_OUTSIDE_BUNDLE", key)
        if not path.exists() and not path.is_symlink():
            _fail("APP_FILE_MISSING", key)
        _verify_symlink(root, path)
        if path.is_dir():
            _fail("APP_FILE_IS_DIRECTORY", key)
        _verify_regular_file(path, key)
        if path.stat().st_size != row["bytes"] or digest(path) != row["sha256"]:
            _fail("APP_FILE_CHANGED", key)
        expected[key] = row

    observed = _iter_entries(root)
    if observed != set(expected):
        _fail("APP_UNMANIFESTED_ENTRY", sorted(observed ^ set(expected))[0])
    if not REQUIRED_FILES.issubset(expected):
        _fail("APP_REQUIRED_ARTIFACT_MISSING", sorted(REQUIRED_FILES - set(expected))[0])

    entrypoints = record.get("entrypoints")
    if not isinstance(entrypoints, list) or not entrypoints:
        _fail("APP_ENTRYPOINTS")
    for entrypoint in entrypoints:
        if not isinstance(entrypoint, str) or not entrypoint.startswith("/opt/ndnsf-di/app/bin/"):
            _fail("APP_ENTRYPOINT_PATH", entrypoint)
        prefix = "/opt/ndnsf-di/app/"
        relative = entrypoint[len(prefix):] if entrypoint.startswith(prefix) else entrypoint
        if relative not in expected:
            _fail("APP_ENTRYPOINT_MISSING", entrypoint)

    return {
        "status": "PASS",
        "schemaVersion": SCHEMA,
        "appDigest": record["appDigest"],
        "baseSifSha256": record["baseSif"]["sha256"],
        "candidateSifSha256": record["candidateSif"]["sha256"],
        "candidateBuildRecordDigest": build_record["recordDigest"],
        "apptainerSha256": apptainer["sha256"],
        "files": len(expected),
        "entrypoints": entrypoints,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--app-root", required=True, type=Path)
    parser.add_argument("--base-sif", type=Path)
    parser.add_argument("--metadata-only", action="store_true")
    args = parser.parse_args()
    try:
        print(json.dumps(validate(args.manifest, args.app_root, args.base_sif,
                                  verify_base_hash=not args.metadata_only),
                            sort_keys=True, separators=(",", ":")))
    except SifAppError as error:
        print(str(error), file=__import__("sys").stderr)
        return 4
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
