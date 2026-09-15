#!/usr/bin/env python3
"""Materialize a relocatable NDNSF-DI APP from a verified candidate SIF.

The candidate SIF is the output of the container-native build.  Extraction is
performed through Apptainer so a host-built shared object or Python extension
cannot silently enter the APP directory.  The resulting directory and JSON
record are the portable half of the ``base SIF + APP`` pair.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from typing import Any


HERE = Path(__file__).resolve().parent
VALIDATOR_PATH = HERE / "validate-sif-app.py"
BUILD_RECORD_VALIDATOR = HERE / "validate-local-sif-build-record.py"
DIGEST = re.compile(r"^sha256:[0-9a-f]{64}$")
APP_LAYOUT = "opt/ndnsf-app"
EXPECTED_BINARIES = (
    "di-native-provider",
    "di-native-fault-provider",
    "App_ServiceController",
)
APP_NATIVE_LIBRARIES = (
    "libndn-service-framework.so.0.1.0",
    "libndnsf-distributed-inference.so",
    "libndn-svs.so.0.1.0",
    "libnac-abe.so",
    "libndnsd.so.0.1.0",
    "libopenabe.so",
    "librelic.so",
    "librelic_ec.so",
)
BASE_NATIVE_LIBRARIES = (
    "libonnxruntime.so.1", "libndn-cxx.so.0.9.0",
    "libboost_log.so.1.71.0", "libboost_stacktrace_backtrace.so.1.71.0",
    "libboost_chrono.so.1.71.0", "libboost_thread.so.1.71.0",
    "libboost_system.so.1.71.0", "libboost_filesystem.so.1.71.0",
    "libboost_program_options.so.1.71.0", "libboost_regex.so.1.71.0",
    "libboost_serialization.so.1.71.0", "libboost_iostreams.so.1.71.0",
    "libboost_date_time.so.1.71.0", "libboost_atomic.so.1.71.0",
    "libboost_log_setup.so.1.71.0", "libprotobuf.so.17", "libsqlite3.so.0",
    "libgmp.so.10", "libpcap.so.0.8", "libssl.so.1.1", "libcrypto.so.1.1",
    "libpthread.so.0", "libdl.so.2", "librt.so.1", "libstdc++.so.6",
    "libm.so.6", "libgcc_s.so.1", "libc.so.6", "ld-linux-x86-64.so.2",
)
BASE_RUNTIME_CHECK = r"""
set -eu
test -x /opt/venv/bin/python
test -d /opt/ndn-base/lib
test -d /opt/onnxruntime/lib
"""
_PUBLISH_LOCKS: list[object] = []
_PUBLISH_DIR_FDS: list[int] = []


class BuildSifAppError(ValueError):
    """A pair cannot be published without a complete provenance chain."""


def fail(code: str, detail: object = "") -> None:
    suffix = f":{detail}" if detail != "" else ""
    raise BuildSifAppError(code + suffix)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            value.update(chunk)
    return "sha256:" + value.hexdigest()


def identity(path: Path) -> tuple[int, int]:
    info = path.stat()
    return info.st_dev, info.st_ino


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        fail("APP_VALIDATOR_IMPORT")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def apptainer_version(binary: Path) -> str:
    try:
        fd = os.open(binary, os.O_RDONLY)
        try:
            output = subprocess.check_output(
                [f"/proc/self/fd/{fd}", "version"], text=True,
                stderr=subprocess.STDOUT, pass_fds=(fd,))
        finally:
            os.close(fd)
    except (OSError, subprocess.CalledProcessError) as error:
        fail("APP_APPTAINER_VERSION_FAILED", error)
    match = re.search(r"([0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.]+)?)", output)
    if not match:
        fail("APP_APPTAINER_VERSION_UNPARSEABLE", output.strip())
    return match.group(1)


def check_simple_path(path: Path, code: str) -> Path:
    if not path.is_absolute() or any(char in str(path) for char in ",\n\r:"):
        fail(code, path)
    current = Path(path.anchor)
    for component in path.parts[1:]:
        current /= component
        if current.is_symlink():
            fail(code + "_SYMLINK", current)
    resolved = path.resolve()
    return resolved


def check_apptainer_path(path: Path) -> Path:
    """Resolve a launcher symlink, then pin and record the real executable."""
    if not path.is_absolute() or any(char in str(path) for char in ",\n\r"):
        fail("APP_APPTAINER_PATH", path)
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        fail("APP_APPTAINER_MISSING", error)
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        fail("APP_APPTAINER_NOT_REGULAR", resolved)
    return resolved


def load_build_record(path: Path, candidate: Path, expected: str) -> dict[str, Any]:
    if not path.is_file():
        fail("APP_BUILD_RECORD_MISSING", path)
    module = load_module("ndnsf_build_record_validator", BUILD_RECORD_VALIDATOR)
    try:
        module.validate(path, candidate, expected, verify_sif_hash=True)
    except Exception as error:
        fail("APP_BUILD_RECORD_INVALID", error)
    try:
        record = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail("APP_BUILD_RECORD_INVALID", error)
    base = record.get("buildInput", {}).get("baseSif")
    if not isinstance(base, dict) or not DIGEST.fullmatch(str(base.get("sha256", ""))):
        fail("APP_BUILD_RECORD_BASE_MISSING")
    return record


def run_apptainer(apptainer: Path, arguments: list[str],
                   expected_identity: tuple[int, int], expected_digest: str,
                   *, pinned_image: Path | None = None,
                   image_identity: tuple[int, int] | None = None,
                   image_digest: str | None = None) -> None:
    """Execute pinned Apptainer and, when supplied, a pinned image inode."""
    image_fd: int | None = None
    pass_fds: list[int] = []
    command = list(arguments)
    try:
        if identity(apptainer) != expected_identity or digest(apptainer) != expected_digest:
            fail("APP_APPTAINER_CHANGED_BEFORE_USE")
        fd = os.open(apptainer, os.O_RDONLY)
        pass_fds.append(fd)
    except (OSError, BuildSifAppError) as error:
        fail("APP_APPTAINER_OPEN_FAILED", error)
    try:
        if os.fstat(fd).st_ino != expected_identity[1] or os.fstat(fd).st_dev != expected_identity[0]:
            fail("APP_APPTAINER_CHANGED_BEFORE_USE")
        fd_path = Path(f"/proc/self/fd/{fd}")
        if digest(fd_path) != expected_digest:
            fail("APP_APPTAINER_CHANGED_BEFORE_USE")
        if pinned_image is not None:
            if image_identity is None or image_digest is None:
                fail("APP_IMAGE_PIN_METADATA_MISSING")
            if identity(pinned_image) != image_identity or digest(pinned_image) != image_digest:
                fail("APP_IMAGE_CHANGED_BEFORE_USE")
            image_fd = os.open(pinned_image, os.O_RDONLY)
            pass_fds.append(image_fd)
            image_fd_path = Path(f"/proc/self/fd/{image_fd}")
            image_info = os.fstat(image_fd)
            if ((image_info.st_dev, image_info.st_ino) != image_identity
                    or digest(image_fd_path) != image_digest):
                fail("APP_IMAGE_CHANGED_BEFORE_USE")
            image_name = str(pinned_image)
            try:
                image_index = command.index(image_name)
            except ValueError:
                fail("APP_IMAGE_ARGUMENT_MISSING", image_name)
            command[image_index] = str(image_fd_path)
        subprocess.run([str(fd_path), *command], check=True, pass_fds=tuple(pass_fds))
    except (OSError, subprocess.CalledProcessError) as error:
        fail("APP_APPTAINER_EXEC_FAILED", error)
    finally:
        os.close(fd)
        if image_fd is not None:
            os.close(image_fd)
    if identity(apptainer) != expected_identity or digest(apptainer) != expected_digest:
        fail("APP_APPTAINER_CHANGED_AFTER_USE")
    if pinned_image is not None and (
            identity(pinned_image) != image_identity or digest(pinned_image) != image_digest):
        fail("APP_IMAGE_CHANGED_AFTER_USE")


def run_in_candidate(apptainer: Path, candidate: Path, script: str,
                     expected_identity: tuple[int, int], expected_digest: str,
                     app_override: Path | None = None,
                     error_code: str = "APP_CANDIDATE_VERIFICATION_FAILED") -> None:
    arguments = ["exec", "--cleanenv", "--containall"]
    if app_override is not None:
        arguments.extend(["--bind", f"{app_override}:/opt/ndnsf-app:ro"])
    arguments.extend([str(candidate), "/bin/sh", "-c", script])
    candidate_identity = identity(candidate)
    candidate_digest = digest(candidate)
    try:
        run_apptainer(apptainer, arguments, expected_identity, expected_digest,
                      pinned_image=candidate, image_identity=candidate_identity,
                      image_digest=candidate_digest)
    except BuildSifAppError as error:
        fail(error_code, error)


def verify_base_runtime(apptainer: Path, base: Path,
                        apptainer_identity: tuple[int, int],
                        apptainer_digest: str,
                        base_identity: tuple[int, int],
                        base_digest: str) -> None:
    """Check the stable paths required by the APP runner before publishing."""
    try:
        run_apptainer(
            apptainer,
            ["exec", "--cleanenv", "--containall", str(base),
             "/bin/sh", "-c", BASE_RUNTIME_CHECK],
            apptainer_identity,
            apptainer_digest,
            pinned_image=base,
            image_identity=base_identity,
            image_digest=base_digest,
        )
    except BuildSifAppError as error:
        fail("APP_BASE_RUNTIME_CONTRACT_FAILED", error)


def extract(apptainer: Path, candidate: Path, output: Path,
            expected_identity: tuple[int, int], expected_digest: str) -> None:
    """Copy only application-owned artifacts through the candidate runtime."""
    output.mkdir(exist_ok=True)
    for name in ("lib", "bin", "python", "manifest", "replay"):
        (output / name).mkdir()
    script = r"""
set -eu
out=/out
test -d /opt/ndnsf-app/manifest
cp -a /opt/ndnsf-app/. "$out/"
"""
    command = ["exec", "--cleanenv", "--containall", "--bind",
               f"{output}:/out:rw", str(candidate), "/bin/sh", "-c", script]
    try:
        run_apptainer(apptainer, command, expected_identity, expected_digest,
                      pinned_image=candidate, image_identity=identity(candidate),
                      image_digest=digest(candidate))
    except BuildSifAppError as error:
        fail("APP_EXTRACTION_FAILED", error)


def verify_materialized(apptainer: Path, candidate: Path, output: Path,
                        expected_identity: tuple[int, int],
                        expected_digest: str) -> None:
    """Re-run the candidate closure checks against the exact extracted tree."""
    script = r"""
set -eu
app=/opt/ndnsf-app
test -d "$app/bin" -a -d "$app/lib" -a -d "$app/python" -a -d "$app/manifest"
export LD_LIBRARY_PATH="$app/lib:/opt/ndn-base/lib:/opt/onnxruntime/lib"
/opt/venv/bin/python - "$app" <<'PY'
import os
import stat
import subprocess
import sys

root = sys.argv[1]
if not stat.S_ISDIR(os.lstat(root).st_mode):
    raise SystemExit("APP_MATERIALIZED_ROOT_NOT_DIRECTORY")

def onerror(error):
    raise SystemExit("APP_MATERIALIZED_WALK_FAILED:" + str(error))

app_libraries = {
    "libndn-service-framework.so.0.1.0", "libndnsf-distributed-inference.so",
    "libndn-svs.so.0.1.0", "libnac-abe.so", "libndnsd.so.0.1.0",
    "libopenabe.so", "librelic.so", "librelic_ec.so",
}
forbidden_roots = ("/opt/ndnsf-di/current/", "/opt/ndnsf-stage/", "/src/", "/usr/local/lib/")

def verify_elf_resolution(path, output):
    if "not found" in output:
        raise SystemExit("APP_MATERIALIZED_ELF_DEPENDENCY_FAILED:" + path)
    for line in output.splitlines():
        fields = line.strip().split()
        if len(fields) < 3 or fields[1] != "=>":
            continue
        name, resolved = fields[0], fields[2]
        if name in app_libraries and not resolved.startswith(root + "/lib/"):
            raise SystemExit("APP_MATERIALIZED_ELF_LIBRARY_ORIGIN_MISMATCH:" + name + ":" + resolved)
        if any(resolved.startswith(prefix) for prefix in forbidden_roots):
            raise SystemExit("APP_MATERIALIZED_ELF_FORBIDDEN_RESOLVED_PATH:" + resolved)

for directory, subdirectories, filenames in os.walk(root, onerror=onerror,
                                                    followlinks=False):
    for name in sorted(subdirectories + filenames):
        path = os.path.join(directory, name)
        mode = os.lstat(path).st_mode
        if stat.S_ISLNK(mode):
            raise SystemExit("APP_MATERIALIZED_SYMLINK:" + path)
        if name in subdirectories:
            valid = stat.S_ISDIR(mode)
        else:
            valid = stat.S_ISREG(mode)
        if not valid:
            raise SystemExit("APP_MATERIALIZED_SPECIAL_FILE:" + path)

for directory, subdirectories, filenames in os.walk(root, onerror=onerror,
                                                    followlinks=False):
    subdirectories.sort()
    for filename in sorted(filenames):
        path = os.path.join(directory, filename)
        try:
            with open(path, "rb") as stream:
                if stream.read(4) != b"\x7fELF":
                    continue
        except OSError as error:
            raise SystemExit("APP_MATERIALIZED_ELF_READ_FAILED:" + str(error))
        result = subprocess.run(["ldd", path], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode != 0:
            raise SystemExit("APP_MATERIALIZED_ELF_DEPENDENCY_FAILED:" + path)
        verify_elf_resolution(path, result.stdout)
PY
for name in di-native-provider di-native-fault-provider App_ServiceController; do
    test -x "$app/bin/$name"
done
for name in libndn-service-framework.so.0.1.0 libndnsf-distributed-inference.so \
            libndn-svs.so.0.1.0 libnac-abe.so libndnsd.so.0.1.0 \
            libopenabe.so librelic.so librelic_ec.so; do
    test -f "$app/lib/$name"
done
LD_LIBRARY_PATH="$app/lib:/opt/ndn-base/lib:/opt/onnxruntime/lib" \
PYTHONPATH="$app/python" PYTHONNOUSERSITE=1 /opt/venv/bin/python \
    -c 'import ndnsf._ndnsf, ndnsf_distributed_inference, py_repoclient'
"""
    run_in_candidate(apptainer, candidate, script, expected_identity,
                     expected_digest, app_override=output,
                     error_code="APP_MATERIALIZED_VERIFICATION_FAILED")


def file_rows(root: Path) -> list[dict[str, Any]]:
    rows = []
    for path in sorted(root.rglob("*")):
        if path.is_dir():
            continue
        if not path.is_symlink() and not path.is_file():
            fail("APP_SPECIAL_FILE", path.relative_to(root))
        try:
            path.resolve(strict=True).relative_to(root)
        except (OSError, ValueError):
            fail("APP_SYMLINK_TARGET_OUTSIDE_BUNDLE", path)
        if path.is_symlink():
            try:
                mode = path.stat().st_mode
            except OSError as error:
                fail("APP_STAT_FAILED", error)
            if not stat.S_ISREG(mode):
                fail("APP_SPECIAL_FILE", path.relative_to(root))
        rows.append({
            "path": path.relative_to(root).as_posix(),
            "bytes": path.stat().st_size,
            "sha256": digest(path),
        })
    return rows


def verify_elf_runtime_paths(root: Path) -> None:
    """Reject builder/compatibility paths embedded in published APP ELF files."""
    forbidden = ("/opt/ndnsf-di/current", "/opt/ndnsf-stage", "/src/")
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        try:
            with path.open("rb") as stream:
                if stream.read(4) != b"\x7fELF":
                    continue
        except OSError as error:
            fail("APP_ELF_READ_FAILED", error)
        result = subprocess.run(
            ["/usr/bin/readelf", "-d", str(path)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if result.returncode != 0:
            fail("APP_ELF_DYNAMIC_SECTION_FAILED", path)
        runpath_lines = [
            line for line in result.stdout.splitlines()
            if "(RPATH)" in line or "(RUNPATH)" in line
        ]
        for marker in forbidden:
            if any(marker in line for line in runpath_lines):
                fail("APP_ELF_FORBIDDEN_RUNTIME_PATH", f"{path}:{marker}")


def reject_manifest_symlinks(root: Path) -> None:
    for path in root.rglob("*"):
        if path.is_symlink():
            fail("APP_CANDIDATE_MANIFEST_SYMLINK", path.relative_to(root))


def canonical(record: dict[str, Any]) -> dict[str, Any]:
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


def record_digest(record: dict[str, Any]) -> str:
    return "sha256:" + hashlib.sha256(
        json.dumps(canonical(record), sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


def immutable(root: Path) -> None:
    """Make the published APP read-only while leaving directory traversal."""
    for path in sorted(root.rglob("*"), reverse=True):
        if path.is_symlink():
            continue
        mode = path.stat().st_mode
        if path.is_dir():
            path.chmod((mode & 0o111) | 0o555)
        else:
            path.chmod((mode & 0o111) | 0o444)
    root.chmod(0o555)


def remove_tree(path: Path) -> None:
    """Remove a failed staging/published tree even after immutable()."""
    if not path.exists() and not path.is_symlink():
        return
    if path.is_symlink() or not path.is_dir():
        path.unlink(missing_ok=True)
        return
    for child in path.rglob("*"):
        if child.is_symlink():
            continue
        try:
            mode = child.stat().st_mode
            child.chmod(mode | (0o700 if child.is_dir() else 0o600))
        except OSError:
            pass
    try:
        path.chmod(path.stat().st_mode | 0o700)
    except OSError:
        pass
    shutil.rmtree(path, ignore_errors=False)


def remove_tree_at(name: str, parent_fd: int) -> None:
    """Recursively remove one child using no-follow directory-FD operations."""
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        directory_fd = os.open(name, flags, dir_fd=parent_fd)
    except NotADirectoryError:
        os.unlink(name, dir_fd=parent_fd)
        return
    try:
        # Publication staging is deliberately made immutable before the
        # process can crash.  Restore owner write/search permission through
        # the pinned descriptor before unlinking children; otherwise cleanup
        # can fail on a valid but immutable staging tree.
        os.fchmod(directory_fd, os.fstat(directory_fd).st_mode | 0o700)
        with os.scandir(directory_fd) as entries:
            for entry in entries:
                if entry.is_symlink():
                    os.unlink(entry.name, dir_fd=directory_fd)
                elif entry.is_dir(follow_symlinks=False):
                    remove_tree_at(entry.name, directory_fd)
                else:
                    os.unlink(entry.name, dir_fd=directory_fd)
        os.rmdir(name, dir_fd=parent_fd)
    finally:
        os.close(directory_fd)


def ensure_publish_parent(parent: Path, parent_fd: int,
                          expected_identity: tuple[int, int]) -> None:
    try:
        if identity(parent) != expected_identity:
            fail("APP_PUBLISH_PARENT_CHANGED")
        info = os.fstat(parent_fd)
    except OSError as error:
        fail("APP_PUBLISH_PARENT_CHECK_FAILED", error)
    if (info.st_dev, info.st_ino) != expected_identity:
        fail("APP_PUBLISH_PARENT_CHANGED")


def replace_in_publish_parent(source: Path, target: Path, parent: Path,
                              parent_fd: int,
                              expected_identity: tuple[int, int],
                              expected_source_identity: tuple[int, int]) -> None:
    ensure_publish_parent(parent, parent_fd, expected_identity)
    try:
        source_info = os.stat(source.name, dir_fd=parent_fd,
                              follow_symlinks=False)
        if ((source_info.st_dev, source_info.st_ino) != expected_source_identity
                or (not stat.S_ISREG(source_info.st_mode)
                    and not stat.S_ISDIR(source_info.st_mode))):
            fail("APP_PUBLISH_SOURCE_CHANGED", source)
        os.replace(source.name, target.name, src_dir_fd=parent_fd,
                   dst_dir_fd=parent_fd)
    except (OSError, BuildSifAppError) as error:
        fail("APP_PUBLISH_RENAME_FAILED", error)


def safe_remove_staging(path: Path, parent: Path, parent_fd: int,
                        expected_identity: tuple[int, int]) -> None:
    try:
        ensure_publish_parent(parent, parent_fd, expected_identity)
    except BuildSifAppError:
        return
    if path.parent != parent:
        return
    try:
        remove_tree_at(path.name, parent_fd)
    except OSError:
        pass


STAGING_TOKEN = ".ndnsf-sif-app-staging.json"


def write_staging_token(temp: Path, output: Path, app_record: Path,
                        temp_record: Path,
                        temp_record_identity: list[int] | None) -> None:
    token = temp / STAGING_TOKEN
    payload = {
        "schemaVersion": "ndnsf-sif-app-staging-v1",
        "output": str(output),
        "record": str(app_record),
        "temp": str(temp),
        "tempRecord": str(temp_record),
        "tempIdentity": marker_identity(temp),
        "tempRecordIdentity": temp_record_identity,
    }
    directory_fd = os.open(
        temp,
        os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0),
    )
    token_temp_name = f"{STAGING_TOKEN}.tmp.{os.getpid()}"
    try:
        fd = os.open(
            token_temp_name,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
            0o600,
            dir_fd=directory_fd,
        )
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as stream:
                json.dump(payload, stream, sort_keys=True, separators=(",", ":"))
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(token_temp_name, STAGING_TOKEN,
                       src_dir_fd=directory_fd, dst_dir_fd=directory_fd)
            os.fsync(directory_fd)
        except BaseException:
            try:
                os.unlink(token_temp_name, dir_fd=directory_fd)
            except OSError:
                pass
            raise
    finally:
        os.close(directory_fd)


def recover_orphan_staging(parent: Path, output: Path,
                           app_record: Path, parent_fd: int | None = None,
                           parent_identity: tuple[int, int] | None = None) -> None:
    """Remove only staging trees carrying this tool's exact transaction token."""
    if parent_fd is not None and parent_identity is not None:
        ensure_publish_parent(parent, parent_fd, parent_identity)
    prefix = output.name + ".tmp."
    try:
        entries = list(parent.iterdir())
    except OSError as error:
        fail("APP_STAGING_SCAN_FAILED", error)
    for candidate in entries:
        if not candidate.name.startswith(prefix) or candidate.is_symlink() or not candidate.is_dir():
            continue
        token = candidate / STAGING_TOKEN
        if token.is_symlink():
            continue
        if not token.exists():
            try:
                suffix = candidate.name[len(prefix):]
                candidate_info = None
                if parent_fd is not None and parent_identity is not None:
                    candidate_info = os.stat(candidate.name, dir_fd=parent_fd,
                                             follow_symlinks=False)
                    if not stat.S_ISDIR(candidate_info.st_mode):
                        continue
                else:
                    candidate_info = candidate.stat()
                placeholder_record = (parent / (app_record.name + ".tmp." + suffix)
                                      if suffix.isdigit() else None)
                placeholder_identity = None
                if placeholder_record is not None:
                    try:
                        if parent_fd is not None:
                            placeholder_fd = os.open(
                                placeholder_record.name,
                                os.O_RDONLY | os.O_NONBLOCK
                                | getattr(os, "O_NOFOLLOW", 0),
                                dir_fd=parent_fd,
                            )
                            try:
                                placeholder_info = os.fstat(placeholder_fd)
                            finally:
                                os.close(placeholder_fd)
                        else:
                            placeholder_info = placeholder_record.stat()
                        if (stat.S_ISREG(placeholder_info.st_mode)
                                and placeholder_info.st_size == 0
                                and not placeholder_info.st_mode & 0o077
                                and placeholder_info.st_uid == os.getuid()):
                            placeholder_identity = [placeholder_info.st_dev,
                                                    placeholder_info.st_ino]
                    except OSError:
                        placeholder_identity = None
                children = list(candidate.iterdir())
                token_temp_prefix = STAGING_TOKEN + ".tmp."
                if children and not all(
                        child.name.startswith(token_temp_prefix)
                        and child.is_file() and not child.is_symlink()
                        for child in children):
                    continue
                if parent_fd is not None and parent_identity is not None:
                    ensure_publish_parent(parent, parent_fd, parent_identity)
                    directory_fd = os.open(
                        candidate.name,
                        os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                        | getattr(os, "O_NOFOLLOW", 0),
                        dir_fd=parent_fd,
                    )
                    try:
                        current_info = os.fstat(directory_fd)
                        if ([current_info.st_dev, current_info.st_ino]
                                != [candidate_info.st_dev, candidate_info.st_ino]):
                            continue
                        os.fchmod(directory_fd, os.fstat(directory_fd).st_mode | 0o700)
                        for child in children:
                            os.unlink(child.name, dir_fd=directory_fd)
                        os.rmdir(candidate.name, dir_fd=parent_fd)
                    finally:
                        os.close(directory_fd)
                else:
                    for child in children:
                        child.unlink()
                    candidate.rmdir()
                if placeholder_record is not None and placeholder_identity is not None:
                    try:
                        if parent_fd is not None:
                            current = os.stat(placeholder_record.name, dir_fd=parent_fd,
                                              follow_symlinks=False)
                            if [current.st_dev, current.st_ino] == placeholder_identity:
                                os.unlink(placeholder_record.name, dir_fd=parent_fd)
                        elif placeholder_record.exists() and not placeholder_record.is_symlink():
                            current = placeholder_record.stat()
                            if [current.st_dev, current.st_ino] == placeholder_identity:
                                placeholder_record.unlink()
                    except OSError:
                        pass
            except OSError:
                pass
            continue
        if not token.is_file():
            continue
        try:
            payload = json.loads(token.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if (isinstance(payload, dict)
                and payload.get("schemaVersion") == "ndnsf-sif-app-staging-v1"
                and payload.get("output") == str(output)
                and payload.get("record") == str(app_record)
                and payload.get("temp") == str(candidate)
                and isinstance(payload.get("tempRecord"), str)):
            temp_record = Path(payload["tempRecord"])
            if (temp_record.parent != parent
                    or not temp_record.name.startswith(app_record.name + ".tmp.")
                    or temp_record.is_symlink()):
                continue
            expected_temp = payload.get("tempIdentity")
            expected_record = payload.get("tempRecordIdentity")
            if (not isinstance(expected_temp, list) or len(expected_temp) != 2
                    or (expected_record is not None
                        and (not isinstance(expected_record, list)
                             or len(expected_record) != 2))
                    or marker_identity(candidate) != expected_temp
                    or (expected_record is not None
                        and marker_identity(temp_record) != expected_record)):
                continue
            if expected_record is None:
                # The intent token is written before the placeholder record.
                # Remove only the exact zero-byte, owner-private placeholder
                # that this protocol creates; never guess at a user file.
                try:
                    if parent_fd is not None:
                        placeholder_fd = os.open(
                            temp_record.name,
                            os.O_RDONLY | os.O_NONBLOCK
                            | getattr(os, "O_NOFOLLOW", 0),
                            dir_fd=parent_fd,
                        )
                        try:
                            info = os.fstat(placeholder_fd)
                        finally:
                            os.close(placeholder_fd)
                    else:
                        info = temp_record.stat()
                    if (not stat.S_ISREG(info.st_mode) or info.st_size != 0
                            or info.st_mode & 0o077 or info.st_uid != os.getuid()):
                        continue
                    expected_record = [info.st_dev, info.st_ino]
                except FileNotFoundError:
                    pass
                except OSError:
                    continue
            if expected_record is not None:
                try:
                    if marker_identity(temp_record) != expected_record:
                        continue
                except BuildSifAppError:
                    continue
            if parent_fd is not None and parent_identity is not None:
                ensure_publish_parent(parent, parent_fd, parent_identity)
                try:
                    remove_tree_at(candidate.name, parent_fd)
                except FileNotFoundError:
                    pass
                try:
                    remove_tree_at(temp_record.name, parent_fd)
                except FileNotFoundError:
                    pass
            else:
                remove_tree(candidate)
                remove_tree(temp_record)


def publish_marker_path(output: Path, app_record: Path) -> Path:
    marker_id = hashlib.sha256(
        f"{output}\0{app_record}".encode("utf-8")
    ).hexdigest()
    return output.parent / f".ndnsf-sif-app-publish-{marker_id}.json"


def recover_orphan_marker_temps(parent: Path, marker: Path,
                                parent_fd: int, parent_identity: tuple[int, int]) -> None:
    """Remove only interrupted atomic-marker temp files for this pair."""
    ensure_publish_parent(parent, parent_fd, parent_identity)
    prefix = marker.name + ".tmp."
    try:
        entries = list(parent.iterdir())
    except OSError as error:
        fail("APP_MARKER_SCAN_FAILED", error)
    for entry in entries:
        if not entry.name.startswith(prefix):
            continue
        try:
            info = os.stat(entry.name, dir_fd=parent_fd, follow_symlinks=False)
            if not stat.S_ISREG(info.st_mode):
                continue
            current = os.stat(entry.name, dir_fd=parent_fd, follow_symlinks=False)
            if [current.st_dev, current.st_ino] != [info.st_dev, info.st_ino]:
                continue
            ensure_publish_parent(parent, parent_fd, parent_identity)
            os.unlink(entry.name, dir_fd=parent_fd)
        except OSError:
            continue


def marker_identity(path: Path) -> list[int] | None:
    if path.is_symlink():
        fail("APP_PUBLISH_RECOVERY_REQUIRED", path)
    try:
        info = path.stat()
    except FileNotFoundError:
        return None
    except OSError as error:
        fail("APP_PUBLISH_RECOVERY_REQUIRED", error)
    return [info.st_dev, info.st_ino]


def write_json_atomically(path: Path, payload: dict[str, Any],
                          parent_fd: int | None = None) -> None:
    """Write a small transaction record and publish it with one directory FD."""
    temporary_name = f"{path.name}.tmp.{os.getpid()}"
    if parent_fd is None:
        temporary = path.with_name(temporary_name)
        with temporary.open("x", encoding="utf-8") as stream:
            json.dump(payload, stream, sort_keys=True, separators=(",", ":"))
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory_fd = os.open(
            path.parent,
            os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0),
        )
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
        return
    fd = os.open(
        temporary_name,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
        0o600,
        dir_fd=parent_fd,
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(payload, stream, sort_keys=True, separators=(",", ":"))
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_name, path.name,
                   src_dir_fd=parent_fd, dst_dir_fd=parent_fd)
        os.fsync(parent_fd)
    except BaseException:
        try:
            os.unlink(temporary_name, dir_fd=parent_fd)
        except OSError:
            pass
        raise


def write_temp_record(path: Path, payload: dict[str, Any], parent_fd: int,
                      expected_identity: tuple[int, int]) -> None:
    """Write the manifest through a no-follow FD for its whole lifetime."""
    try:
        fd = os.open(
            path.name,
            os.O_WRONLY | os.O_NONBLOCK
            | getattr(os, "O_NOFOLLOW", 0),
            dir_fd=parent_fd,
        )
    except OSError as error:
        fail("APP_TEMP_RECORD_OPEN_FAILED", error)
    try:
        info = os.fstat(fd)
        if ((info.st_dev, info.st_ino) != expected_identity
                or not stat.S_ISREG(info.st_mode)):
            fail("APP_TEMP_RECORD_CHANGED")
        os.ftruncate(fd, 0)
        stream = os.fdopen(fd, "w", encoding="utf-8")
        try:
            json.dump(payload, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
            os.fchmod(stream.fileno(), 0o444)
            os.fsync(stream.fileno())
        finally:
            stream.close()
    except BaseException:
        try:
            os.close(fd)
        except OSError:
            pass
        raise
    try:
        info = os.stat(path.name, dir_fd=parent_fd, follow_symlinks=False)
    except OSError as error:
        fail("APP_TEMP_RECORD_CHANGED", error)
    if [info.st_dev, info.st_ino] != list(expected_identity):
        fail("APP_TEMP_RECORD_CHANGED")


def write_publish_marker(marker: Path, state: str, output: Path,
                         app_record: Path, temp: Path,
                         temp_record: Path, parent: Path | None = None,
                         parent_fd: int | None = None,
                         parent_identity: tuple[int, int] | None = None) -> None:
    if parent is not None and parent_fd is not None and parent_identity is not None:
        ensure_publish_parent(parent, parent_fd, parent_identity)
    temp_id = marker_identity(temp)
    temp_record_id = marker_identity(temp_record)
    if state == "prepared":
        output_id, record_id = temp_id, temp_record_id
    elif state == "app-published":
        output_id, record_id = marker_identity(output), temp_record_id
    elif state == "pair-published":
        output_id, record_id = marker_identity(output), marker_identity(app_record)
    else:
        fail("APP_PUBLISH_STATE_INVALID", state)
    payload = {
        "schemaVersion": "ndnsf-sif-app-transaction-v1",
        "state": state,
        "output": str(output),
        "record": str(app_record),
        "temp": str(temp),
        "tempRecord": str(temp_record),
        "tempIdentity": temp_id,
        "tempRecordIdentity": temp_record_id,
        "outputIdentity": output_id,
        "recordIdentity": record_id,
    }
    if parent is not None and parent_fd is not None and parent_identity is not None:
        ensure_publish_parent(parent, parent_fd, parent_identity)
        write_json_atomically(marker, payload, parent_fd)
    else:
        write_json_atomically(marker, payload)


def recover_publish_marker(marker: Path, output: Path, app_record: Path,
                           parent_fd: int | None = None,
                           parent_identity: tuple[int, int] | None = None) -> None:
    """Finish or roll back an interrupted two-file publication."""
    marker_file_identity: list[int] | None = None

    def check_parent() -> None:
        if parent_fd is not None and parent_identity is not None:
            ensure_publish_parent(output.parent, parent_fd, parent_identity)

    def remove_child(path: Path) -> None:
        check_parent()
        if parent_fd is not None and path.parent == output.parent:
            try:
                remove_tree_at(path.name, parent_fd)
            except FileNotFoundError:
                pass
        else:
            remove_tree(path)

    def unlink_marker() -> None:
        check_parent()
        try:
            if parent_fd is not None:
                info = os.stat(marker.name, dir_fd=parent_fd, follow_symlinks=False)
                if (marker_file_identity is not None
                        and [info.st_dev, info.st_ino] != marker_file_identity):
                    fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
                os.unlink(marker.name, dir_fd=parent_fd)
            else:
                marker.unlink()
        except FileNotFoundError:
            pass

    check_parent()
    if parent_fd is not None:
        try:
            marker_fd = os.open(
                marker.name,
                os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0),
                dir_fd=parent_fd,
            )
        except FileNotFoundError:
            return
        try:
            marker_info = os.fstat(marker_fd)
            if not stat.S_ISREG(marker_info.st_mode):
                os.close(marker_fd)
                fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
            marker_file_identity = [marker_info.st_dev, marker_info.st_ino]
            with os.fdopen(marker_fd, "r", encoding="utf-8") as stream:
                payload = json.load(stream)
        except (OSError, json.JSONDecodeError) as error:
            fail("APP_PUBLISH_RECOVERY_REQUIRED", error)
    else:
        if not marker.exists():
            return
        if marker.is_symlink() or not marker.is_file():
            fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
        try:
            marker_info = marker.stat()
            marker_file_identity = [marker_info.st_dev, marker_info.st_ino]
            payload = json.loads(marker.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            fail("APP_PUBLISH_RECOVERY_REQUIRED", error)
    if (not isinstance(payload, dict)
            or payload.get("schemaVersion") != "ndnsf-sif-app-transaction-v1"
            or payload.get("output") != str(output)
            or payload.get("record") != str(app_record)
            or not isinstance(payload.get("temp"), str)
            or not isinstance(payload.get("tempRecord"), str)):
        fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
    state = payload.get("state")
    temp = Path(payload.get("temp", ""))
    temp_record = Path(payload.get("tempRecord", ""))
    if (temp.parent != output.parent or temp_record.parent != output.parent
            or not temp.name.startswith(output.name + ".tmp.")
            or not temp_record.name.startswith(app_record.name + ".tmp.")):
        fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
    if state == "pair-published":
        expected_output = payload.get("outputIdentity")
        expected_record = payload.get("recordIdentity")
        if (not isinstance(expected_output, list) or len(expected_output) != 2
                or not isinstance(expected_record, list) or len(expected_record) != 2
                or any(type(item) is not int for item in expected_output + expected_record)):
            fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
        if (output.is_dir() and not output.is_symlink()
                and app_record.is_file() and not app_record.is_symlink()
                and marker_identity(output) == expected_output
                and marker_identity(app_record) == expected_record
                and marker_identity(temp) is None
                and marker_identity(temp_record) is None):
            check_parent()
            unlink_marker()
            return
        fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
    if state not in {"prepared", "app-published"}:
        fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
    expected_temp = payload.get("tempIdentity")
    expected_temp_record = payload.get("tempRecordIdentity")
    expected_output = payload.get("outputIdentity")
    expected_record = payload.get("recordIdentity")
    for value in (expected_temp, expected_temp_record, expected_output, expected_record):
        if value is not None and (not isinstance(value, list) or len(value) != 2
                                   or any(type(item) is not int for item in value)):
            fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
    if marker_identity(temp) not in (None, expected_temp):
        fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
    if marker_identity(temp_record) not in (None, expected_temp_record):
        fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
    if state == "prepared":
        if marker_identity(output) not in (None, expected_temp):
            fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
        if marker_identity(app_record) not in (None, expected_temp_record):
            fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
    if state == "app-published" and expected_output is None:
        fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
    if (state == "app-published" and output.is_dir()
            and not output.is_symlink() and app_record.is_file()
            and not app_record.is_symlink()):
        if (marker_identity(output) != expected_output
                or marker_identity(app_record) != expected_record):
            fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
        check_parent()
        remove_child(temp)
        remove_child(temp_record)
        unlink_marker()
        return
    check_parent()
    remove_child(temp)
    remove_child(temp_record)
    check_parent()
    if marker_identity(output) == expected_output:
        remove_child(output)
    elif marker_identity(output) is not None:
        fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
    if marker_identity(app_record) == expected_record:
        remove_child(app_record)
    elif marker_identity(app_record) is not None:
        fail("APP_PUBLISH_RECOVERY_REQUIRED", marker)
    check_parent()
    unlink_marker()


def publish(args: argparse.Namespace) -> dict[str, Any]:
    base = check_simple_path(Path(args.base_sif), "APP_BASE_SIF_PATH")
    candidate = check_simple_path(Path(args.candidate_sif), "APP_CANDIDATE_SIF_PATH")
    record_path = check_simple_path(Path(args.candidate_record), "APP_BUILD_RECORD_PATH")
    output = check_simple_path(Path(args.output).expanduser(), "APP_OUTPUT_PATH")
    app_record_path = check_simple_path(Path(args.record).expanduser(), "APP_RECORD_PATH")
    if not base.is_file() or not candidate.is_file():
        fail("APP_SIF_MISSING")
    if output.parent != app_record_path.parent:
        fail("APP_RECORD_MUST_SHARE_PARENT")
    if not output.parent.is_dir() or output.parent.is_symlink():
        fail("APP_OUTPUT_PARENT_MISSING", output.parent)
    try:
        parent_identity = identity(output.parent)
        parent_fd = os.open(
            output.parent,
            os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0),
        )
        info = os.fstat(parent_fd)
        if (info.st_dev, info.st_ino) != parent_identity:
            os.close(parent_fd)
            fail("APP_PUBLISH_PARENT_CHANGED")
    except OSError as error:
        fail("APP_PUBLISH_PARENT_OPEN_FAILED", error)
    _PUBLISH_DIR_FDS.append(parent_fd)
    lock_streams = []
    try:
        # Lock both publication targets in lexical order.  This serializes
        # same-output and same-manifest races, including different pair names
        # that accidentally reuse one manifest path.
        for target in sorted({str(output), str(app_record_path)}):
            lock_id = hashlib.sha256(target.encode("utf-8")).hexdigest()
            lock_path = Path("/tmp") / f"ndnsf-sif-app-{lock_id}.lock"
            lock_fd = os.open(
                lock_path,
                os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0),
                0o600,
            )
            lock_info = os.fstat(lock_fd)
            if not stat.S_ISREG(lock_info.st_mode):
                os.close(lock_fd)
                raise OSError(f"APP_PUBLISH_LOCK_NOT_REGULAR:{lock_path}")
            lock_stream = os.fdopen(lock_fd, "a+")
            fcntl.flock(lock_stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            lock_streams.append(lock_stream)
    except BlockingIOError:
        for stream in lock_streams:
            stream.close()
        fail("APP_OUTPUT_BUSY", output)
    except OSError as error:
        for stream in lock_streams:
            stream.close()
        fail("APP_PUBLISH_LOCK_FAILED", error)
    # Keep the descriptor alive until publication (and cleanup) is complete.
    _PUBLISH_LOCKS.extend(lock_streams)
    marker = publish_marker_path(output, app_record_path)
    ensure_publish_parent(output.parent, parent_fd, parent_identity)
    recover_orphan_marker_temps(output.parent, marker, parent_fd, parent_identity)
    recover_publish_marker(marker, output, app_record_path, parent_fd,
                           parent_identity)
    recover_orphan_staging(output.parent, output, app_record_path, parent_fd,
                           parent_identity)
    if output.exists() or app_record_path.exists():
        fail("APP_OUTPUT_EXISTS")
    try:
        app_record_path.relative_to(output)
    except ValueError:
        pass
    else:
        fail("APP_RECORD_MUST_BE_ADJACENT")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.]+)?", args.expected_apptainer):
        fail("APP_EXPECTED_APPTAINER_INVALID")
    apptainer = check_apptainer_path(Path(args.apptainer))
    actual_version = apptainer_version(apptainer)
    if actual_version != args.expected_apptainer:
        fail("APP_APPTAINER_VERSION_MISMATCH", f"local={actual_version} expected={args.expected_apptainer}")
    apptainer_identity = identity(apptainer)
    apptainer_sha = digest(apptainer)
    base_sha = digest(base)
    candidate_sha = digest(candidate)
    build_record = load_build_record(record_path, candidate, candidate_sha)
    if build_record["buildInput"]["baseSif"]["sha256"] != base_sha:
        fail("APP_BASE_DIGEST_MISMATCH", build_record["buildInput"]["baseSif"]["sha256"])
    base_identity = identity(base)
    candidate_identity = identity(candidate)
    record_identity = identity(record_path)
    record_sha = digest(record_path)
    verify_base_runtime(apptainer, base, apptainer_identity, apptainer_sha,
                        base_identity, base_sha)
    input_temp: Path | None = None
    try:
        fd, input_name = tempfile.mkstemp(prefix="ndnsf-sif-app-candidate-", suffix=".sif")
        os.close(fd)
        input_temp = Path(input_name)
        shutil.copyfile(candidate, input_temp)
        input_temp.chmod(0o444)
        if (identity(candidate) != candidate_identity or digest(candidate) != candidate_sha
                or digest(input_temp) != candidate_sha):
            fail("APP_INPUT_CHANGED_DURING_SNAPSHOT")
        candidate_for_use = input_temp
    except OSError as error:
        if input_temp is not None:
            input_temp.unlink(missing_ok=True)
        fail("APP_CANDIDATE_SNAPSHOT_FAILED", error)

    try:
        run_in_candidate(
            apptainer,
            candidate_for_use,
            r"""set -eu
app=/opt/ndnsf-app
test -d "$app/bin" -a -d "$app/lib" -a -d "$app/python"
export LD_LIBRARY_PATH="$app/lib:/opt/ndn-base/lib:/opt/onnxruntime/lib"
PYTHONNOUSERSITE=1 /opt/venv/bin/python - "$app" <<'PY'
import os
import stat
import sys

root = sys.argv[1]
if not stat.S_ISDIR(os.lstat(root).st_mode):
    raise SystemExit("APP_CANDIDATE_ROOT_NOT_DIRECTORY")
def onerror(error):
    raise SystemExit("APP_LAYOUT_WALK_FAILED:" + str(error))

for directory, subdirectories, filenames in os.walk(root, onerror=onerror, followlinks=False):
    for name in sorted(subdirectories + filenames):
        path = os.path.join(directory, name)
        mode = os.lstat(path).st_mode
        if stat.S_ISLNK(mode):
            raise SystemExit("APP_CANDIDATE_SYMLINK:" + path)
        if name in subdirectories:
            valid = stat.S_ISDIR(mode)
        else:
            valid = stat.S_ISREG(mode)
        if not valid:
            raise SystemExit("APP_CANDIDATE_SPECIAL_FILE:" + path)
PY
for name in di-native-provider di-native-fault-provider App_ServiceController; do
    test -x "$app/bin/$name"
done
for name in libndn-service-framework.so.0.1.0 libndnsf-distributed-inference.so \
            libndn-svs.so.0.1.0 libnac-abe.so libndnsd.so.0.1.0 \
            libopenabe.so librelic.so librelic_ec.so; do
    test -f "$app/lib/$name"
done
PYTHONNOUSERSITE=1 /opt/venv/bin/python - "$app" <<'PY'
import os
import stat
import subprocess
import sys

root = sys.argv[1]
def onerror(error):
    raise SystemExit("APP_ELF_WALK_FAILED:" + str(error))

app_libraries = {
    "libndn-service-framework.so.0.1.0", "libndnsf-distributed-inference.so",
    "libndn-svs.so.0.1.0", "libnac-abe.so", "libndnsd.so.0.1.0",
    "libopenabe.so", "librelic.so", "librelic_ec.so",
}
forbidden_roots = ("/opt/ndnsf-di/current/", "/opt/ndnsf-stage/", "/src/", "/usr/local/lib/")

def verify_elf_resolution(path, output):
    if "not found" in output:
        raise SystemExit("APP_ELF_DEPENDENCY_FAILED:" + path)
    for line in output.splitlines():
        fields = line.strip().split()
        if len(fields) < 3 or fields[1] != "=>":
            continue
        name, resolved = fields[0], fields[2]
        if name in app_libraries and not resolved.startswith(root + "/lib/"):
            raise SystemExit("APP_ELF_LIBRARY_ORIGIN_MISMATCH:" + name + ":" + resolved)
        if any(resolved.startswith(prefix) for prefix in forbidden_roots):
            raise SystemExit("APP_ELF_FORBIDDEN_RESOLVED_PATH:" + resolved)

for directory, subdirectories, filenames in os.walk(root, onerror=onerror, followlinks=False):
    subdirectories.sort()
    for filename in sorted(filenames):
        path = os.path.join(directory, filename)
        try:
            if not stat.S_ISREG(os.stat(path).st_mode):
                raise OSError("non-regular file")
            with open(path, "rb") as stream:
                if stream.read(4) != b"\x7fELF":
                    continue
        except OSError as error:
            raise SystemExit("APP_ELF_READ_FAILED:" + str(error))
        result = subprocess.run(["ldd", path], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode != 0:
            raise SystemExit("APP_ELF_DEPENDENCY_FAILED:" + path)
        verify_elf_resolution(path, result.stdout)
PY
LD_LIBRARY_PATH="$app/lib:/opt/ndn-base/lib:/opt/onnxruntime/lib" \
PYTHONPATH="$app/python" PYTHONNOUSERSITE=1 /opt/venv/bin/python \
    -c 'import ndnsf._ndnsf, ndnsf_distributed_inference, py_repoclient'
            """,
            apptainer_identity,
            apptainer_sha,
        )
        if (identity(base) != base_identity or digest(base) != base_sha
                or identity(candidate) != candidate_identity or digest(candidate) != candidate_sha
                or identity(record_path) != record_identity or digest(record_path) != record_sha
                or digest(input_temp) != candidate_sha):
            fail("APP_INPUT_CHANGED_DURING_VERIFICATION")
    except BaseException:
        input_temp.unlink(missing_ok=True)
        raise
    output.parent.mkdir(parents=True, exist_ok=True)
    temp = output.with_name(output.name + f".tmp.{os.getpid()}")
    temp_record = app_record_path.with_name(app_record_path.name + f".tmp.{os.getpid()}")
    if temp.exists() or temp_record.exists():
        input_temp.unlink(missing_ok=True)
        fail("APP_TEMP_OUTPUT_EXISTS", temp if temp.exists() else temp_record)
    try:
        ensure_publish_parent(output.parent, parent_fd, parent_identity)
        os.mkdir(temp.name, 0o700, dir_fd=parent_fd)
        ensure_publish_parent(output.parent, parent_fd, parent_identity)
        # Persist the intent before creating the second sibling.  A crash in
        # either gap is therefore recoverable by the next invocation.
        write_staging_token(temp, output, app_record_path, temp_record, None)
        ensure_publish_parent(output.parent, parent_fd, parent_identity)
        temp_record_fd = os.open(
            temp_record.name,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
            0o600,
            dir_fd=parent_fd,
        )
        temp_record_info = os.fstat(temp_record_fd)
        os.close(temp_record_fd)
        ensure_publish_parent(output.parent, parent_fd, parent_identity)
        write_staging_token(temp, output, app_record_path, temp_record,
                            [temp_record_info.st_dev, temp_record_info.st_ino])
        ensure_publish_parent(output.parent, parent_fd, parent_identity)
        write_publish_marker(marker, "prepared", output, app_record_path,
                             temp, temp_record, output.parent, parent_fd,
                             parent_identity)
    except BaseException:
        safe_remove_staging(temp, output.parent, parent_fd, parent_identity)
        safe_remove_staging(temp_record, output.parent, parent_fd, parent_identity)
        input_temp.unlink(missing_ok=True)
        raise
    try:
        extract(apptainer, candidate_for_use, temp, apptainer_identity, apptainer_sha)
        ensure_publish_parent(output.parent, parent_fd, parent_identity)
        reject_manifest_symlinks(temp / "manifest")
        candidate_record_target = temp / "manifest/candidate-build-record.json"
        if candidate_record_target.exists():
            candidate_record_target.unlink()
        shutil.copyfile(record_path, candidate_record_target)
        # The transaction token is recovery metadata, never application data.
        (temp / STAGING_TOKEN).unlink(missing_ok=True)
        verify_materialized(apptainer, candidate_for_use, temp,
                            apptainer_identity, apptainer_sha)
        verify_elf_runtime_paths(temp)
        temp_source_identity = identity(temp)
        rows = file_rows(temp)
        names = {row["path"] for row in rows}
        required = {
            *(f"lib/{name}" for name in APP_NATIVE_LIBRARIES),
            *(f"bin/{name}" for name in EXPECTED_BINARIES),
            "python/ndnsf_distributed_inference/__init__.py",
            "manifest/app-runtime.lock.json",
            "manifest/source-seal.json",
            "manifest/candidate-build-record.json",
        }
        missing = sorted(required - names)
        if missing:
            fail("APP_REQUIRED_ARTIFACT_MISSING", missing[0])
        entrypoints = [f"/opt/ndnsf-di/app/bin/{name}" for name in EXPECTED_BINARIES]
        body: dict[str, Any] = {
            "schemaVersion": "ndnsf-sif-app-v2",
            "status": "PASS",
            "buildBoundary": "container-runtime-in-sif-extracted",
            "candidateLayout": APP_LAYOUT,
            "baseSif": {"path": str(base), "sha256": base_sha},
            "candidateSif": {"path": str(candidate), "sha256": candidate_sha},
            "candidateBuildRecord": {
                "path": str(record_path),
                "sha256": digest(record_path),
                "recordDigest": build_record["recordDigest"],
            },
            "candidateVerification": "PASS",
            "apptainer": {
                "version": actual_version,
                "expectedVersion": args.expected_apptainer,
                "path": str(apptainer),
                "sha256": apptainer_sha,
            },
            "mounts": [
                {"source": name, "target": target}
                for name, target in (
                    ("lib", "/opt/ndnsf-di/app/lib"),
                    ("bin", "/opt/ndnsf-di/app/bin"),
                    ("python", "/opt/ndnsf-di/app/python"),
                    ("manifest", "/opt/ndnsf-di/app/manifest"),
                    ("replay", "/opt/ndnsf-di/app/replay"),
                )
            ],
            "files": rows,
            "entrypoints": entrypoints,
            "runtimeContract": {
                "cleanenv": True,
                "containall": True,
                "appFallback": "forbidden",
                "modelMount": "/models:ro",
                "artifactMount": "/artifacts:ro",
                "appNativeLibraries": list(APP_NATIVE_LIBRARIES),
                "baseNativeLibraries": list(BASE_NATIVE_LIBRARIES),
                "baseRuntime": {
                    "python": "/opt/venv/bin/python",
                    "ndnBaseLib": "/opt/ndn-base/lib",
                    "onnxRuntimeLib": "/opt/onnxruntime/lib",
                },
            },
            "tigerAction": "verify-pair-and-execute-only",
        }
        body["appDigest"] = record_digest(body)
        write_temp_record(temp_record, body, parent_fd,
                          (temp_record_info.st_dev, temp_record_info.st_ino))
        immutable(temp)
        validator = load_module("ndnsf_sif_app_validator", VALIDATOR_PATH)
        validator.validate(temp_record, temp, base, verify_base_hash=True)
        published_output = False
        ensure_publish_parent(output.parent, parent_fd, parent_identity)
        write_publish_marker(marker, "prepared", output, app_record_path,
                             temp, temp_record, output.parent, parent_fd,
                             parent_identity)
        replace_in_publish_parent(
            temp, output, output.parent, parent_fd, parent_identity,
            temp_source_identity,
        )
        published_output = True
        ensure_publish_parent(output.parent, parent_fd, parent_identity)
        try:
            write_publish_marker(marker, "app-published", output, app_record_path,
                                 temp, temp_record, output.parent, parent_fd,
                                 parent_identity)
        except BaseException:
            # The prepared marker still names the output inode.  Roll back
            # that exact inode so a failed state transition cannot leave an
            # output that recovery would later reject.
            try:
                ensure_publish_parent(output.parent, parent_fd, parent_identity)
                output_info = os.stat(output.name, dir_fd=parent_fd,
                                      follow_symlinks=False)
                if ([output_info.st_dev, output_info.st_ino]
                        == list(temp_source_identity)):
                    remove_tree_at(output.name, parent_fd)
            except OSError:
                pass
            raise
        try:
            replace_in_publish_parent(temp_record, app_record_path, output.parent,
                                      parent_fd, parent_identity,
                                      (temp_record_info.st_dev,
                                       temp_record_info.st_ino))
        except BaseException:
            if published_output:
                ensure_publish_parent(output.parent, parent_fd, parent_identity)
                try:
                    remove_tree_at(output.name, parent_fd)
                except FileNotFoundError:
                    pass
            raise
        write_publish_marker(marker, "pair-published", output, app_record_path,
                             temp, temp_record, output.parent, parent_fd,
                             parent_identity)
        try:
            ensure_publish_parent(output.parent, parent_fd, parent_identity)
            os.unlink(marker.name, dir_fd=parent_fd)
        except OSError:
            # The pair is valid; the next invocation will recover this marker.
            pass
        return {"status": "PASS", "app": str(output), "manifest": str(app_record_path),
                "appDigest": body["appDigest"], "baseSifSha256": base_sha,
                "candidateSifSha256": candidate_sha, "files": len(rows)}
    except BaseException:
        safe_remove_staging(temp, output.parent, parent_fd, parent_identity)
        safe_remove_staging(temp_record, output.parent, parent_fd, parent_identity)
        raise
    finally:
        input_temp.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-sif", required=True)
    parser.add_argument("--candidate-sif", required=True)
    parser.add_argument("--candidate-record", required=True)
    parser.add_argument("--output", required=True, help="new external APP directory")
    parser.add_argument("--record", required=True, help="new APP manifest JSON")
    parser.add_argument("--apptainer", default="/usr/local/bin/apptainer")
    parser.add_argument("--expected-apptainer", required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(publish(args), sort_keys=True, separators=(",", ":")))
    except BuildSifAppError as error:
        print(str(error), file=sys.stderr)
        return 4
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
