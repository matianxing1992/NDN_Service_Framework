#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_BIN="${PYTHON:-python3}"
JOBS="${NDNSF_BUILD_JOBS:-4}"
WAF_CONFIGURE_ARGS=()
QWEN_MININDN=0
RUN_WAF_CONFIGURE=auto
RUN_SYSTEM_INSTALL=1
USE_USER_FLAG=0
INSTALL_EDITABLE=0
INSTALL_DEPENDENCIES=auto
FORCE_DEPENDENCIES=0
NO_DEPENDENCIES_REQUESTED=0
DEPS_DIR="$ROOT/dependencies"
BUILD_DIR="${NDNSF_BUILD_DIR:-}"
REUSE_BUILD_DIR="${NDNSF_REUSE_BUILD_DIR:-0}"
INSTALL_SYSTEM_PACKAGES=1
INSTALL_TEST_PACKAGES=0
INSTALL_DOC_PACKAGES=0
INSTALL_MININDN_PACKAGES=0
INSTALL_YOLO_MININDN=0
INSTALL_NFD_NLSR_PACKAGES=0
CHECK_DEPENDENCIES=0
UNINSTALL_STACK=0
LEGACY_BUILD_DIR="${NDNSF_LEGACY_BUILD_DIR:-}"
PLAN_ONLY=0
CONFIGURE_ONLY=0
DEPS_ONLY=0
SOURCE_MODE=0
LOCK_FILE="$ROOT/packaging/host-dependencies.lock.json"
SOURCE_HELPER="$ROOT/scripts/stack_sources.py"
# Every host-side NDNSF dependency is installed into one canonical prefix.
# The source checkout below is only a build input; it must never become a
# runtime or pkg-config dependency path.
GLOBAL_DEPENDENCY_PREFIX="/usr/local"
OPENABE_PREFIX="$GLOBAL_DEPENDENCY_PREFIX"
GLOBAL_LIBRARY_DIR="$GLOBAL_DEPENDENCY_PREFIX/lib"
SYSTEM_PATH="/usr/bin:/bin:/usr/sbin:/sbin"
PKG_CONFIG_BIN="/usr/bin/pkg-config"
GLOBAL_PKG_CONFIG_PATH=""
# Host SDKs may have a versioned global prefix, but they must still be
# installed outside the repository and visible to every consumer.  ONNX
# Runtime is the only NDNSF dependency currently using such a versioned SDK;
# all NDN/NDNSF outputs remain in /usr/local.
GLOBAL_SDK_ROOTS=("/usr" "/usr/local" "/opt/onnxruntime" "/opt/onnxruntime-1.26.0" "/opt/onnx-1.17.0" "/opt/ndn-base")
GLOBAL_ONNX_PREFIX="/opt/onnx-1.17.0"
BOOST_INCLUDE_DIR=""
BOOST_LIBRARY_DIR=""
BOOST_VERSION_NUMBER=""
BOOST_VERSION_STRING=""
HOST_UBUNTU_VERSION=""
GLOBAL_IDENTITY_DIR="/usr/local/share/ndnsf"
GLOBAL_IDENTITY_FILE="$GLOBAL_IDENTITY_DIR/global-dependency-identity.json"
INSTALL_MANIFEST="$GLOBAL_IDENTITY_DIR/stack-install-manifest.json"

# Minimum installed package metadata accepted by this host workflow.  A
# missing/older package or a package outside /usr/local is rebuilt and
# installed globally before NDNSF is configured.  The checkout in --deps-dir
# is only a build input and never becomes a runtime/search path.
MIN_NDNCXX_VERSION="0.9.0"
MIN_NDNSD_VERSION="0.1.0"
MIN_NDNSVS_VERSION="0.1.0"
MIN_NACABE_VERSION="0.1"

NDNCXX_REPO_URL="${NDNCXX_REPO_URL:-https://github.com/matianxing1992/ndn-cxx.git}"
NDNSD_REPO_URL="${NDNSD_REPO_URL:-https://github.com/matianxing1992/NDNSD.git}"
NDNSVS_REPO_URL="${NDNSVS_REPO_URL:-https://github.com/matianxing1992/ndn-svs.git}"
NACABE_REPO_URL="${NACABE_REPO_URL:-https://github.com/matianxing1992/NAC-ABE.git}"
OPENABE_REPO_URL="${OPENABE_REPO_URL:-https://github.com/matianxing1992/openabe.git}"

# Mini-NDN v0.7.0's 2024-08 release map is the compatibility set used by the
# official installer. Keep full commit IDs here so this profile never follows
# moving upstream branches or disturbs the installed ndn-cxx/NFD.
MININDN_REPO_URL="https://github.com/named-data/mini-ndn.git"
MININDN_COMMIT="73add71f860979426aef63136bf78ed525862e5c"
MININDN_PSYNC_URL="https://github.com/named-data/PSync.git"
MININDN_PSYNC_COMMIT="b65d6db9f6300a39e006c43f7a9a51d796a562a5"
MININDN_NLSR_URL="https://github.com/named-data/NLSR.git"
MININDN_NLSR_COMMIT="bb59d40e3a71f1433beccae4eafdddd016f4ae91"
MININDN_TOOLS_URL="https://github.com/named-data/ndn-tools.git"
MININDN_TOOLS_COMMIT="435d201dc6d4a52c86808dae604f0b1b78fdb59c"
MININDN_TRAFFIC_URL="https://github.com/named-data/ndn-traffic-generator.git"
MININDN_TRAFFIC_COMMIT="9bec15ac63ae2ac225c4e514daa9fc1366889d62"
MININDN_INFOEDIT_URL="https://github.com/NDN-Routing/infoedit.git"
MININDN_INFOEDIT_COMMIT="cecfe583f5d2df974f5cbd50996f91b9d321be99"
YOLO_MODEL_URL="https://github.com/ultralytics/assets/releases/download/v8.4.0/yolo26n.pt"
YOLO_MODEL_SHA256="9b09cc8bf347f0fc8a5f7657480587f25db09b34bf33b0652110fb03a8ad4fef"
YOLO_ULTRALYTICS_VERSION="8.4.164"

usage() {
  cat <<'EOF'
Usage: ./install_ndnsf_stack.sh [options]

Build and install compatible pinned NDN dependencies, the NDNSF C++ stack,
Python bindings, and distributed-inference packages.

Options:
  --source                 Build pinned source for missing/incompatible dependencies.
  --lock-file PATH         Dependency URLs, refs, and exact commits.
  --plan, --dry-run        Offline plan; no fetch, install, or configure.
  --deps-only              Install/check dependencies, then stop.
  --configure-only         Install prerequisites and configure Waf; do not build.
  --no-install              Configure without installing OS packages.
  --install-dependencies   Resolve missing dependencies from pinned sources.
  --no-dependencies        Check dependencies without rebuilding them.
  --force-dependencies     Rebuild dependencies even when found.
  --deps-dir PATH          Source checkout root (default: ./dependencies).
  NDNSF_REUSE_BUILD_DIR=1  Reuse a build dir only when its checkout marker matches.
  --install-system-packages Install common APT build packages (default).
  --no-system-packages     Do not install OS packages.
  --with-system-tests-deps Install test/documentation OS packages.
  --with-minindn-deps      Install Mini-NDN experiment OS packages.
  --with-yolo-minindn      Install the pinned two-node Mini-NDN + YOLO CPU profile.
  --with-qwen-minindn      Check Qwen Mini-NDN prerequisites; do not download a model.
  --with-nfd-nlsr-deps     Install NFD/NLSR build OS packages.
  --check-dependencies     Verify the installed global dependency closure.
  --uninstall              Remove only installer-recorded files/packages/sources.
  --legacy-build-dir PATH  Validate/remove a pre-manifest build dir with --uninstall.
  --configure              Always run ./waf configure.
  --no-configure           Unsupported; configure is required for every install.
  --jobs N                 Build parallelism (default: NDNSF_BUILD_JOBS or 4).
  NDNSF_TMPDIR=PATH        Temporary files (default: BUILD_DIR/tmp).
  --with-examples          Pass --with-examples to Waf configure.
  --with-tests             Pass --with-tests to Waf configure.
  --no-system-install      Build without running ./waf install.
  --system-install         Run ./waf install (default).
  --user                   Unsupported; system Python is required.
  --no-user                Use system Python (default).
  --no-editable            Install wheels, not source-tree links (default).
  --python PATH            Python executable (default: python3 or $PYTHON).
  -h, --help               Show this help.

Notes:
  - Files, replaced-file backups, sources, and added APT packages are recorded in
    /usr/local/share/ndnsf/stack-install-manifest.json; uninstall restores backups
    and never guesses about reused or unrecorded paths.
  - Compatible global dependencies are probed and reused individually; missing
    ones use locked sources. Source trees must match their URL/commit and be clean.
  - --with-yolo-minindn installs Mini-NDN, python-ndn, CPU-only YOLO dependencies,
    and the hash-checked yolo26n.pt model; run ndnsf-yolo-minindn for inference.
  - Supports Ubuntu 20.04/26.04 x86-64. Boost must be a matching pair (1.71 on
    20.04; 1.71+ on 26.04). ONNX Runtime 1.26+, full-protobuf ONNX, and the Rust
    tokenizer archive must already exist in the declared global SDK locations.
  - OpenABE is installed before NAC-ABE when needed. Waf/Python builds use the
    installed global NDNSF closure, never a checkout or per-run runtime path.
  - Options after -- are forwarded to Waf configure. --no-system-install is
    build-only; legacy installs require validated --uninstall --legacy-build-dir.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --uninstall)
      UNINSTALL_STACK=1; shift ;;
    --legacy-build-dir)
      [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || { echo '--legacy-build-dir requires a path' >&2; exit 2; }
      LEGACY_BUILD_DIR="$2"; shift 2 ;;
    --source)
      SOURCE_MODE=1; INSTALL_DEPENDENCIES=auto; shift ;;
    --lock-file)
      [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || { echo '--lock-file requires a path' >&2; exit 2; }
      LOCK_FILE="$2"; shift 2 ;;
    --plan|--dry-run)
      PLAN_ONLY=1; shift ;;
    --deps-only)
      DEPS_ONLY=1; shift ;;
    --configure-only)
      CONFIGURE_ONLY=1; shift ;;
    --no-install)
      CONFIGURE_ONLY=1; INSTALL_SYSTEM_PACKAGES=0; shift ;;
    --)
      shift; WAF_CONFIGURE_ARGS+=("$@"); break ;;
    --install-dependencies)
      INSTALL_DEPENDENCIES=1
      shift
      ;;
    --no-dependencies)
      INSTALL_DEPENDENCIES=0
      NO_DEPENDENCIES_REQUESTED=1
      shift
      ;;
    --force-dependencies)
      INSTALL_DEPENDENCIES=1
      FORCE_DEPENDENCIES=1
      shift
      ;;
    --deps-dir)
      [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || { echo '--deps-dir requires a path' >&2; exit 2; }
      DEPS_DIR="$2"
      shift 2
      ;;
    --install-system-packages)
      INSTALL_SYSTEM_PACKAGES=1
      shift
      ;;
    --no-system-packages)
      INSTALL_SYSTEM_PACKAGES=0
      shift
      ;;
    --with-system-tests-deps)
      INSTALL_TEST_PACKAGES=1
      INSTALL_DOC_PACKAGES=1
      shift
      ;;
    --with-qwen-minindn)
      QWEN_MININDN=1
      INSTALL_TEST_PACKAGES=1
      shift
      ;;
    --with-minindn-deps)
      INSTALL_MININDN_PACKAGES=1
      shift
      ;;
    --with-yolo-minindn)
      INSTALL_YOLO_MININDN=1
      INSTALL_MININDN_PACKAGES=1
      INSTALL_NFD_NLSR_PACKAGES=1
      shift
      ;;
    --with-nfd-nlsr-deps)
      INSTALL_NFD_NLSR_PACKAGES=1
      shift
      ;;
    --check-dependencies)
      CHECK_DEPENDENCIES=1
      INSTALL_DEPENDENCIES=0
      shift
      ;;
    --configure)
      RUN_WAF_CONFIGURE=1
      shift
      ;;
    --no-configure)
      RUN_WAF_CONFIGURE=0
      shift
      ;;
    --with-examples|--with-tests)
      WAF_CONFIGURE_ARGS+=("$1")
      if [[ "$1" == "--with-tests" ]]; then
        INSTALL_TEST_PACKAGES=1
      fi
      if [[ "$RUN_WAF_CONFIGURE" == "auto" ]]; then
        RUN_WAF_CONFIGURE=1
      fi
      shift
      ;;
    --no-system-install)
      RUN_SYSTEM_INSTALL=0
      shift
      ;;
    --system-install)
      RUN_SYSTEM_INSTALL=1
      shift
      ;;
    --user)
      USE_USER_FLAG=1
      shift
      ;;
    --no-user)
      USE_USER_FLAG=0
      shift
      ;;
    --no-editable)
      INSTALL_EDITABLE=0
      shift
      ;;
    --python)
      [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || { echo '--python requires an executable' >&2; exit 2; }
      PYTHON_BIN="$2"
      shift 2
      ;;
    --jobs)
      [[ $# -ge 2 ]] || { echo '--jobs requires a positive integer' >&2; exit 2; }
      JOBS="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

# Reject unsupported options before apt, dependency installation, or configure.
if (( QWEN_MININDN )); then
  if (( CONFIGURE_ONLY || DEPS_ONLY || CHECK_DEPENDENCIES || ! RUN_SYSTEM_INSTALL )); then
    echo '--with-qwen-minindn requires a complete system installation' >&2
    exit 2
  fi
  WAF_CONFIGURE_ARGS+=(--with-examples --with-tests --install-experiment-fixtures)
fi
if (( INSTALL_YOLO_MININDN )); then
  if (( CONFIGURE_ONLY || DEPS_ONLY || CHECK_DEPENDENCIES || ! RUN_SYSTEM_INSTALL )); then
    echo '--with-yolo-minindn requires a complete system installation' >&2
    exit 2
  fi
  if (( QWEN_MININDN )); then
    echo '--with-yolo-minindn and --with-qwen-minindn are separate experiment profiles' >&2
    exit 2
  fi
fi
if (( CONFIGURE_ONLY && (SOURCE_MODE || DEPS_ONLY || CHECK_DEPENDENCIES || FORCE_DEPENDENCIES) )); then
  echo '--configure-only/--no-install cannot be combined with source/deps/check modes' >&2
  exit 2
fi
if (( CHECK_DEPENDENCIES && (SOURCE_MODE || DEPS_ONLY || FORCE_DEPENDENCIES) )); then
  echo '--check-dependencies cannot be combined with source/deps install modes' >&2
  exit 2
fi
if (( FORCE_DEPENDENCIES && NO_DEPENDENCIES_REQUESTED )); then
  echo '--force-dependencies cannot be combined with --no-dependencies' >&2
  exit 2
fi
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo '--jobs must be a positive integer' >&2; exit 2; }
[[ "$REUSE_BUILD_DIR" == 0 || "$REUSE_BUILD_DIR" == 1 ]] || {
  echo 'NDNSF_REUSE_BUILD_DIR must be 0 or 1' >&2
  exit 2
}
[[ "$USE_USER_FLAG" == "0" ]] || { echo '--user is incompatible with system-installed MiniNDN software' >&2; exit 2; }
if [[ "$RUN_WAF_CONFIGURE" == "0" ]]; then
  echo '--no-configure is not allowed for the global-closure installer' >&2
  exit 2
fi
if (( UNINSTALL_STACK )); then
  if (( PLAN_ONLY || CONFIGURE_ONLY || DEPS_ONLY || CHECK_DEPENDENCIES || FORCE_DEPENDENCIES ||
        SOURCE_MODE || QWEN_MININDN || INSTALL_YOLO_MININDN )) || ((${#WAF_CONFIGURE_ARGS[@]})); then
    echo '--uninstall cannot be combined with install/configure options' >&2
    exit 2
  fi
elif [[ -n "$LEGACY_BUILD_DIR" ]]; then
  echo '--legacy-build-dir is only valid with --uninstall' >&2
  exit 2
fi
# Resolve user-relative paths before changing to the repository root.
DEPS_DIR="$(realpath -m -- "$DEPS_DIR")"
LOCK_FILE="$(realpath -m -- "$LOCK_FILE")"
PYTHON_BIN="$(command -v -- "$PYTHON_BIN")" || { echo 'Python executable not found' >&2; exit 2; }
PYTHON_BIN="$(realpath -s -- "$PYTHON_BIN")"

run() {
  echo "+ $*"
  "$@"
}

sudo_run() {
  if [[ "${EUID}" -eq 0 ]]; then
    run "$@"
  else
    run sudo -n "$@"
  fi
}

manifest_source() {
  cat <<'NDNSF_INSTALL_MANIFEST_PY'
#!/usr/bin/env python3
"""Track and conservatively remove files installed by install_ndnsf_stack.sh.

Only explicitly recorded files are eligible for removal. Existing file
contents are backed up before replacement and restored by the uninstall action.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from urllib.parse import unquote, urlparse


SCHEMA = 1
MANAGED_PREFIXES = (Path("/usr/local"), Path("/opt/onnxruntime-1.26.0"), Path("/opt/onnx-1.17.0"))


def file_state(path: Path) -> dict[str, object] | None:
    try:
        info = path.lstat()
    except FileNotFoundError:
        return None
    if stat.S_ISLNK(info.st_mode):
        return {"kind": "symlink", "target": os.readlink(path), "mode": stat.S_IMODE(info.st_mode)}
    if not stat.S_ISREG(info.st_mode):
        raise ValueError(f"refusing non-file install target: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return {"kind": "file", "sha256": digest.hexdigest(), "mode": stat.S_IMODE(info.st_mode)}


def is_relative_to(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def read_manifest(path: Path) -> dict[str, object]:
    if not path.exists():
        return {"schema": SCHEMA, "files": {}, "sources": [], "aptPackages": []}
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != SCHEMA or not isinstance(data.get("files"), dict) or \
            not isinstance(data.get("sources"), list) or \
            not isinstance(data.get("aptPackages", []), list):
        raise ValueError(f"unsupported or corrupt install manifest: {path}")
    data.setdefault("aptPackages", [])
    return data


def write_manifest(path: Path, data: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(data, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def backup_path(manifest: Path, target: Path) -> Path:
    key = hashlib.sha256(str(target).encode()).hexdigest()
    return manifest.parent / f"{manifest.stem}.backups" / key


def copy_backup(manifest: Path, target: Path) -> tuple[str | None, dict[str, object] | None]:
    before = file_state(target)
    if before is None:
        return None, None
    backup = backup_path(manifest, target)
    backup.parent.mkdir(parents=True, exist_ok=True)
    if backup.exists() or backup.is_symlink():
        if file_state(backup) != before:
            raise ValueError(f"backup already exists with different contents: {backup}")
    elif before["kind"] == "symlink":
        os.symlink(str(before["target"]), backup)
    else:
        shutil.copy2(target, backup, follow_symlinks=False)
    return str(backup), before


def check_prefix(prefix: Path) -> Path:
    resolved = prefix.resolve()
    if resolved not in MANAGED_PREFIXES:
        raise ValueError(f"install prefix is outside the managed allowlist: {prefix}")
    return resolved


def check_target(target: Path, prefix: Path) -> Path:
    target = prefix / target if not target.is_absolute() else target
    lexical = Path(os.path.abspath(target))
    if lexical != prefix and prefix not in lexical.parents:
        raise ValueError(f"install target escapes its managed prefix: {target}")
    resolved_parent = lexical.parent.resolve()
    if resolved_parent != prefix and prefix not in resolved_parent.parents:
        raise ValueError(f"install target parent escapes its managed prefix: {target}")
    return lexical


def staged_files(root: Path):
    root = root.resolve(strict=True)
    for directory, dirs, files in os.walk(root, followlinks=False):
        dirs[:] = sorted(d for d in dirs if not (Path(directory) / d).is_symlink())
        for name in sorted(files):
            path = Path(directory) / name
            if path.is_symlink() or path.is_file():
                yield path, path.relative_to(root)
            else:
                raise ValueError(f"refusing special staged install entry: {path}")


def install_tree(args: argparse.Namespace) -> None:
    manifest_path = Path(args.manifest)
    prefix = check_prefix(Path(args.prefix))
    stage = Path(args.stage)
    data = read_manifest(manifest_path)
    entries: dict[str, dict[str, object]] = data["files"]  # type: ignore[assignment]
    plan = []
    for source, relative in staged_files(stage):
        target = check_target(prefix / relative, prefix)
        if target == manifest_path.absolute() or manifest_path.absolute() in target.parents:
            raise ValueError(f"installer payload may not overwrite its ownership ledger: {target}")
        current = file_state(target)
        installed = file_state(source)
        assert installed is not None
        key = str(target)
        old = entries.get(key)
        if old is not None and current not in (old.get("installed"), old.get("backupState")):
            raise ValueError(f"managed install target was modified; refusing overwrite: {target}")
        backup = old.get("backup") if old is not None else None
        before = old.get("backupState") if old is not None else None
        owners = sorted(set((old or {}).get("owners", [])) | {args.component})
        entry = {"installed": installed, "backup": backup, "backupState": before,
                 "owners": owners}
        plan.append((source, target, key, entry, old, current))

    for offset in range(0, len(plan), 64):
        batch = plan[offset:offset + 64]
        for src, dst, key, entry, old, current in batch:
            dst.parent.mkdir(parents=True, exist_ok=True)
            if old is None:
                if file_state(dst) != current:
                    raise ValueError(f"install target changed during staging: {dst}")
                entry["backup"], entry["backupState"] = copy_backup(manifest_path, dst)
            data["files"][key] = entry
        write_manifest(manifest_path, data)
        for src, dst, _, entry, _, _ in batch:
            if file_state(dst) == entry["installed"]:
                continue
            temporary = dst.parent / f".{dst.name}.ndnsf-install-{os.getpid()}"
            try:
                if src.is_symlink():
                    os.symlink(os.readlink(src), temporary)
                else:
                    shutil.copy2(src, temporary)
                os.replace(temporary, dst)
            finally:
                if temporary.exists() or temporary.is_symlink():
                    temporary.unlink()


def snapshot_python(args: argparse.Namespace) -> None:
    manifest_path = Path(args.manifest)
    data = read_manifest(manifest_path)
    entries: dict[str, dict[str, object]] = data["files"]  # type: ignore[assignment]
    for package in args.packages:
        try:
            distribution = importlib.metadata.distribution(package)
        except importlib.metadata.PackageNotFoundError:
            continue
        for item in distribution.files or ():
            target = Path(distribution.locate_file(item)).absolute()
            if not target.is_file() and not target.is_symlink():
                continue
            key = str(target)
            old = entries.get(key)
            if old is not None:
                current = file_state(target)
                if old.get("installed") is not None and current != old.get("installed"):
                    raise ValueError(f"managed Python file was modified: {target}")
                if old.get("installed") is None and current != old.get("backupState"):
                    raise ValueError(f"Python file changed while install ownership was pending: {target}")
                old["owners"] = sorted(set(old.get("owners", [])) | {f"python:{package}"})
                entries[key] = old
                continue
            ensure_python_target(target)
            backup, before = copy_backup(manifest_path, target)
            entries[key] = {"installed": None, "backup": backup, "backupState": before,
                            "owners": [f"python:{package}"]}
            write_manifest(manifest_path, data)


def record_python(args: argparse.Namespace) -> None:
    manifest_path = Path(args.manifest)
    data = read_manifest(manifest_path)
    entries: dict[str, dict[str, object]] = data["files"]  # type: ignore[assignment]
    for package in args.packages:
        try:
            distribution = importlib.metadata.distribution(package)
        except importlib.metadata.PackageNotFoundError as error:
            raise ValueError(f"expected installed Python distribution is missing: {package}") from error
        for item in distribution.files or ():
            target = Path(distribution.locate_file(item)).absolute()
            ensure_python_target(target)
            current = file_state(target)
            if current is None:
                continue
            key = str(target)
            old = entries.get(key)
            if old is None:
                backup, before = None, None
                old = {"backup": backup, "backupState": before, "owners": []}
            elif old.get("installed") is not None and current != old.get("installed") and \
                    not args.after_reinstall:
                raise ValueError(f"managed Python file was modified: {target}")
            old["installed"] = current
            old["owners"] = sorted(set(old.get("owners", [])) | {f"python:{package}"})
            entries[key] = old
        write_manifest(manifest_path, data)


def ensure_python_target(target: Path) -> None:
    resolved = target.resolve(strict=False)
    if not any(is_relative_to(resolved, root) for root in
               (Path("/usr/local/lib"), Path("/usr/local/bin"))):
        raise ValueError(f"Python package file is outside /usr/local/lib or /usr/local/bin: {target}")


def add_source(args: argparse.Namespace) -> None:
    raw_source = Path(args.path).absolute()
    if raw_source.is_symlink():
        raise ValueError(f"refusing symlink source tree: {raw_source}")
    source = raw_source.resolve(strict=True)
    base = Path(args.base).resolve(strict=True)
    if source.parent != base or not (source / ".git").is_dir():
        raise ValueError(f"not a standalone source checkout directly under its recorded base: {source}")
    item = {"path": str(source), "base": str(base), "leaf": source.name, "name": args.name,
            "url": args.url, "commit": args.commit,
            "baseCreated": bool(getattr(args, "base_created", False))}
    data = read_manifest(Path(args.manifest))
    sources: list[dict[str, str]] = data["sources"]  # type: ignore[assignment]
    if item not in sources:
        sources.append(item)
        write_manifest(Path(args.manifest), data)


def list_manifest(args: argparse.Namespace) -> None:
    path = Path(args.manifest)
    if not path.is_file():
        raise ValueError(f"no installer ownership manifest found: {path}; legacy installs are not safe to guess")
    data = read_manifest(path)
    entries: dict[str, dict[str, object]] = data["files"]  # type: ignore[assignment]
    owners = sorted({owner for entry in entries.values() for owner in entry.get("owners", [])})
    print(f"Tracked files: {len(entries)}")
    print(f"Source checkouts: {len(data['sources'])}")
    for owner in owners:
        count = sum(owner in entry.get("owners", []) for entry in entries.values())
        print(f"  {owner}: {count} files")


def uninstall(args: argparse.Namespace) -> None:
    path = Path(args.manifest)
    if not path.is_file():
        raise ValueError(f"no installer ownership manifest found: {path}")
    data = read_manifest(path)
    entries: dict[str, dict[str, object]] = data["files"]  # type: ignore[assignment]
    actions = []
    for raw_target, entry in entries.items():
        target = checked_manifest_target(Path(raw_target), path)
        current = file_state(target)
        backup = Path(str(entry["backup"])) if entry.get("backup") else None
        installed = entry.get("installed")
        before = entry.get("backupState")
        if current is None:
            actions.append((target, entry, "restore-missing" if backup is not None else "missing"))
        elif installed is not None and current == installed:
            actions.append((target, entry, "remove-or-restore"))
        elif before is not None and current == before:
            actions.append((target, entry, "already-restored"))
        else:
            raise ValueError(f"installed file changed since install; refusing partial uninstall: {target}")
        if backup is not None and (not backup.exists() and not backup.is_symlink()):
            raise ValueError(f"required pre-install backup is missing: {backup}")
        if backup is not None and not is_relative_to(
                backup.absolute(), (path.parent / f"{path.stem}.backups").absolute()):
            raise ValueError(f"backup path escapes the manifest backup directory: {backup}")

    for item in data["sources"]:
        source = Path(item["path"])
        base = Path(item["base"]).resolve()
        if source.absolute().parent.resolve() != base or source.is_symlink():
            raise ValueError(f"managed source path escapes its recorded base: {source}")
        if not source.exists():
            continue
        if source.is_symlink() or source.name != item.get("leaf", f"{item['name']}-{item['commit']}"):
            raise ValueError(f"managed source path changed; refusing removal: {source}")
        remote = subprocess.check_output(
            ["git", "-C", str(source), "remote", "get-url", "origin"], text=True).strip()
        commit = subprocess.check_output(
            ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
        dirty = subprocess.check_output(
            ["git", "-C", str(source), "status", "--porcelain", "--untracked-files=all"], text=True)
        if remote != item["url"] or commit != item["commit"] or dirty:
            raise ValueError(f"source checkout changed or is dirty; refusing removal: {source}")

    for package in data.get("aptPackages", []):
        if not isinstance(package, str) or not re.fullmatch(r"[a-z0-9][a-z0-9+.-]*(?::[a-z0-9]+)?", package):
            raise ValueError(f"invalid APT package name in ownership manifest: {package!r}")
    installed_packages = []
    for package in sorted(set(data.get("aptPackages", []))):
        query = subprocess.run(["dpkg-query", "-W", "-f=${Status}", package],
                               text=True, capture_output=True)
        if query.returncode == 0 and query.stdout.strip() == "install ok installed":
            installed_packages.append(package)
    if installed_packages:
        simulation = subprocess.run(
            ["apt-get", "-s", "remove", "--purge", "--", *installed_packages],
            text=True, capture_output=True)
        if simulation.returncode:
            raise ValueError("APT removal preflight failed: " + simulation.stderr.strip())
        removals = {line.split()[1] for line in simulation.stdout.splitlines()
                    if line.startswith("Remv ") and len(line.split()) >= 2}
        unexpected = removals - set(installed_packages)
        if unexpected:
            raise ValueError("APT would remove packages not owned by this installer: " +
                             ", ".join(sorted(unexpected)))

    for target, entry, action in actions:
        backup = Path(str(entry["backup"])) if entry.get("backup") else None
        current = file_state(target)
        before = entry.get("backupState")
        if action in {"already-restored", "missing"}:
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        if backup is None:
            target.unlink()
        elif before and before["kind"] == "symlink":
            temporary = target.parent / f".{target.name}.ndnsf-restore-{os.getpid()}"
            os.symlink(str(before["target"]), temporary)
            os.replace(temporary, target)
        else:
            shutil.copy2(backup, target)
    for item in data["sources"]:
        source = Path(item["path"])
        if source.exists():
            shutil.rmtree(source)
    for item in data["sources"]:
        if item.get("baseCreated"):
            base = Path(item["base"])
            if base.is_dir() and not base.is_symlink():
                try:
                    base.rmdir()
                except OSError:
                    pass
    if installed_packages:
        subprocess.run(["apt-get", "remove", "--purge", "-y", "--", *installed_packages], check=True)
    for raw_target in entries:
        target = checked_manifest_target(Path(raw_target), path)
        for parent in target.parents:
            if parent in (*MANAGED_PREFIXES, Path("/")):
                break
            try:
                parent.rmdir()
            except OSError:
                break
    path.unlink(missing_ok=True)
    backup_root = path.parent / f"{path.stem}.backups"
    if backup_root.exists():
        shutil.rmtree(backup_root)
    print("Uninstalled all unchanged files and source checkouts recorded by this installer.")
    print("Reused APT/system packages were not removed; only installer-added APT packages were considered.")


def checked_manifest_target(target: Path, manifest: Path) -> Path:
    absolute = Path(os.path.abspath(target))
    if not any(absolute == root or root in absolute.parents for root in MANAGED_PREFIXES):
        raise ValueError(f"manifest target is outside managed install prefixes: {target}")
    if absolute == manifest.absolute() or manifest.absolute() in absolute.parents:
        raise ValueError(f"manifest may not list itself or its backup data as an install target: {target}")
    parent = absolute.parent.resolve()
    if not any(parent == root or root in parent.parents for root in MANAGED_PREFIXES):
        raise ValueError(f"manifest target parent escapes managed install prefixes: {target}")
    return absolute


def record_apt(args: argparse.Namespace) -> None:
    path = Path(args.manifest)
    data = read_manifest(path)
    for package in args.packages:
        if not re.fullmatch(r"[a-z0-9][a-z0-9+.-]*(?::[a-z0-9]+)?", package):
            raise ValueError(f"invalid APT package name: {package!r}")
    data["aptPackages"] = sorted(set(data.get("aptPackages", [])) | set(args.packages))
    write_manifest(path, data)


def verify_legacy_python(args: argparse.Namespace) -> None:
    raw_build_dir = Path(args.build_dir).absolute()
    if raw_build_dir.is_symlink():
        raise ValueError(f"legacy build directory may not be a symlink: {raw_build_dir}")
    build_dir = raw_build_dir.resolve(strict=True)
    if not build_dir.is_dir():
        raise ValueError(f"legacy build directory is not a directory: {build_dir}")
    expected_roots = {(build_dir / "tmp").resolve(),
                      (build_dir.parent / "tmp").resolve(), Path("/tmp").resolve()}
    observed_wheel_stage = None
    for package in args.packages:
        try:
            distribution = importlib.metadata.distribution(package)
        except importlib.metadata.PackageNotFoundError as error:
            raise ValueError(f"legacy installer package is missing: {package}") from error
        text = distribution.read_text("direct_url.json")
        if not text:
            raise ValueError(f"cannot prove legacy installer ownership for Python distribution: {package}")
        url = json.loads(text).get("url", "")
        parsed = urlparse(url)
        wheel = Path(unquote(parsed.path)) if parsed.scheme == "file" else Path()
        if parsed.scheme != "file" or not any(is_relative_to(wheel, root) for root in expected_roots) or \
                not wheel.name.endswith(".whl") or not wheel.parent.name.startswith("ndnsf-wheels."):
            raise ValueError(f"Python distribution is not from this build's installer wheel stage: {package}: {url}")
        if observed_wheel_stage is None:
            observed_wheel_stage = wheel.parent
        elif wheel.parent != observed_wheel_stage:
            raise ValueError(f"legacy Python distributions came from multiple wheel stages: {package}")
        for item in distribution.files or ():
            target = Path(distribution.locate_file(item)).absolute()
            ensure_python_target(target)
            if target.is_file() and item.hash is not None:
                digest = hashlib.new(item.hash.mode)
                with target.open("rb") as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b""):
                        digest.update(block)
                actual_hash = base64.urlsafe_b64encode(digest.digest()).decode().rstrip("=")
                if actual_hash != item.hash.value:
                    raise ValueError(f"legacy Python file differs from its wheel RECORD: {target}")
    print("Legacy Python package origins verified against the supplied installer build directory.")


def build_marker(args: argparse.Namespace) -> None:
    raw_build = Path(args.build_dir).absolute()
    if raw_build.is_symlink():
        raise ValueError(f"build directory may not be a symlink: {raw_build}")
    build = raw_build.resolve(strict=True)
    root = Path(args.root).resolve(strict=True)
    if not build.is_dir() or build == root or root in build.parents:
        raise ValueError(f"build directory must be outside the source checkout: {build}")
    marker = build / ".ndnsf-stack-build.json"
    expected = {"schema": SCHEMA, "root": str(root), "buildDir": str(build)}
    if args.verify:
        if not marker.is_file() or json.loads(marker.read_text(encoding="utf-8")) != expected:
            raise ValueError(f"build directory is not recorded as owned by this checkout: {build}")
        if not (build / "c4che/_cache.py").is_file() or not list(build.glob(".wafpickle-*")):
            raise ValueError(f"recorded Waf build directory is no longer configured: {build}")
        print(f"Verified reusable NDNSF build directory: {build}")
        return
    if marker.exists():
        if json.loads(marker.read_text(encoding="utf-8")) != expected:
            raise ValueError(f"build ownership marker belongs to another checkout: {marker}")
        return
    write_manifest(marker, expected)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    install = sub.add_parser("install-tree")
    install.add_argument("--manifest", required=True)
    install.add_argument("--stage", required=True)
    install.add_argument("--prefix", required=True)
    install.add_argument("--component", required=True)
    install.set_defaults(function=install_tree)
    snapshot = sub.add_parser("snapshot-python")
    snapshot.add_argument("--manifest", required=True)
    snapshot.add_argument("--python", required=True)
    snapshot.add_argument("--packages", nargs="+", required=True)
    snapshot.set_defaults(function=snapshot_python)
    record = sub.add_parser("record-python")
    record.add_argument("--manifest", required=True)
    record.add_argument("--python", required=True)
    record.add_argument("--packages", nargs="+", required=True)
    record.add_argument("--after-reinstall", action="store_true",
                        help="record files after the installer has verified the pre-install snapshot and reinstalled wheels")
    record.set_defaults(function=record_python)
    source = sub.add_parser("record-source")
    source.add_argument("--manifest", required=True)
    source.add_argument("--path", required=True)
    source.add_argument("--name", required=True)
    source.add_argument("--url", required=True)
    source.add_argument("--commit", required=True)
    source.add_argument("--base", required=True)
    source.add_argument("--base-created", action="store_true")
    source.set_defaults(function=add_source)
    listing = sub.add_parser("list")
    listing.add_argument("--manifest", required=True)
    listing.set_defaults(function=list_manifest)
    remove = sub.add_parser("uninstall")
    remove.add_argument("--manifest", required=True)
    remove.set_defaults(function=uninstall)
    apt = sub.add_parser("record-apt")
    apt.add_argument("--manifest", required=True)
    apt.add_argument("--packages", nargs="+", required=True)
    apt.set_defaults(function=record_apt)
    legacy = sub.add_parser("verify-legacy-python")
    legacy.add_argument("--build-dir", required=True)
    legacy.add_argument("--packages", nargs="+", required=True)
    legacy.set_defaults(function=verify_legacy_python)
    build = sub.add_parser("build-marker")
    build.add_argument("--build-dir", required=True)
    build.add_argument("--root", required=True)
    build.add_argument("--verify", action="store_true")
    build.set_defaults(function=build_marker)
    args = parser.parse_args()
    try:
        args.function(args)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"install-manifest: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
NDNSF_INSTALL_MANIFEST_PY
}

manifest_tool() {
  local privileged=0
  local -a consumer pipeline_status=()
  if [[ "${1:-}" == --privileged ]]; then
    privileged=1
    shift
  fi
  if (( privileged )); then
    consumer=(sudo_run "$PYTHON_BIN" - "$@")
  else
    consumer=("$PYTHON_BIN" - "$@")
  fi
  manifest_source | "${consumer[@]}" || pipeline_status=("${PIPESTATUS[@]}")
  if ((${#pipeline_status[@]})); then
    (( pipeline_status[1] == 0 )) || return "${pipeline_status[1]}"
    (( pipeline_status[0] == 0 || pipeline_status[0] == 141 )) || \
      return "${pipeline_status[0]}"
  fi
}

install_manifest_tree() {
  local stage="$1" prefix="$2" component="$3"
  [[ -d "$stage" ]] || { echo "Install staging tree is missing: $stage" >&2; return 1; }
  manifest_tool --privileged install-tree \
    --manifest "$INSTALL_MANIFEST" --stage "$stage" --prefix "$prefix" \
    --component "$component"
}

record_source_checkout() {
  local name="$1" url="$2" commit="$3" path="$4" base="$5" base_created="${6:-0}"
  local -a args=(--manifest "$INSTALL_MANIFEST" --path "$path" --base "$base"
    --name "$name" --url "$url" --commit "$commit")
  [[ "$base_created" == 1 ]] && args+=(--base-created)
  manifest_tool --privileged record-source "${args[@]}"
}

uninstall_stack() {
  if [[ -f "$INSTALL_MANIFEST" ]]; then
    echo "==> Uninstalling the recorded NDNSF stack from $INSTALL_MANIFEST"
    manifest_tool --privileged uninstall --manifest "$INSTALL_MANIFEST"
    sudo_run /sbin/ldconfig
    return
  fi
  if [[ -z "$LEGACY_BUILD_DIR" ]]; then
    echo "No ownership manifest exists at $INSTALL_MANIFEST." >&2
    echo "For a pre-manifest install, rerun with --uninstall --legacy-build-dir PATH after verifying the exact old build directory." >&2
    return 2
  fi

  local legacy_root
  [[ ! -L "$LEGACY_BUILD_DIR" ]] || {
    echo "Legacy build path may not be a symlink: $LEGACY_BUILD_DIR" >&2
    return 2
  }
  legacy_root="$(realpath -e -- "$LEGACY_BUILD_DIR")"
  case "$legacy_root" in
    "$ROOT"|"$ROOT"/*)
      echo "Legacy build directory must be outside the source checkout: $legacy_root" >&2
      return 2
      ;;
  esac
  [[ -f "$legacy_root/c4che/_cache.py" ]] && compgen -G "$legacy_root/.wafpickle-*" >/dev/null || {
    echo "Not a configured NDNSF build directory: $legacy_root" >&2
    return 2
  }
  local -a legacy_python_packages=(
    ndnsf py-repoclient ndnsf-distributed-inference ndnsf-di-core ndnsf-di-sdk
    ndnsf-di-planner ndnsf-di-app ndnsf-di-ops
  )
  manifest_tool verify-legacy-python \
    --build-dir "$legacy_root" --packages "${legacy_python_packages[@]}"
  [[ -f "$legacy_root/libndn-service-framework.so" && \
     -f "$legacy_root/libndnsf-distributed-inference.so" ]] || {
    echo "Legacy NDNSF build outputs are missing; refusing Waf uninstall" >&2
    return 1
  }
  if [[ -f "$GLOBAL_LIBRARY_DIR/libndn-service-framework.so.0.1.0" && \
        -f "$GLOBAL_LIBRARY_DIR/libndnsf-distributed-inference.so" ]]; then
    cmp -s "$legacy_root/libndn-service-framework.so" \
      "$GLOBAL_LIBRARY_DIR/libndn-service-framework.so.0.1.0" || {
      echo "Installed NDNSF Core library differs from the supplied legacy build; refusing cleanup" >&2
      return 1
    }
    cmp -s "$legacy_root/libndnsf-distributed-inference.so" \
      "$GLOBAL_LIBRARY_DIR/libndnsf-distributed-inference.so" || {
      echo "Installed NDNSF DI library differs from the supplied legacy build; refusing cleanup" >&2
      return 1
    }
    echo "==> Removing only the legacy build's Waf install targets"
    refresh_global_pkg_config_path
    sudo_run env -u PKG_CONFIG_LIBDIR -u PYTHONPATH -u PYTHONHOME \
      PATH="$SYSTEM_PATH" PKG_CONFIG_PATH="$GLOBAL_PKG_CONFIG_PATH" \
      NDNSF_SKIP_DEV_PIP_INSTALL=1 \
      "$PYTHON_BIN" ./waf --out="$legacy_root" --onnx-prefix="$GLOBAL_ONNX_PREFIX" uninstall
  elif [[ ! -e "$GLOBAL_LIBRARY_DIR/libndn-service-framework.so.0.1.0" && \
          ! -e "$GLOBAL_LIBRARY_DIR/libndnsf-distributed-inference.so" ]]; then
    echo "==> Legacy Waf targets are already absent; continuing the interrupted uninstall"
  else
    echo "Only part of the legacy Waf libraries remains; refusing ambiguous cleanup" >&2
    return 1
  fi
  echo "==> Removing the eight Python distributions proven to come from that build's wheel stage"
  sudo_run env PYTHONNOUSERSITE=1 PIP_BREAK_SYSTEM_PACKAGES=1 \
    "$PYTHON_BIN" -m pip --isolated uninstall --break-system-packages -y \
    "${legacy_python_packages[@]}"
  sudo_run /sbin/ldconfig
  echo "==> Legacy NDNSF install removed. Reused NDN/ONNX/system dependencies were left untouched."
}

stage_python_wheel_sources() {
  local stage_root="$1"
  shift
  local repository_copy="$stage_root/repository"
  local package relative staged
  local repository_snapshot_needed=0
  local package_index=0
  local -a staged_packages=()

  for package in "$@"; do
    case "$package" in
      "$ROOT"/*) repository_snapshot_needed=1 ;;
    esac
  done
  if (( repository_snapshot_needed )); then
    mkdir -p -- "$repository_copy"
    # Setuptools' PEP 517 wheel builds write build/lib into the source tree.
    # Snapshot the working tree outside the checkout so local edits are kept,
    # while generated build/cache directories and Git metadata stay out of the
    # wheel inputs.
    rsync -a \
      --exclude='/.git/' --exclude='/**/.git/' \
      --exclude='/**/build/' --exclude='/**/__pycache__/' \
      --exclude='/**/*.egg-info/' \
      "$ROOT/" "$repository_copy/"
  fi

  for package in "$@"; do
    case "$package" in
      "$ROOT"/*)
        relative="${package#"$ROOT"/}"
        staged="$repository_copy/$relative"
        ;;
      *)
        staged="$stage_root/external/$package_index/$(basename -- "$package")"
        mkdir -p -- "$(dirname -- "$staged")"
        rsync -a \
          --exclude='build/' --exclude='__pycache__/' \
          --exclude='*.egg-info/' \
          "$package/" "$staged/"
        ;;
    esac
    [[ -f "$staged/pyproject.toml" || -f "$staged/setup.py" ]] || {
      echo "Python wheel source is missing its build definition: $package" >&2
      return 2
    }
    staged_packages+=("$staged")
    package_index=$((package_index + 1))
  done

  PYTHON_WHEEL_INPUTS=("${staged_packages[@]}")
}

pip_install() (
  local wheel_dir
  local -a python_distributions=(
    ndnsf py-repoclient ndnsf-distributed-inference ndnsf-di-core ndnsf-di-sdk
    ndnsf-di-planner ndnsf-di-app ndnsf-di-ops
  )
  wheel_dir="$(mktemp -d -t ndnsf-wheels.XXXXXXXX)"
  trap 'rm -rf -- "$wheel_dir"' EXIT
  stage_python_wheel_sources "$wheel_dir/source-tree" "$@"
  # Compile bindings as the caller, then install wheels into system Python.
  run env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
    -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH \
    -u CPLUS_INCLUDE_PATH -u LDSHARED -u PKG_CONFIG_LIBDIR \
    -u PYTHONPATH -u PYTHONHOME \
    PATH="$SYSTEM_PATH" PKG_CONFIG_PATH="$GLOBAL_PKG_CONFIG_PATH" \
    CC=/usr/bin/gcc CXX=/usr/bin/g++ PYTHONNOUSERSITE=1 \
    "$PYTHON_BIN" -m pip --isolated wheel --wheel-dir "$wheel_dir" \
    "${PYTHON_WHEEL_INPUTS[@]}"
  manifest_tool --privileged snapshot-python \
    --manifest "$INSTALL_MANIFEST" --python "$PYTHON_BIN" \
    --packages "${python_distributions[@]}"
  sudo_run env -u PYTHONPATH -u PYTHONHOME PYTHONNOUSERSITE=1 \
    PIP_BREAK_SYSTEM_PACKAGES=1 \
    "$PYTHON_BIN" -m pip --isolated install --break-system-packages \
    --no-index --no-deps \
    --force-reinstall "$wheel_dir"/*.whl
  manifest_tool --privileged record-python \
    --manifest "$INSTALL_MANIFEST" --python "$PYTHON_BIN" --after-reinstall \
    --packages "${python_distributions[@]}"
)

require_host_profile() {
  local ID VERSION_ID
  . /etc/os-release
  if [[ "$ID" != ubuntu || ( "$VERSION_ID" != 20.04 && "$VERSION_ID" != 26.04 ) || "$(uname -m)" != x86_64 ]]; then
    echo 'Supported host profiles: Ubuntu 20.04 or 26.04 x86_64; no system changes made.' >&2
    exit 2
  fi
  if [[ "$(readlink -f "$PYTHON_BIN")" != "$(readlink -f /usr/bin/python3)" ]]; then
    echo 'Use /usr/bin/python3; venv/custom Python is not the system installation target.' >&2
    exit 2
  fi
  # Resolve symlink-based venvs to the actual system invocation path as well.
  PYTHON_BIN=/usr/bin/python3
  HOST_UBUNTU_VERSION="$VERSION_ID"
  select_global_boost || true
}

check_waf_inventory() {
  refresh_global_pkg_config_path
  env -u PKG_CONFIG_LIBDIR -u NDNSF_TOKENIZER_BRIDGE_ARCHIVE \
    PKG_CONFIG_PATH="$GLOBAL_PKG_CONFIG_PATH" \
    PATH="$SYSTEM_PATH" "$PYTHON_BIN" "$ROOT/scripts/configure_dependencies.py" \
    "$@" --onnx-prefix "$GLOBAL_ONNX_PREFIX"
}

run_waf_clean() {
  refresh_global_pkg_config_path
  local -a waf_args=("$@")
  if [[ "${waf_args[0]:-}" == "configure" ]]; then
    # Waf defaults LIBDIR to lib64 on this Ubuntu host.  The global Python
    # bindings and dependency-identity receipt use the canonical /usr/local/lib
    # root, so keep the NDNSF libraries and pkg-config files there as well.
    waf_args+=("--libdir=$GLOBAL_LIBRARY_DIR")
  fi
  # The top-level configure/build sees only canonical global pkg-config roots;
  # this includes NDN-CXX's valid /usr/local/lib64 installation.
  env \
    -u PKG_CONFIG_LIBDIR \
    -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
    -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH \
    -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH -u LDSHARED -u WAFDIR \
    -u PKGCONFIG -u LD -u AR -u AS -u RANLIB -u NM -u STRIP \
    -u OBJCOPY -u OBJDUMP -u READELF \
    -u BOOST_ROOT -u BOOST_INCLUDEDIR -u BOOST_LIBRARYDIR \
    PATH="$SYSTEM_PATH" PKGCONFIG="$PKG_CONFIG_BIN" \
    PKG_CONFIG_PATH="$GLOBAL_PKG_CONFIG_PATH" \
    CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld \
    AR=/usr/bin/ar AS=/usr/bin/as RANLIB=/usr/bin/ranlib \
    NM=/usr/bin/nm STRIP=/usr/bin/strip \
    NDNSF_LIBRARY_DIR="$GLOBAL_LIBRARY_DIR" \
    "$PYTHON_BIN" ./waf --out="$BUILD_DIR" --onnx-prefix="$GLOBAL_ONNX_PREFIX" "${waf_args[@]}"
}

prepare_build_dir() {
  local existed=0 temp_dir
  if [[ -z "$BUILD_DIR" ]]; then
    [[ "$REUSE_BUILD_DIR" == 0 ]] || {
      echo 'NDNSF_REUSE_BUILD_DIR=1 requires NDNSF_BUILD_DIR to name an existing build directory' >&2
      exit 2
    }
    BUILD_DIR="$(mktemp -d --tmpdir=/var/tmp ndnsf-build.XXXXXXXX)"
  else
    BUILD_DIR="$(realpath -m -- "$BUILD_DIR")"
    case "$BUILD_DIR" in
      "$ROOT"|"$ROOT"/*)
        echo "Build artifacts must stay outside the repository: $BUILD_DIR" >&2
        exit 2
        ;;
    esac
    if [[ -e "$BUILD_DIR" || -L "$BUILD_DIR" ]]; then
      [[ "$REUSE_BUILD_DIR" == 1 && -d "$BUILD_DIR" && ! -L "$BUILD_DIR" ]] || {
        echo "Build directory already exists; refusing to reuse it without NDNSF_REUSE_BUILD_DIR=1: $BUILD_DIR" >&2
        exit 2
      }
      existed=1
      manifest_tool build-marker \
        --build-dir "$BUILD_DIR" --root "$ROOT" --verify
    else
      mkdir -p -- "$BUILD_DIR"
    fi
  fi
  if (( ! existed )); then
    manifest_tool build-marker \
      --build-dir "$BUILD_DIR" --root "$ROOT"
  fi
  temp_dir="$(realpath -m -- "${NDNSF_TMPDIR:-$BUILD_DIR/tmp}")"
  case "$temp_dir" in
    "$ROOT"|"$ROOT"/*)
      echo "Temporary files must stay outside the source checkout: $temp_dir" >&2
      exit 2
      ;;
  esac
  mkdir -p -- "$temp_dir"
  export TMPDIR="$temp_dir"
  echo "==> External temporary directory: $TMPDIR"
  echo "==> External build/log root: $BUILD_DIR"
}

require_system_toolchain() {
  local tool
  select_global_boost || true
  require_global_boost
  for tool in gcc g++ ld ar as ranlib nm strip readelf "$PKG_CONFIG_BIN"; do
    if [[ "$tool" == /* ]]; then
      [[ -x "$tool" ]] || { echo "Missing canonical host tool: $tool" >&2; exit 1; }
    else
      [[ -x "/usr/bin/$tool" ]] || { echo "Missing canonical host tool: /usr/bin/$tool" >&2; exit 1; }
    fi
  done
}

has_global_boost() {
  local version_line
  [[ -n "$BOOST_INCLUDE_DIR" && -n "$BOOST_LIBRARY_DIR" && -n "$BOOST_VERSION_NUMBER" ]] || return 1
  if [[ ! -f "$BOOST_INCLUDE_DIR/boost/version.hpp" ]]; then
    return 1
  fi
  version_line="$(/usr/bin/grep -E '^#define[[:space:]]+BOOST_VERSION[[:space:]]+' \
    "$BOOST_INCLUDE_DIR/boost/version.hpp" | /usr/bin/awk '{print $3}' | /usr/bin/head -n 1)"
  if [[ "$version_line" != "$BOOST_VERSION_NUMBER" ]]; then
    return 1
  fi
  for library in libboost_system.so libboost_filesystem.so libboost_unit_test_framework.so; do
    local path="$BOOST_LIBRARY_DIR/$library"
    if [[ ! -e "$path" ]]; then
      return 1
    fi
    local resolved
    resolved="$(/usr/bin/readlink -f "$path" 2>/dev/null || true)"
    if [[ "$resolved" != "$BOOST_LIBRARY_DIR/"* ]]; then
      return 1
    fi
    local soname="${library}.${BOOST_VERSION_STRING}"
    if ! /usr/bin/readelf -d "$resolved" 2>/dev/null |
        /usr/bin/grep -Fq "Library soname: [$soname]"; then
      return 1
    fi
  done
  return 0
}

select_global_boost() {
  local selection
  selection="$("$PYTHON_BIN" "$ROOT/scripts/host_profile.py" "$HOST_UBUNTU_VERSION")" || return 1
  IFS=$'\t' read -r BOOST_INCLUDE_DIR BOOST_LIBRARY_DIR BOOST_VERSION_NUMBER BOOST_VERSION_STRING <<< "$selection"
}

refresh_global_pkg_config_path() {
  local candidate
  GLOBAL_PKG_CONFIG_PATH=""
  for candidate in \
      /usr/local/lib/pkgconfig /usr/local/lib64/pkgconfig \
      /opt/onnxruntime/lib/pkgconfig /opt/onnxruntime-1.26.0/lib/pkgconfig; do
    [[ -d "$candidate" ]] || continue
    GLOBAL_PKG_CONFIG_PATH="${GLOBAL_PKG_CONFIG_PATH:+$GLOBAL_PKG_CONFIG_PATH:}$candidate"
  done
}

pkg_config() {
  refresh_global_pkg_config_path
  env -u PKG_CONFIG_LIBDIR PKG_CONFIG_PATH="$GLOBAL_PKG_CONFIG_PATH" \
    "$PKG_CONFIG_BIN" "$@"
}

global_pkg_library_path() {
  local package="$1" library="$2" libdir
  libdir="$(pkg_config --variable=libdir "$package" 2>/dev/null)" || return 1
  [[ -n "$libdir" ]] || return 1
  printf '%s/%s\n' "$libdir" "$library"
}

has_global_sdk_pkg() {
  local package="$1" minimum="$2" library="$3"
  local version prefix libdir path resolved
  pkg_config --exists "$package" || return 1
  version="$(pkg_config --modversion "$package" 2>/dev/null)" || return 1
  [[ "$(printf '%s\n' "$minimum" "$version" | /usr/bin/sort -V | /usr/bin/head -n 1)" == "$minimum" ]] || return 1
  prefix="$(pkg_config --variable=prefix "$package" 2>/dev/null)" || return 1
  is_global_sdk_prefix "$prefix" || return 1
  libdir="$(pkg_config --variable=libdir "$package" 2>/dev/null)" || return 1
  path="$libdir/$library"
  resolved="$(/usr/bin/readlink -f "$path" 2>/dev/null || true)"
  [[ -f "$path" ]] && is_global_sdk_prefix "$resolved" || return 1
  require_global_pkg_flags "$package" >/dev/null 2>&1
}

require_global_boost() {
  local version_line
  if ! has_global_boost; then
    version_line="$(/usr/bin/grep -E '^#define[[:space:]]+BOOST_VERSION[[:space:]]+' \
      "$BOOST_INCLUDE_DIR/boost/version.hpp" 2>/dev/null | /usr/bin/awk '{print $3}' | /usr/bin/head -n 1 || true)"
    if [[ ! -f "$BOOST_INCLUDE_DIR/boost/version.hpp" ]]; then
      echo "Installed Boost headers are missing: $BOOST_INCLUDE_DIR/boost/version.hpp" >&2
    elif [[ "$version_line" != "$BOOST_VERSION_NUMBER" ]]; then
      echo "Installed Boost version is ${version_line:-unknown}; expected $BOOST_VERSION_NUMBER" >&2
    else
      echo "Installed Boost libraries are missing or resolve outside $BOOST_LIBRARY_DIR" >&2
    fi
    echo "Install a matching global Boost pair before configuring; no temporary prefix is allowed." >&2
    exit 1
  fi
  echo "==> Global Boost $BOOST_VERSION_STRING ($BOOST_INCLUDE_DIR, $BOOST_LIBRARY_DIR)"
}

is_global_sdk_prefix() {
  local raw="$1"
  local resolved
  resolved="$(/usr/bin/readlink -f "$raw" 2>/dev/null || true)"
  local root
  for root in "${GLOBAL_SDK_ROOTS[@]}"; do
    root="$(/usr/bin/readlink -f "$root" 2>/dev/null || true)"
    if [[ -n "$resolved" && ( "$resolved" == "$root" || "$resolved" == "$root"/* ) ]]; then
      return 0
    fi
  done
  return 1
}

require_global_sdk_pkg() {
  local package="$1"
  local minimum="$2"
  local library="$3"
  local version prefix
  if ! pkg_config --exists "$package"; then
    echo "Global SDK dependency is missing: $package >= $minimum" >&2
    echo "Install/rebuild it globally before continuing; no checkout or temporary prefix is allowed." >&2
    exit 1
  fi
  version="$(pkg_config --modversion "$package" 2>/dev/null || true)"
  if [[ "$(printf '%s\n' "$minimum" "$version" | /usr/bin/sort -V | /usr/bin/head -n 1)" != "$minimum" ]]; then
    echo "Global SDK dependency is too old: $package $version (need >= $minimum)" >&2
    echo "Install/rebuild it globally; do not point the build at a temporary prefix." >&2
    exit 1
  fi
  prefix="$(pkg_config --variable=prefix "$package" 2>/dev/null || true)"
  if ! is_global_sdk_prefix "$prefix"; then
    echo "$package resolves outside the declared global SDK roots: $prefix" >&2
    exit 1
  fi
  require_global_pkg_flags "$package"
  local libdir library_path resolved_library
  libdir="$(pkg_config --variable=libdir "$package" 2>/dev/null || true)"
  library_path="$libdir/$library"
  resolved_library="$(/usr/bin/readlink -f "$library_path" 2>/dev/null || true)"
  if [[ -z "$libdir" ]] || ! is_global_sdk_prefix "$libdir" || \
     [[ ! -e "$library_path" ]] || ! is_global_sdk_prefix "$resolved_library"; then
    echo "Global SDK library is missing or outside its prefix: $libdir/$library" >&2
    exit 1
  fi
  echo "==> Global SDK dependency $package $version ($prefix)"
}

global_dependency_identity_receipt() {
  local ndncxx_libdir ndnsd_libdir ndnsvs_libdir nacabe_libdir onnx_libdir
  ndncxx_libdir="$(pkg_config --variable=libdir libndn-cxx 2>/dev/null || true)"
  ndnsd_libdir="$(pkg_config --variable=libdir ndnsd 2>/dev/null || true)"
  ndnsvs_libdir="$(pkg_config --variable=libdir libndn-svs 2>/dev/null || true)"
  nacabe_libdir="$(pkg_config --variable=libdir libnac-abe 2>/dev/null || true)"
  onnx_libdir="$(pkg_config --variable=libdir onnxruntime 2>/dev/null || true)"
  if [[ -z "$ndncxx_libdir" || -z "$ndnsd_libdir" || -z "$ndnsvs_libdir" || \
        -z "$nacabe_libdir" || -z "$onnx_libdir" ]]; then
    echo "Cannot determine a global dependency library directory" >&2
    return 1
  fi
  "$PYTHON_BIN" - "$ndncxx_libdir" "$ndnsd_libdir" "$ndnsvs_libdir" \
    "$nacabe_libdir" "$GLOBAL_LIBRARY_DIR" "$onnx_libdir" <<'PY'
import hashlib
import json
import pathlib
import re
import subprocess
import sys

ndncxx_root, ndnsd_root, ndnsvs_root, nacabe_root = map(pathlib.Path, sys.argv[1:5])
openabe_root = pathlib.Path(sys.argv[5])
onnx_root = pathlib.Path(sys.argv[6])
paths = {
    "libndn-cxx.so": ndncxx_root / "libndn-cxx.so",
    "libndnsd.so": ndnsd_root / "libndnsd.so",
    "libndn-svs.so": ndnsvs_root / "libndn-svs.so",
    "libnac-abe.so": nacabe_root / "libnac-abe.so",
    "libopenabe.so": openabe_root / "libopenabe.so",
    "libonnxruntime.so": onnx_root / "libonnxruntime.so",
}
entries = {}
for name, path in paths.items():
    if not path.is_file():
        raise SystemExit(f"missing global dependency library: {path}")
    resolved = path.resolve()
    output = subprocess.check_output(
        ["/usr/bin/readelf", "-d", str(resolved)], text=True)
    match = re.search(r"Library soname: \[([^]]+)\]", output)
    entries[name] = {
        "path": str(path),
        "realpath": str(resolved),
        "sha256": hashlib.sha256(resolved.read_bytes()).hexdigest(),
        "soname": match.group(1) if match else "",
    }
print(json.dumps({"schema": 1, "libraries": entries}, sort_keys=True))
PY
}

write_global_dependency_identity() {
  local receipt stage
  receipt="$(global_dependency_identity_receipt)" || exit 1
  stage="$(mktemp -d "$BUILD_DIR/global-identity-stage.XXXXXXXX")"
  mkdir -p -- "$stage/share/ndnsf"
  printf '%s\n' "$receipt" > "$stage/share/ndnsf/global-dependency-identity.json"
  install_manifest_tree "$stage" "$GLOBAL_DEPENDENCY_PREFIX" global-dependency-identity
  rm -rf -- "$stage"
  echo "==> Global dependency identity receipt: $GLOBAL_IDENTITY_FILE"
}

require_global_dependency_identity() {
  local ndncxx_libdir ndnsd_libdir ndnsvs_libdir nacabe_libdir onnx_libdir
  [[ -f "$GLOBAL_IDENTITY_FILE" ]] || {
    echo "Global dependency identity receipt is missing: $GLOBAL_IDENTITY_FILE" >&2
    echo "Run the installer to install/rebuild the global dependency closure." >&2
    exit 1
  }
  ndncxx_libdir="$(pkg_config --variable=libdir libndn-cxx 2>/dev/null || true)"
  ndnsd_libdir="$(pkg_config --variable=libdir ndnsd 2>/dev/null || true)"
  ndnsvs_libdir="$(pkg_config --variable=libdir libndn-svs 2>/dev/null || true)"
  nacabe_libdir="$(pkg_config --variable=libdir libnac-abe 2>/dev/null || true)"
  onnx_libdir="$(pkg_config --variable=libdir onnxruntime 2>/dev/null || true)"
  "$PYTHON_BIN" - "$GLOBAL_IDENTITY_FILE" "$ndncxx_libdir" "$ndnsd_libdir" \
    "$ndnsvs_libdir" "$nacabe_libdir" "$GLOBAL_LIBRARY_DIR" "$onnx_libdir" <<'PY'
import hashlib
import json
import pathlib
import re
import subprocess
import sys

receipt_path = pathlib.Path(sys.argv[1])
ndncxx_root, ndnsd_root, ndnsvs_root, nacabe_root = map(pathlib.Path, sys.argv[2:6])
openabe_root = pathlib.Path(sys.argv[6])
onnx_root = pathlib.Path(sys.argv[7])
try:
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
except (OSError, ValueError) as error:
    raise SystemExit(f"cannot read global dependency identity receipt: {error}")
if receipt.get("schema") != 1 or not isinstance(receipt.get("libraries"), dict):
    raise SystemExit(f"invalid global dependency identity receipt: {receipt_path}")
expected_paths = {
    "libndn-cxx.so": ndncxx_root / "libndn-cxx.so",
    "libndnsd.so": ndnsd_root / "libndnsd.so",
    "libndn-svs.so": ndnsvs_root / "libndn-svs.so",
    "libnac-abe.so": nacabe_root / "libnac-abe.so",
    "libopenabe.so": openabe_root / "libopenabe.so",
    "libonnxruntime.so": onnx_root / "libonnxruntime.so",
}
for name, path in expected_paths.items():
    entry = receipt["libraries"].get(name)
    if not isinstance(entry, dict):
        raise SystemExit(f"identity receipt missing {name}")
    actual = path.resolve()
    if not path.is_file() or str(entry.get("path")) != str(path) or \
            str(entry.get("realpath")) != str(actual):
        raise SystemExit(f"global dependency realpath changed for {name}: {path}")
    digest = hashlib.sha256(actual.read_bytes()).hexdigest()
    if digest != entry.get("sha256"):
        raise SystemExit(f"global dependency digest changed for {name}: {path}")
    output = subprocess.check_output(["/usr/bin/readelf", "-d", str(actual)], text=True)
    match = re.search(r"Library soname: \[([^]]+)\]", output)
    actual_soname = match.group(1) if match else ""
    if actual_soname != entry.get("soname"):
        raise SystemExit(f"global dependency SONAME changed for {name}: {path}")
PY
}

has_global_dependency_identity() {
  require_global_dependency_identity >/dev/null 2>&1
}

has_global_onnx_identity() {
  local onnx_libdir
  [[ -f "$GLOBAL_IDENTITY_FILE" ]] || return 1
  onnx_libdir="$(pkg_config --variable=libdir onnxruntime 2>/dev/null || true)"
  "$PYTHON_BIN" - "$GLOBAL_IDENTITY_FILE" "$onnx_libdir" <<'PY' >/dev/null
import hashlib
import json
import pathlib
import re
import subprocess
import sys

receipt = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
entry = receipt.get("libraries", {}).get("libonnxruntime.so")
path = pathlib.Path(sys.argv[2]) / "libonnxruntime.so"
if not isinstance(entry, dict) or str(entry.get("path")) != str(path):
    raise SystemExit(1)
resolved = path.resolve()
if not path.is_file() or str(entry.get("realpath")) != str(resolved):
    raise SystemExit(1)
if not any(str(resolved).startswith(root + "/")
           for root in ("/opt/onnxruntime", "/opt/onnxruntime-1.26.0")):
    raise SystemExit(1)
if hashlib.sha256(resolved.read_bytes()).hexdigest() != entry.get("sha256"):
    raise SystemExit(1)
output = subprocess.check_output(["/usr/bin/readelf", "-d", str(resolved)], text=True)
match = re.search(r"Library soname: \[([^]]+)\]", output)
if (match.group(1) if match else "") != entry.get("soname"):
    raise SystemExit(1)
PY
}

is_pkg_installed() {
  local package="$1"
  local minimum="${2:-0}"
  local version prefix libdir library library_path resolved_library
  if ! pkg_config --exists "$package"; then
    return 1
  fi
  version="$(pkg_config --modversion "$package" 2>/dev/null)" || return 1
  if [[ "$(printf '%s\n' "$minimum" "$version" | /usr/bin/sort -V | /usr/bin/head -n 1)" != "$minimum" ]]; then
    return 1
  fi
  prefix="$(pkg_config --variable=prefix "$package" 2>/dev/null)" || return 1
  prefix="$(/usr/bin/readlink -f "$prefix" 2>/dev/null || true)"
  [[ "$prefix" == "$GLOBAL_DEPENDENCY_PREFIX" || "$prefix" == "$GLOBAL_DEPENDENCY_PREFIX"/* ]] || return 1
  case "$package" in
    libndn-cxx) library="libndn-cxx.so" ;;
    ndnsd) library="libndnsd.so" ;;
    libndn-svs) library="libndn-svs.so" ;;
    libnac-abe) library="libnac-abe.so" ;;
    *) return 0 ;;
  esac
  libdir="$(pkg_config --variable=libdir "$package" 2>/dev/null)" || return 1
  library_path="$libdir/$library"
  resolved_library="$(/usr/bin/readlink -f "$library_path" 2>/dev/null || true)"
  [[ -f "$library_path" && "$resolved_library" == "$GLOBAL_DEPENDENCY_PREFIX"/* ]] || return 1
  require_global_pkg_flags "$package" >/dev/null 2>&1
}

require_global_pkg_flags() {
  "$PYTHON_BIN" - "$@" <<'PY'
import os
import pathlib
import shlex
import subprocess
import sys

roots = tuple(pathlib.Path(item).resolve()
              for item in ("/usr", "/usr/local", "/lib", "/lib64",
                           "/opt/onnxruntime", "/opt/onnxruntime-1.26.0"))
env = dict(os.environ)
env.pop("PKG_CONFIG_PATH", None)
env.pop("PKG_CONFIG_LIBDIR", None)
env["PKG_CONFIG_PATH"] = (
    "/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig:"
    "/opt/onnxruntime/lib/pkgconfig:/opt/onnxruntime-1.26.0/lib/pkgconfig")
env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"

for package in sys.argv[1:]:
    output = subprocess.check_output(
        ["/usr/bin/pkg-config", "--cflags", "--libs", package], env=env, text=True)
    tokens = shlex.split(output)
    paths = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in ("-I", "-isystem", "-L") and index + 1 < len(tokens):
            paths.append(tokens[index + 1])
            index += 2
            continue
        if token.startswith(("-I", "-isystem", "-L")):
            for prefix in ("-isystem", "-I", "-L"):
                if token.startswith(prefix) and token != prefix:
                    paths.append(token[len(prefix):])
                    break
        if token.startswith("-Wl,"):
            parts = token[4:].split(",")
            for part_index, part in enumerate(parts):
                if part in ("-rpath", "-rpath-link", "-R", "-L",
                            "--rpath", "--rpath-link") and part_index + 1 < len(parts):
                    paths.append(parts[part_index + 1])
                elif part.startswith("-rpath="):
                    paths.append(part.split("=", 1)[1])
                elif part.startswith(("-rpath-link=", "--rpath=", "--rpath-link=")):
                    paths.append(part.split("=", 1)[1])
                elif part.startswith("-R") and part != "-R":
                    paths.append(part[2:])
                elif part.startswith("-L") and part != "-L":
                    paths.append(part[2:])
        index += 1
    for value in paths:
        path = pathlib.Path(value).expanduser().resolve()
        if not any(path == root or root in path.parents for root in roots):
            raise SystemExit(
                f"{package} exposes a non-global compiler/linker path: {path}")
PY
}

require_global_pkg() {
  local package="$1"
  local minimum="$2"
  local library="$3"
  local version prefix
  if ! is_pkg_installed "$package" "$minimum"; then
    echo "Global dependency is missing, too old, or outside $GLOBAL_DEPENDENCY_PREFIX: $package >= $minimum" >&2
    echo "Install/rebuild it globally before continuing; no checkout or temporary prefix is allowed." >&2
    exit 1
  fi
  version="$(pkg_config --modversion "$package")"
  prefix="$(/usr/bin/readlink -f "$(pkg_config --variable=prefix "$package")")"
  require_global_pkg_flags "$package"
  local library_path
  library_path="$(global_pkg_library_path "$package" "$library")"
  require_global_file "$library_path"
  echo "==> Global dependency $package $version ($prefix)"
}

require_global_file() {
  local path="$1"
  local resolved
  if [[ ! -f "$path" ]]; then
    echo "Global dependency file is missing: $path" >&2
    echo "Install the matching dependency under $GLOBAL_DEPENDENCY_PREFIX before continuing." >&2
    exit 1
  fi
  resolved="$(readlink -f "$path")"
  if ! is_global_sdk_prefix "$resolved"; then
    echo "Global dependency resolves outside the declared SDK roots: $path -> $resolved" >&2
    exit 1
  fi
}

require_global_external_closure() {
  require_global_boost
  require_global_pkg "libndn-cxx" "$MIN_NDNCXX_VERSION" "libndn-cxx.so"
  require_global_pkg "ndnsd" "$MIN_NDNSD_VERSION" "libndnsd.so"
  require_global_pkg "libndn-svs" "$MIN_NDNSVS_VERSION" "libndn-svs.so"
  require_global_pkg "libnac-abe" "$MIN_NACABE_VERSION" "libnac-abe.so"
  require_global_sdk_pkg "onnxruntime" "1.26.0" "libonnxruntime.so"
  require_global_file "$GLOBAL_LIBRARY_DIR/libopenabe.so"
  require_global_file "$GLOBAL_ONNX_PREFIX/lib/libonnx.a"
  require_global_file "$GLOBAL_ONNX_PREFIX/lib/libonnx_proto.a"
  require_global_file "$GLOBAL_LIBRARY_DIR/libndnsf_tokenizer_bridge.a"
  require_global_file "$GLOBAL_ONNX_PREFIX/include/onnx/checker.h"
  require_global_file "$GLOBAL_ONNX_PREFIX/include/onnx/shape_inference/implementation.h"
  require_global_dependency_identity
}

native_digest_receipt() {
  "$PYTHON_BIN" - "$GLOBAL_LIBRARY_DIR" <<'PY'
import hashlib
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
names = ("libndn-service-framework.so", "libndnsf-distributed-inference.so")
paths = [root / name for name in names]
missing = [str(path) for path in paths if not path.is_file()]
if missing:
    raise SystemExit("missing installed NDNSF library: " + ", ".join(missing))
print(json.dumps({name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                  for name in names}, sort_keys=True))
PY
}

has_openabe() {
  [[ "$OPENABE_PREFIX" == "$GLOBAL_DEPENDENCY_PREFIX" ]] || return 1
  local library="$GLOBAL_DEPENDENCY_PREFIX/lib/libopenabe.so"
  [[ -f "$library" ]] || return 1
  local resolved
  resolved="$(readlink -f "$library")"
  [[ "$resolved" == "$GLOBAL_DEPENDENCY_PREFIX/lib/"* ]] || return 1
  # These C ABI exports are consumed by NAC-ABE.  A same-name library with an
  # older or unrelated ABI must not silently satisfy the install preflight.
  for symbol in OpenABEalloc OpenABEfree openSslInitialize; do
    if ! nm -D --defined-only -C "$library" 2>/dev/null |
        grep -E " ${symbol}\\(" >/dev/null; then
      return 1
    fi
  done
  return 0
}

install_common_system_packages() {
  if [[ "$INSTALL_SYSTEM_PACKAGES" != "1" ]]; then
    echo "==> Skipping OS package installation"
    return
  fi

  if command -v apt-get >/dev/null 2>&1; then
    echo "==> Installing common Debian/Ubuntu build packages"
    local packages=(
      build-essential binutils git pkg-config cmake python3 python3-pip wget curl \
      python3-dev python3-setuptools python3-wheel python3-venv \
      autoconf automake libtool m4 bison flex ninja-build \
      libgmp-dev libssl-dev \
      libsqlite3-dev libpcap-dev libsodium-dev zlib1g-dev \
      liblog4cxx-dev sqlite3 libprotobuf-dev protobuf-compiler libgtkmm-3.0-dev \
      libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libffi-dev
    )
    if [[ "$HOST_UBUNTU_VERSION" == "20.04" ]]; then
      packages+=(libpcre3-dev)
    fi
    if ! has_global_boost; then
      packages+=(libboost-all-dev)
    fi
    if [[ ! -f "$GLOBAL_LIBRARY_DIR/libndnsf_tokenizer_bridge.a" ]]; then
      command -v rustc >/dev/null 2>&1 || packages+=(rustc)
      command -v cargo >/dev/null 2>&1 || packages+=(cargo)
    fi
    if [[ "$INSTALL_TEST_PACKAGES" == "1" ]]; then
      packages+=(libgtest-dev)
    fi
    if [[ "$INSTALL_DOC_PACKAGES" == "1" ]]; then
      packages+=(doxygen graphviz)
    fi
    if [[ "$INSTALL_MININDN_PACKAGES" == "1" ]]; then
      packages+=(
        mininet openvswitch-switch tcpdump iproute2 net-tools
        python3-pyroute2 python3-networkx python3-matplotlib python3-igraph
        python3-tqdm python3-joblib
      )
    fi
    if [[ "$INSTALL_YOLO_MININDN" == "1" ]]; then
      packages+=(libbz2-dev)
    fi
    if [[ "$INSTALL_NFD_NLSR_PACKAGES" == "1" ]]; then
      packages+=(
        libsystemd-dev libcap-dev libprotobuf-dev protobuf-compiler
        libsqlite3-dev libpcap-dev libsodium-dev
      )
    fi
    local missing=() package state apt_simulation
    for package in "${packages[@]}"; do
      state="$(dpkg-query -W -f='${Status}' "$package" 2>/dev/null || true)"
      [[ "$state" == 'install ok installed' ]] || missing+=("$package")
    done
    if ((${#missing[@]})); then
      if (( INSTALL_YOLO_MININDN )); then
        local -a newly_installed=() planned=()
        local -A seen_packages=()
        sudo_run apt-get update
        echo "==> Simulating APT so uninstall can track this profile's complete new package closure"
        if [[ "$EUID" -eq 0 ]]; then
          apt_simulation="$(apt-get -s install --no-install-recommends --no-upgrade -y "${missing[@]}")"
        else
          apt_simulation="$(sudo -n apt-get -s install --no-install-recommends --no-upgrade -y "${missing[@]}")"
        fi
        mapfile -t planned < <(printf '%s\n' "$apt_simulation" | /usr/bin/awk '$1 == "Inst" {print $2}')
        for package in "${missing[@]}" "${planned[@]}"; do
          state="$(dpkg-query -W -f='${Status}' "$package" 2>/dev/null || true)"
          if [[ "$state" != 'install ok installed' && -z "${seen_packages[$package]+present}" ]]; then
            newly_installed+=("$package")
            seen_packages["$package"]=1
          fi
        done
        if ((${#newly_installed[@]})); then
          manifest_tool --privileged record-apt \
            --manifest "$INSTALL_MANIFEST" --packages "${newly_installed[@]}"
        fi
        sudo_run env DEBIAN_FRONTEND=noninteractive apt-get install \
          --no-install-recommends --no-upgrade -y "${missing[@]}"
      else
        manifest_tool --privileged record-apt \
          --manifest "$INSTALL_MANIFEST" --packages "${missing[@]}"
        sudo_run apt-get update
        sudo_run env DEBIAN_FRONTEND=noninteractive apt-get install \
          --no-install-recommends -y "${missing[@]}"
      fi
    fi
    select_global_boost || {
      echo "No compatible Boost pair found after OS package installation" >&2
      return 1
    }
  else
    echo "==> No supported OS package manager detected; assuming build tools are installed"
    select_global_boost || {
      echo "No compatible Boost pair found on this host" >&2
      return 1
    }
  fi
}

ensure_source_tree() {
  local name="$1"
  local url="$2"
  local source_base="${3:-$DEPS_DIR}"
  local locked_url commit expected was_present was_base_present source_path
  locked_url="$(source_field "$name" url)"
  [[ "$url" == "$locked_url" ]] || { echo "URL differs from lock for $name; use --lock-file" >&2; return 1; }
  commit="$(source_field "$name" commit)"
  expected="$source_base/$name-$commit"
  was_present=0
  [[ -e "$expected" || -L "$expected" ]] && was_present=1
  was_base_present=0
  [[ -d "$source_base" ]] && was_base_present=1
  source_path="$("$PYTHON_BIN" "$SOURCE_HELPER" prepare --lock "$LOCK_FILE" --name "$name" --deps-dir "$source_base")"
  if (( ! was_present )); then
    if (( was_base_present )); then
      record_source_checkout "$name" "$locked_url" "$commit" "$source_path" "$source_base" 0
    else
      record_source_checkout "$name" "$locked_url" "$commit" "$source_path" "$source_base" 1
    fi
  fi
  printf '%s\n' "$source_path"
}

ensure_fixed_source_tree() {
  local name="$1" url="$2" commit="$3"
  local source_base source_path staging_root staging_path
  local was_present=0 was_base_present=0
  source_base="$(realpath -m -- "$DEPS_DIR")"
  source_path="$source_base/$name-$commit"
  [[ -e "$source_path" || -L "$source_path" ]] && was_present=1
  if (( was_present )); then
    [[ ! -L "$source_path" && -d "$source_path/.git" ]] || {
      echo "Not a standalone pinned source checkout: $source_path" >&2
      return 1
    }
    [[ "$(git -C "$source_path" remote get-url origin)" == "$url" ]] || {
      echo "Wrong source remote; preserving existing tree: $source_path" >&2
      return 1
    }
    [[ "$(git -C "$source_path" rev-parse HEAD)" == "$commit" ]] || {
      echo "Wrong source commit; preserving existing tree: $source_path" >&2
      return 1
    }
    [[ -z "$(git -C "$source_path" status --porcelain --untracked-files=all)" ]] || {
      echo "Dirty source tree; preserving existing changes: $source_path" >&2
      return 1
    }
  else
    [[ -d "$source_base" ]] && was_base_present=1
    mkdir -p -- "$source_base"
    staging_root="$(mktemp -d "$source_base/.ndnsf-source-${name}.XXXXXXXX")"
    staging_path="$staging_root/$name-$commit"
    if ! {
      run git clone --no-checkout -- "$url" "$staging_path" &&
        run git -C "$staging_path" fetch --no-tags origin "$commit" &&
        run git -C "$staging_path" -c advice.detachedHead=false checkout --detach "$commit" &&
        run git -C "$staging_path" submodule update --init --recursive
    }; then
      rm -rf -- "$staging_root"
      if (( ! was_base_present )); then
        rmdir -- "$source_base" 2>/dev/null || true
      fi
      return 1
    fi
    if [[ "$(git -C "$staging_path" remote get-url origin)" != "$url" ||
          "$(git -C "$staging_path" rev-parse HEAD)" != "$commit" ||
          -n "$(git -C "$staging_path" status --porcelain --untracked-files=all)" ]]; then
      echo "Pinned source checkout failed validation; preserving existing trees" >&2
      rm -rf -- "$staging_root"
      if (( ! was_base_present )); then
        rmdir -- "$source_base" 2>/dev/null || true
      fi
      return 1
    fi
    if ! mv -nT -- "$staging_path" "$source_path" ||
        [[ -e "$staging_path" || -L "$staging_path" ]]; then
      echo "Could not publish pinned source checkout without replacing an existing path: $source_path" >&2
      rm -rf -- "$staging_root"
      if (( ! was_base_present )); then
        rmdir -- "$source_base" 2>/dev/null || true
      fi
      return 1
    fi
    rmdir -- "$staging_root"
    if ! record_source_checkout "$name" "$url" "$commit" "$source_path" \
        "$source_base" "$(( ! was_base_present ))"; then
      echo "Could not record ownership of the new source checkout: $source_path" >&2
      rm -rf -- "$source_path"
      if (( ! was_base_present )); then
        rmdir -- "$source_base" 2>/dev/null || true
      fi
      return 1
    fi
  fi
  printf '%s\n' "$source_path"
}

source_field() {
  "$PYTHON_BIN" "$SOURCE_HELPER" field --lock "$LOCK_FILE" --name "$1" --field "$2"
}

host_sdk_lock_field() {
  "$PYTHON_BIN" - "$ROOT/packaging/host-sdk-dependencies.lock.json" "$1" <<'PY'
import json
import pathlib
import sys

document = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding='utf-8'))
value = document
for key in sys.argv[2].split('.'):
    value = value[key]
if not isinstance(value, (str, int)):
    raise SystemExit(f'lock field is not scalar: {sys.argv[2]}')
print(value)
PY
}

record_source_receipt() {
  local name="$1" stage
  stage="$(mktemp -d "$BUILD_DIR/receipt-stage.XXXXXXXX")"
  mkdir -p -- "$stage/share/ndnsf/source-receipts"
  "$PYTHON_BIN" - "$ROOT/scripts" "$LOCK_FILE" "$name" \
    "$GLOBAL_DEPENDENCY_PREFIX" "$stage/share/ndnsf/source-receipts/$name.json" <<'PY'
import json
from pathlib import Path
import sys

sys.path.insert(0, sys.argv[1])
import stack_sources

receipt = stack_sources.fingerprint(
    stack_sources.load_lock(sys.argv[2]), sys.argv[3], sys.argv[4])
destination = Path(sys.argv[5])
destination.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n",
                       encoding="utf-8")
PY
  install_manifest_tree "$stage" "$GLOBAL_DEPENDENCY_PREFIX" "source-receipt:$name"
  rm -rf -- "$stage"
}

build_openabe_dependency() {
  local dir
  local source_base="$BUILD_DIR/openabe-source"
  local install_stage="$BUILD_DIR/install-staging/openabe/usr/local"

  if [[ "$FORCE_DEPENDENCIES" != "1" ]] && has_openabe; then
    echo "==> OpenABE already installed; skipping"
    return
  fi

  dir="$(ensure_source_tree "openabe" "$OPENABE_REPO_URL" "$source_base" | tail -n 1)"
  mkdir -p -- "$install_stage"
  echo "==> Building OpenABE from the dependency source tree"
  run bash -e -c "cd \"\$1\" && . ./env && \
    unset PKG_CONFIG_PATH PKG_CONFIG_LIBDIR CFLAGS CXXFLAGS CPPFLAGS LDFLAGS \
      LD_LIBRARY_PATH LIBRARY_PATH CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH \
      BOOST_ROOT BOOST_INCLUDEDIR BOOST_LIBRARYDIR && \
    export PATH='$SYSTEM_PATH' CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld \
      AR=/usr/bin/ar RANLIB=/usr/bin/ranlib NM=/usr/bin/nm STRIP=/usr/bin/strip \
      BOOST_ROOT= BOOST_INCLUDEDIR= BOOST_LIBRARYDIR= && \
    { make clean >/dev/null 2>&1 || true; } && \
    make -C deps/relic && make -C deps/gtest && \
    USE_DEPS='relic gtest' BISON=\$(command -v bison) FLEX=\$(command -v flex) \
      make -j'$JOBS' src && \
    USE_DEPS='relic gtest' make -j'$JOBS' examples" _ "$dir"
  echo "==> Installing OpenABE into the global prefix $OPENABE_PREFIX"
  run env \
    -u PKG_CONFIG_PATH -u CXXFLAGS -u CFLAGS -u CPPFLAGS -u LDFLAGS \
    -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH \
    -u CPLUS_INCLUDE_PATH -u BOOST_ROOT -u BOOST_INCLUDEDIR -u BOOST_LIBRARYDIR \
    PATH="$SYSTEM_PATH" CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld \
    AR=/usr/bin/ar RANLIB=/usr/bin/ranlib NM=/usr/bin/nm STRIP=/usr/bin/strip \
    bash -e -c \
    "cd \"\$1\" && . ./env && unset PKG_CONFIG_PATH PKG_CONFIG_LIBDIR CFLAGS \
      CXXFLAGS CPPFLAGS LDFLAGS LD_LIBRARY_PATH LIBRARY_PATH CPATH \
      C_INCLUDE_PATH CPLUS_INCLUDE_PATH BOOST_ROOT BOOST_INCLUDEDIR \
      BOOST_LIBRARYDIR && export PATH='$SYSTEM_PATH' CC=/usr/bin/gcc \
      CXX=/usr/bin/g++ LD=/usr/bin/ld AR=/usr/bin/ar RANLIB=/usr/bin/ranlib \
      NM=/usr/bin/nm STRIP=/usr/bin/strip && \
      USE_DEPS='relic gtest' make INSTALL_PREFIX=\"\$2\" install" _ "$dir" "$install_stage"
  install_manifest_tree "$install_stage" "$OPENABE_PREFIX" openabe
  rm -rf -- "$BUILD_DIR/install-staging/openabe"
  sudo_run ldconfig
  record_source_receipt openabe
}

build_waf_dependency() {
  local name="$1"
  local pkg="$2"
  local url="$3"
  local minimum="$4"
  local dir
  local install_stage="$BUILD_DIR/install-staging/$name"
  local -a waf_env

  if [[ "$FORCE_DEPENDENCIES" != "1" ]] && is_pkg_installed "$pkg" "$minimum"; then
    echo "==> $name already installed ($pkg); skipping"
    return
  fi

  dir="$(ensure_source_tree "$name" "$url" | tail -n 1)"
  mkdir -p -- "$install_stage"
  echo "==> Building dependency $name"
  # Match Mini-NDN's per-project Waf lifecycle: finish one dependency before
  # moving on to the next, and report configure/build/install failures at the
  # exact command boundary. Use the same selected system Python for every Waf.
  waf_env=(
    env -u PKG_CONFIG_LIBDIR -u CXXFLAGS -u CFLAGS -u CPPFLAGS -u LDFLAGS
    -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH
    -u CPLUS_INCLUDE_PATH -u BOOST_ROOT -u BOOST_INCLUDEDIR -u BOOST_LIBRARYDIR
    PATH="$SYSTEM_PATH" PKGCONFIG="$PKG_CONFIG_BIN"
    PKG_CONFIG_PATH="$GLOBAL_PKG_CONFIG_PATH"
    CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld AR=/usr/bin/ar
    AS=/usr/bin/as RANLIB=/usr/bin/ranlib NM=/usr/bin/nm STRIP=/usr/bin/strip
  )
  (
    cd "$dir"
    run "${waf_env[@]}" "$PYTHON_BIN" ./waf distclean
    run "${waf_env[@]}" "$PYTHON_BIN" ./waf configure "--prefix=$GLOBAL_DEPENDENCY_PREFIX"
    run "${waf_env[@]}" "$PYTHON_BIN" ./waf "-j$JOBS"
  )
  echo "==> Installing dependency $name"
  (
    cd "$dir"
    run "${waf_env[@]}" "$PYTHON_BIN" ./waf install "--destdir=$install_stage"
  )
  install_manifest_tree "$install_stage/usr/local" "$GLOBAL_DEPENDENCY_PREFIX" "$name"
  rm -rf -- "$install_stage"
  sudo_run ldconfig
  record_source_receipt "$name"
}

preserve_existing_stage_targets() {
  local staged_root="$1" staged target relative
  [[ -d "$staged_root" ]] || return 0
  while IFS= read -r -d '' staged; do
    relative="${staged#"$staged_root"}"
    target="/usr/local$relative"
    if [[ -e "$target" || -L "$target" ]]; then
      rm -f -- "$staged"
    fi
  done < <(find "$staged_root" \( -type f -o -type l \) -print0)
}

build_fixed_minindn_waf_dependency() {
  local name="$1" url="$2" commit="$3"
  shift 3
  local -a required=("$@")
  local bin missing=0 dir build_out stage_root
  local -a waf_env

  for bin in "${required[@]}"; do
    if [[ "$bin" == pkg:* ]]; then
      if ! env -u PKG_CONFIG_LIBDIR PKG_CONFIG_PATH="$GLOBAL_PKG_CONFIG_PATH" \
          "$PKG_CONFIG_BIN" --exists "${bin#pkg:}"; then
        missing=1
      fi
    elif ! PATH="$SYSTEM_PATH:/usr/local/bin" command -v "$bin" >/dev/null; then
      missing=1
    fi
  done
  if (( ! missing )); then
    echo "==> Reusing installed Mini-NDN prerequisite(s): ${required[*]}"
    return 0
  fi

  dir="$(ensure_fixed_source_tree "$name" "$url" "$commit" | tail -n 1)"
  build_out="$(mktemp -d "$BUILD_DIR/minindn-build-${name}.XXXXXXXX")"
  mkdir -p -- "$BUILD_DIR/install-staging"
  stage_root="$(mktemp -d "$BUILD_DIR/install-staging/minindn-${name}.XXXXXXXX")"
  waf_env=(
    env -u PKG_CONFIG_LIBDIR -u CXXFLAGS -u CFLAGS -u CPPFLAGS -u LDFLAGS
    -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH
    -u CPLUS_INCLUDE_PATH -u BOOST_ROOT -u BOOST_INCLUDEDIR -u BOOST_LIBRARYDIR
    PATH="$SYSTEM_PATH:/usr/local/bin" PKGCONFIG="$PKG_CONFIG_BIN"
    PKG_CONFIG_PATH="$GLOBAL_PKG_CONFIG_PATH"
    CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld AR=/usr/bin/ar
    AS=/usr/bin/as RANLIB=/usr/bin/ranlib NM=/usr/bin/nm STRIP=/usr/bin/strip
  )
  echo "==> Building pinned Mini-NDN prerequisite $name in $build_out"
  (
    cd "$dir"
    run "${waf_env[@]}" "$PYTHON_BIN" ./waf --out="$build_out" distclean
    run "${waf_env[@]}" "$PYTHON_BIN" ./waf --out="$build_out" configure \
      "--prefix=$GLOBAL_DEPENDENCY_PREFIX"
    run "${waf_env[@]}" "$PYTHON_BIN" ./waf --out="$build_out" "-j$JOBS"
    run "${waf_env[@]}" "$PYTHON_BIN" ./waf --out="$build_out" install \
      "--destdir=$stage_root"
    run "${waf_env[@]}" "$PYTHON_BIN" ./waf --out="$build_out" distclean
  )
  preserve_existing_stage_targets "$stage_root/usr/local"
  install_manifest_tree "$stage_root/usr/local" "$GLOBAL_DEPENDENCY_PREFIX" \
    "minindn:$name"
  if [[ -n "$(git -C "$dir" status --porcelain --untracked-files=all)" ]]; then
    echo "Pinned source tree is not clean after its out-of-tree build: $dir" >&2
    return 1
  fi
  rm -rf -- "$build_out" "$stage_root"
  sudo_run ldconfig
}

build_fixed_minindn_infoedit() {
  local dir stage_root
  if PATH="$SYSTEM_PATH:/usr/local/bin" command -v infoedit >/dev/null; then
    echo '==> Reusing installed Mini-NDN prerequisite: infoedit'
    return 0
  fi
  dir="$(ensure_fixed_source_tree infoedit "$MININDN_INFOEDIT_URL" \
    "$MININDN_INFOEDIT_COMMIT" | tail -n 1)"
  mkdir -p -- "$BUILD_DIR/install-staging"
  stage_root="$(mktemp -d "$BUILD_DIR/install-staging/minindn-infoedit.XXXXXXXX")"
  run env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
    CC=/usr/bin/gcc CXX=/usr/bin/g++ PATH="$SYSTEM_PATH" \
    make -C "$dir" -j"$JOBS"
  run env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
    CC=/usr/bin/gcc CXX=/usr/bin/g++ PATH="$SYSTEM_PATH" \
    make -C "$dir" DESTDIR="$stage_root" \
      PREFIX="$GLOBAL_DEPENDENCY_PREFIX" install
  run make -C "$dir" clean
  preserve_existing_stage_targets "$stage_root/usr/local"
  install_manifest_tree "$stage_root/usr/local" "$GLOBAL_DEPENDENCY_PREFIX" \
    minindn:infoedit
  if [[ -n "$(git -C "$dir" status --porcelain --untracked-files=all)" ]]; then
    echo "Pinned infoedit source tree is not clean after make clean: $dir" >&2
    return 1
  fi
  rm -rf -- "$stage_root"
}

check_minindn_ndn_base() {
  local tool nfd_version cxx_version
  for tool in nfd nfdc ndnsec; do
    PATH="$SYSTEM_PATH:/usr/local/bin" command -v "$tool" >/dev/null || {
      echo "Mini-NDN profile requires the existing $tool command; it will not replace NFD/ndn-cxx" >&2
      return 1
    }
  done
  nfd_version="$(PATH="$SYSTEM_PATH:/usr/local/bin" nfd --version 2>/dev/null)"
  [[ "$nfd_version" == 24.07* ]] || {
    echo "Mini-NDN v0.7.0 profile is pinned for NFD 24.07; found '$nfd_version'. Existing NFD left untouched." >&2
    return 1
  }
  refresh_global_pkg_config_path
  cxx_version="$(env -u PKG_CONFIG_LIBDIR PKG_CONFIG_PATH="$GLOBAL_PKG_CONFIG_PATH" \
    "$PKG_CONFIG_BIN" --modversion libndn-cxx)"
  [[ "$cxx_version" == 0.9.0 ]] || {
    echo "Mini-NDN v0.7.0 profile is pinned for ndn-cxx 0.9.0; found '$cxx_version'. Existing ndn-cxx left untouched." >&2
    return 1
  }
}

ensure_minindn_boost_iostreams() {
  local version="$BOOST_VERSION_STRING" sha soname dir lib build archive
  case "$version" in
    1.71.0) sha=d73a8da01e8bf8c7eda40b4c84915071a8c8a0df4a6734537ddde4a8580524ee ;;
    1.85.0) sha=7009fe1faa1697476bdc7027703a2badb84e849b7b0baad5086b087b971f8617 ;;
    *)
      echo "No pinned Boost $version archive; refusing to mix Boost versions" >&2
      return 1
      ;;
  esac
  soname="libboost_iostreams.so.$version"
  for dir in "$BOOST_LIBRARY_DIR" "$GLOBAL_LIBRARY_DIR"; do
    lib="$dir/libboost_iostreams.so"
    if [[ -e "$lib" || -L "$lib" || -e "$dir/$soname" || -L "$dir/$soname" ]]; then
      [[ -e "$lib" && -e "$dir/$soname" ]] &&
        readelf -d "$dir/$soname" 2>/dev/null | grep -Fq "Library soname: [$soname]" || return 1
      echo "==> Reusing Boost $version iostreams from $dir"
      return 0
    fi
  done
  build="$(mktemp -d "$BUILD_DIR/boost-iostreams-$version.XXXXXXXX")"
  archive="$build/boost_${version//./_}.tar.bz2"
  mkdir -p "$build/source" "$build/stage" "$build/install/usr/local/lib"
  run curl --fail --location --retry 3 --output "$archive" \
    "https://archives.boost.io/release/$version/source/${archive##*/}"
  printf '%s  %s\n' "$sha" "$archive" | sha256sum --check --status || return 1
  run tar -xjf "$archive" --strip-components=1 --no-same-owner --no-same-permissions -C "$build/source"
  (
    cd "$build/source"
    run env CC=/usr/bin/gcc CXX=/usr/bin/g++ ./bootstrap.sh --with-libraries=iostreams
    run ./b2 --build-dir="$build/build" --stagedir="$build/stage" --with-iostreams \
      toolset=gcc link=shared runtime-link=shared threading=multi --layout=system "-j$JOBS" stage
  )
  lib="$build/stage/lib/$soname"
  readelf -d "$lib" 2>/dev/null | grep -Fq "Library soname: [$soname]" || return 1
  install -m 0755 "$lib" "$build/install/usr/local/lib/$soname"
  ln -s "$soname" "$build/install/usr/local/lib/libboost_iostreams.so"
  install_manifest_tree "$build/install/usr/local" "$GLOBAL_DEPENDENCY_PREFIX" "boost-iostreams:$version"
  sudo_run /sbin/ldconfig
  rm -rf -- "$build"
}

install_yolo_minindn_profile() {
  local python_tag torch_version torchvision_version python_ndn_version onnxruntime_python_version
  local mini_prefix yolo_prefix
  local mini_source source_copy stage_root mini_site yolo_site mini_stage yolo_stage
  local model_stage topology
  local -a pip_env

  case "$("$PYTHON_BIN" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')" in
    3.8)
      python_tag=python3.8
      torch_version=2.4.1+cpu
      torchvision_version=0.19.1+cpu
      python_ndn_version=0.3
      onnxruntime_python_version=1.18.0
      ;;
    3.14)
      python_tag=python3.14
      torch_version=2.14.0+cpu
      torchvision_version=0.29.0+cpu
      python_ndn_version=0.5.1
      onnxruntime_python_version=1.26.0
      ;;
    *)
      echo "--with-yolo-minindn has pinned CPU wheels for Python 3.8 or 3.14, not $PYTHON_BIN" >&2
      return 2
      ;;
  esac

  check_minindn_ndn_base
  ensure_minindn_boost_iostreams
  build_fixed_minindn_waf_dependency PSync "$MININDN_PSYNC_URL" \
    "$MININDN_PSYNC_COMMIT" 'pkg:PSync >= 0.5.0'
  build_fixed_minindn_waf_dependency NLSR "$MININDN_NLSR_URL" \
    "$MININDN_NLSR_COMMIT" nlsr
  build_fixed_minindn_waf_dependency ndn-tools "$MININDN_TOOLS_URL" \
    "$MININDN_TOOLS_COMMIT" ndnping ndnpingserver
  build_fixed_minindn_waf_dependency ndn-traffic-generator "$MININDN_TRAFFIC_URL" \
    "$MININDN_TRAFFIC_COMMIT" ndn-traffic-client
  build_fixed_minindn_infoedit

  mini_source="$(ensure_fixed_source_tree mini-ndn "$MININDN_REPO_URL" \
    "$MININDN_COMMIT" | tail -n 1)"
  source_copy="$(mktemp -d "$BUILD_DIR/minindn-python-source.XXXXXXXX")"
  mkdir -p -- "$BUILD_DIR/install-staging"
  stage_root="$(mktemp -d "$BUILD_DIR/install-staging/yolo-minindn.XXXXXXXX")"
  mini_prefix="$GLOBAL_LIBRARY_DIR/ndnsf/mini-ndn/v0.7.0/$python_tag"
  mini_site="$mini_prefix/site-packages"
  yolo_prefix="torch-${torch_version//+/-}-ultralytics-$YOLO_ULTRALYTICS_VERSION"
  yolo_site="$GLOBAL_LIBRARY_DIR/ndnsf/yolo-minindn/$python_tag/$yolo_prefix/site-packages"
  mini_stage="$stage_root/usr/local/${mini_site#/usr/local/}"
  yolo_stage="$stage_root/usr/local/${yolo_site#/usr/local/}"
  mkdir -p -- "$mini_stage" "$yolo_stage"
  pip_env=(env -u PYTHONPATH -u PYTHONHOME PYTHONNOUSERSITE=1)

  echo "==> Preparing the pinned Mini-NDN $MININDN_COMMIT Python package"
  git -C "$mini_source" archive --format=tar "$MININDN_COMMIT" |
    tar -xf - -C "$source_copy"
  run "${pip_env[@]}" "$PYTHON_BIN" -m pip --isolated install \
    --no-deps --no-build-isolation --no-compile --target "$mini_stage" "$source_copy"

  echo "==> Installing python-ndn $python_ndn_version for the Mini-NDN experiments"
  run "${pip_env[@]}" "$PYTHON_BIN" -m pip --isolated install \
    --no-cache-dir --no-compile --target "$mini_stage" \
    "python-ndn==$python_ndn_version"

  echo "==> Installing pinned CPU-only PyTorch and Ultralytics Python packages"
  run "${pip_env[@]}" "$PYTHON_BIN" -m pip --isolated install \
    --no-cache-dir --no-compile --only-binary=:all: --ignore-installed \
    --index-url https://download.pytorch.org/whl/cpu \
    --extra-index-url https://pypi.org/simple --target "$yolo_stage" \
    "torch==$torch_version" "torchvision==$torchvision_version" \
    "ultralytics==$YOLO_ULTRALYTICS_VERSION" \
    "onnxruntime==$onnxruntime_python_version"

  model_stage="$stage_root/usr/local/share/ndnsf/models/yolo26n.pt"
  mkdir -p -- "$(dirname -- "$model_stage")" \
    "$stage_root/usr/local/share/ndnsf/yolo-minindn"
  run curl --fail --location --retry 3 --output "$stage_root/yolo26n.pt.download" \
    "$YOLO_MODEL_URL"
  printf '%s  %s\n' "$YOLO_MODEL_SHA256" "$stage_root/yolo26n.pt.download" |
    sha256sum --check --status
  install -m 0644 "$stage_root/yolo26n.pt.download" "$model_stage"
  printf '%s\n' "$ROOT" > "$stage_root/usr/local/share/ndnsf/yolo-minindn/source-root"
  printf '%s\n' "$mini_site" "$yolo_site" \
    > "$stage_root/usr/local/share/ndnsf/yolo-minindn/python-site-paths"
  printf '%s\n' "$YOLO_MODEL_SHA256" \
    > "$stage_root/usr/local/share/ndnsf/yolo-minindn/yolo26n.sha256"

  mkdir -p -- "$stage_root/usr/local/etc/mini-ndn"
  while IFS= read -r -d '' topology; do
    install -m 0644 "$topology" \
      "$stage_root/usr/local/etc/mini-ndn/$(basename -- "$topology")"
  done < <(find "$source_copy/topologies" -type f -name '*.conf' -print0)

  mkdir -p -- "$stage_root/usr/local/bin"
  cat > "$stage_root/usr/local/bin/ndnsf-yolo-minindn" <<'LAUNCHER'
#!/usr/bin/env bash
set -euo pipefail

if (( EUID != 0 )); then
  command -v sudo >/dev/null 2>&1 || { echo 'Mini-NDN needs root; sudo is unavailable' >&2; exit 1; }
  exec sudo -- "$0" "$@"
fi
if (($#)); then
  echo 'Usage: ndnsf-yolo-minindn' >&2
  exit 2
fi

state_dir=/usr/local/share/ndnsf/yolo-minindn
model=/usr/local/share/ndnsf/models/yolo26n.pt
[[ -r "$state_dir/source-root" && -r "$state_dir/python-site-paths" && -r "$state_dir/yolo26n.sha256" ]] || {
  echo "Incomplete Mini-NDN/YOLO install state: $state_dir" >&2
  exit 1
}
repo="${NDNSF_ROOT:-$(<"$state_dir/source-root")}"
repo="$(realpath -e -- "$repo")"
[[ -f "$repo/Experiments/NDNSF_DI_YoloSplit_Minindn.py" ]] || {
  echo "YOLO Mini-NDN experiment not found under $repo; set NDNSF_ROOT" >&2
  exit 1
}
[[ -f "$model" ]] || { echo "Cached YOLO model is missing: $model" >&2; exit 1; }
printf '%s  %s\n' "$(<"$state_dir/yolo26n.sha256")" "$model" | sha256sum --check --status || {
  echo 'Cached YOLO model failed SHA-256 verification' >&2
  exit 1
}
mapfile -t python_sites < "$state_dir/python-site-paths"
[[ ${#python_sites[@]} -eq 2 && -d "${python_sites[0]}" && -d "${python_sites[1]}" ]] || {
  echo 'Mini-NDN/YOLO Python package paths are missing' >&2
  exit 1
}

run_root="${NDNSF_YOLO_RUN_ROOT:-/var/tmp/ndnsf-yolo-minindn}"
mkdir -p -- "$run_root"
run_root="$(realpath -e -- "$run_root")"
case "$run_root" in
  "$repo"|"$repo"/*) echo "Run artifacts must be outside the checkout: $run_root" >&2; exit 2 ;;
esac
run_dir="$(mktemp -d "$run_root/run.$(date -u +%Y%m%dT%H%M%SZ).XXXXXXXX")"
model_cwd="$run_dir/model-cache"
mkdir -p -- "$model_cwd" "$run_dir/tmp" "$run_dir/matplotlib" "$run_dir/torch"
ln -s -- "$model" "$model_cwd/yolo26n.pt"

export PATH="/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export NDNSF_ROOT="$repo"
export NDNSF_YOLO_MODEL="$model"
export NDNSF_YOLO_RUN_DIR="$run_dir"
export NDNSF_YOLO_MODEL_CWD="$model_cwd"
export NDNSF_MININDN_SITE="${python_sites[0]}"
export NDNSF_YOLO_SITE="${python_sites[1]}"
export PYTHONPATH="${python_sites[0]}:${python_sites[1]}"
export PYTHONNOUSERSITE=1 PYTHONPYCACHEPREFIX="$run_dir/pycache"
export TMPDIR="$run_dir/tmp" MPLCONFIGDIR="$run_dir/matplotlib"
export TORCH_HOME="$run_dir/torch" YOLO_CONFIG_DIR="$run_dir/ultralytics-config"
unset DISPLAY GST_PLUGIN_PATH

set -o pipefail
/usr/bin/python3 - 2>&1 <<'PY' | tee "$run_dir/launcher.log"
import os
from pathlib import Path
import sys
import subprocess

repo = Path(os.environ['NDNSF_ROOT']).resolve()
run_dir = Path(os.environ['NDNSF_YOLO_RUN_DIR']).resolve()
model = Path(os.environ['NDNSF_YOLO_MODEL']).resolve()
model_cwd = Path(os.environ['NDNSF_YOLO_MODEL_CWD']).resolve()
mini_site = os.environ['NDNSF_MININDN_SITE']
yolo_site = os.environ['NDNSF_YOLO_SITE']
py_dir = repo / 'examples/python/NDNSF-DistributedInference/yolo_split'
python_path = ':'.join((
    mini_site,
    yolo_site,
    str(repo / 'NDNSF-DistributedInference'),
    str(repo / 'pythonWrapper'),
    str(py_dir),
    '/usr/lib/python3/dist-packages',
))
os.environ['PYTHONPATH'] = python_path
sys.path.insert(0, str(repo / 'Experiments'))

import NDNSF_DI_YoloSplit_Minindn as experiment
import minindn.minindn as minindn_impl

experiment.REPO = repo
two_node_topology = run_dir / 'minindn-two-node.conf'
two_node_topology.write_text(
    '[nodes]\nucla:\nwustl:\n\n'
    '[links]\nucla:wustl delay=1ms bw=1000\n',
    encoding='utf-8',
)
experiment.TOPO = two_node_topology
experiment.PY_DIR = py_dir
experiment.OUT = run_dir / 'experiment'
experiment.CONFIG = experiment.OUT / 'yolo_policy.yaml'
experiment.GEN_POLICY = str(run_dir / 'generated-policy')

# The existing two-stage harness normally uses a third host for controller and
# user roles. Co-locate those roles with Stage 0 so this launcher really creates
# only two Mininet hosts while retaining the unmodified experiment code.
mininet_getitem = minindn_impl.Mininet.__getitem__
def two_node_getitem(net, name):
    return mininet_getitem(net, 'ucla' if name == 'memphis' else name)
minindn_impl.Mininet.__getitem__ = two_node_getitem

minindn_init = experiment.Minindn.__init__
def verify_two_node_topology(self, *args, **kwargs):
    minindn_init(self, *args, **kwargs)
    names = sorted(node.name for node in self.net.hosts)
    if names != ['ucla', 'wustl']:
        raise RuntimeError(f'expected exactly two Mini-NDN hosts, found {names}')
    print(f'MININDN_TWO_NODE_TOPOLOGY_OK nodes={names}', flush=True)
experiment.Minindn.__init__ = verify_two_node_topology

def isolated_python_path():
    return python_path

def isolated_python_cmd(script, argv):
    args = ' '.join([experiment.perf.shell_quote(str(py_dir / script))] +
                    [experiment.perf.shell_quote(str(arg)) for arg in argv])
    return (f'cd {experiment.perf.shell_quote(str(model_cwd))} && '
            f'exec {experiment.perf.shell_quote(sys.executable)} {args}')

def generate_cached_policy():
    experiment.OUT.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PYTHONPATH=python_path)
    subprocess.run([
        sys.executable,
        str(py_dir / 'split_model.py'),
        '--model', str(model),
        '--input-size', '32',
        '--auto-split',
        '--out-dir', str(experiment.OUT / 'model'),
        '--policy', str(experiment.CONFIG),
    ], cwd=str(run_dir), env=env, check=True)

experiment.build_python_path = isolated_python_path
experiment.python_cmd = isolated_python_cmd
experiment.generate_auto_split_policy = generate_cached_policy

# The stock cleanUp calls host-level `nfd-stop` before `mn --clean`. The
# experiment owns this topology; ndn.stop() tears down its exact network.
experiment.Minindn.cleanUp = staticmethod(lambda: None)
parse_args = experiment.Minindn.parseArgs
def isolated_minindn_args(parent):
    parser = parse_args(parent)
    parser.set_defaults(workDir=str(run_dir / 'minindn-work'), resultDir=None)
    return parser
experiment.Minindn.parseArgs = staticmethod(isolated_minindn_args)

print(f'YOLO_MININDN_RUN_DIR={run_dir}', flush=True)
print('YOLO_MININDN_HOST_LAYOUT=ucla(controller,user,stage0),wustl(stage1)', flush=True)
experiment.main()
PY
LAUNCHER
  chmod 0755 "$stage_root/usr/local/bin/ndnsf-yolo-minindn"

  echo '==> Installing Mini-NDN runtime, YOLO CPU packages, model cache, and launcher'
  preserve_existing_stage_targets "$stage_root/usr/local"
  install_manifest_tree "$stage_root/usr/local" "$GLOBAL_DEPENDENCY_PREFIX" \
    yolo-minindn-runtime
  run env -u PYTHONHOME PYTHONNOUSERSITE=1 PYTHONPATH="$mini_site:$yolo_site" \
    MPLCONFIGDIR="$TMPDIR/matplotlib" YOLO_CONFIG_DIR="$TMPDIR/ultralytics" \
    "$PYTHON_BIN" - "$YOLO_ULTRALYTICS_VERSION" "$torch_version" \
    "$torchvision_version" "$onnxruntime_python_version" <<'PY'
import importlib.metadata
import onnxruntime
import torch
import torchvision
import ultralytics
from minindn.minindn import Minindn
from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
import mininet
import igraph
from ndn.encoding import Name, parse_data

expected_ultralytics, expected_torch, expected_torchvision, expected_onnxruntime = __import__('sys').argv[1:]
assert importlib.metadata.version('ultralytics') == expected_ultralytics
assert onnxruntime.__version__ == expected_onnxruntime, onnxruntime.__version__
assert 'CPUExecutionProvider' in onnxruntime.get_available_providers()
assert torch.__version__ == expected_torch, torch.__version__
assert torchvision.__version__ == expected_torchvision, torchvision.__version__
assert torch.version.cuda is None, torch.version.cuda
assert Minindn and NdnRoutingHelper and mininet and igraph and ultralytics
assert Name and parse_data
print('MININDN_YOLO_PYTHON_IMPORTS_OK')
PY
  printf '%s  %s\n' "$YOLO_MODEL_SHA256" \
    "$GLOBAL_DEPENDENCY_PREFIX/share/ndnsf/models/yolo26n.pt" | sha256sum --check --status
  sudo_run ldconfig
  rm -rf -- "$source_copy" "$stage_root"
  echo '==> Mini-NDN/YOLO profile installed; start a real two-stage run with ndnsf-yolo-minindn'
}

build_cmake_dependency() {
  local name="$1"
  local pkg="$2"
  local url="$3"
  local minimum="$4"
  local dir build_dir install_stage

  if [[ "$FORCE_DEPENDENCIES" != "1" ]] && is_pkg_installed "$pkg" "$minimum"; then
    echo "==> $name already installed ($pkg); skipping"
    return
  fi

  dir="$(ensure_source_tree "$name" "$url" | tail -n 1)"
  build_dir="$dir/.ndnsf-global-build-${name//[^A-Za-z0-9]/_}-$$"
  install_stage="$BUILD_DIR/install-staging/$name/usr/local"
  mkdir -p -- "$install_stage"
  echo "==> Building dependency $name"
  if [[ "$name" == "NAC-ABE" && -n "$OPENABE_PREFIX" && -f "$OPENABE_PREFIX/lib/libopenabe.so" ]]; then
    run bash -e -c "cd \"\$1\" && env \
      -u PKG_CONFIG_LIBDIR -u BOOST_ROOT -u BOOST_INCLUDEDIR -u BOOST_LIBRARYDIR \
      PKG_CONFIG_PATH='$GLOBAL_PKG_CONFIG_PATH' \
      PATH='$SYSTEM_PATH' CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld \
      AR=/usr/bin/ar RANLIB=/usr/bin/ranlib \
      CMAKE_PREFIX_PATH='$OPENABE_PREFIX' \
      CMAKE_INCLUDE_PATH='$OPENABE_PREFIX/include' \
      CMAKE_LIBRARY_PATH='$OPENABE_PREFIX/lib' \
      CPPFLAGS='-I$OPENABE_PREFIX/include' \
      CXXFLAGS='-I$OPENABE_PREFIX/include' \
      LDFLAGS='-L$OPENABE_PREFIX/lib -Wl,-rpath,$OPENABE_PREFIX/lib' \
      LD_LIBRARY_PATH='$OPENABE_PREFIX/lib' \
      cmake -S . -B \"\$2\" \
        -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
        -DCMAKE_LINKER=/usr/bin/ld -DCMAKE_AR=/usr/bin/ar -DCMAKE_RANLIB=/usr/bin/ranlib \
        -DCMAKE_INSTALL_PREFIX='$GLOBAL_DEPENDENCY_PREFIX' \
        -DBoost_NO_BOOST_CMAKE=ON -DBoost_NO_SYSTEM_PATHS=ON \
        -DBOOST_INCLUDEDIR='$BOOST_INCLUDE_DIR' -DBOOST_LIBRARYDIR='$BOOST_LIBRARY_DIR' \
        -DCMAKE_BUILD_RPATH='$OPENABE_PREFIX/lib' \
        -DCMAKE_INSTALL_RPATH='$OPENABE_PREFIX/lib' && \
      env -u PKG_CONFIG_PATH -u CMAKE_PREFIX_PATH -u CMAKE_INCLUDE_PATH \
        -u CMAKE_LIBRARY_PATH -u CMAKE_FRAMEWORK_PATH -u CMAKE_APPBUNDLE_PATH \
        -u CXXFLAGS -u CFLAGS -u CPPFLAGS -u LDFLAGS -u LD_LIBRARY_PATH \
        -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH \
        -u BOOST_ROOT -u BOOST_INCLUDEDIR -u BOOST_LIBRARYDIR \
        PATH='$SYSTEM_PATH' CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld \
        AR=/usr/bin/ar RANLIB=/usr/bin/ranlib \
        cmake --build \"\$2\" --parallel '$JOBS'" _ "$dir" "$build_dir"
  else
    run bash -e -c "cd \"\$1\" && env \
      -u PKG_CONFIG_PATH -u CMAKE_PREFIX_PATH -u CMAKE_INCLUDE_PATH -u CMAKE_LIBRARY_PATH \
      -u CMAKE_FRAMEWORK_PATH -u CMAKE_APPBUNDLE_PATH -u CXXFLAGS \
      -u CFLAGS -u CPPFLAGS -u LDFLAGS -u LD_LIBRARY_PATH \
      -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH \
      -u BOOST_ROOT -u BOOST_INCLUDEDIR -u BOOST_LIBRARYDIR \
      PATH='$SYSTEM_PATH' CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld \
      AR=/usr/bin/ar RANLIB=/usr/bin/ranlib \
      cmake -S . -B \"\$2\" \
      -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
      -DCMAKE_LINKER=/usr/bin/ld -DCMAKE_AR=/usr/bin/ar -DCMAKE_RANLIB=/usr/bin/ranlib \
      -DCMAKE_INSTALL_PREFIX='$GLOBAL_DEPENDENCY_PREFIX' \
      -DBoost_NO_BOOST_CMAKE=ON -DBoost_NO_SYSTEM_PATHS=ON \
      -DBOOST_INCLUDEDIR='$BOOST_INCLUDE_DIR' -DBOOST_LIBRARYDIR='$BOOST_LIBRARY_DIR' && \
      env -u PKG_CONFIG_PATH -u CMAKE_PREFIX_PATH -u CMAKE_INCLUDE_PATH \
        -u CMAKE_LIBRARY_PATH -u CMAKE_FRAMEWORK_PATH -u CMAKE_APPBUNDLE_PATH \
        -u CXXFLAGS -u CFLAGS -u CPPFLAGS -u LDFLAGS -u LD_LIBRARY_PATH \
        -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH \
        -u BOOST_ROOT -u BOOST_INCLUDEDIR -u BOOST_LIBRARYDIR \
        PATH='$SYSTEM_PATH' CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld \
        AR=/usr/bin/ar RANLIB=/usr/bin/ranlib \
        cmake --build \"\$2\" --parallel '$JOBS'" _ "$dir" "$build_dir"
  fi
  echo "==> Installing dependency $name"
  run bash -e -c "cd \"\$1\" && env -u BOOST_ROOT -u BOOST_INCLUDEDIR \
    -u BOOST_LIBRARYDIR PATH='$SYSTEM_PATH' CC=/usr/bin/gcc CXX=/usr/bin/g++ \
    LD=/usr/bin/ld AR=/usr/bin/ar RANLIB=/usr/bin/ranlib \
    cmake --install \"\$2\" --prefix \"\$3\"" _ "$dir" "$build_dir" "$install_stage"
  install_manifest_tree "$install_stage" "$GLOBAL_DEPENDENCY_PREFIX" "$name"
  rm -rf "$build_dir"
  rm -rf -- "$BUILD_DIR/install-staging/$name"
  sudo_run ldconfig
  record_source_receipt "$name"
}

has_onnxruntime_sdk() {
  has_global_sdk_pkg onnxruntime 1.26.0 libonnxruntime.so || return 1
  local includedir
  includedir="$(pkg_config --variable=includedir onnxruntime 2>/dev/null)" || return 1
  [[ -f "$includedir/onnxruntime_c_api.h" && -f "$includedir/onnxruntime_cxx_api.h" ]]
}

has_onnx_full_proto_sdk() {
  local config="$GLOBAL_ONNX_PREFIX/lib/cmake/ONNX/ONNXConfigVersion.cmake"
  [[ -f "$config" && -f "$GLOBAL_ONNX_PREFIX/include/onnx/checker.h" && \
     -f "$GLOBAL_ONNX_PREFIX/include/onnx/shape_inference/implementation.h" && \
     -f "$GLOBAL_ONNX_PREFIX/lib/libonnx.a" && \
     -f "$GLOBAL_ONNX_PREFIX/lib/libonnx_proto.a" ]] || return 1
  /usr/bin/grep -Fq 'set(PACKAGE_VERSION "1.17.0")' "$config" || return 1
  /usr/bin/ar t "$GLOBAL_ONNX_PREFIX/lib/libonnx.a" >/dev/null 2>&1 &&
    /usr/bin/ar t "$GLOBAL_ONNX_PREFIX/lib/libonnx_proto.a" >/dev/null 2>&1
}

has_global_tokenizer_bridge() {
  local archive="$GLOBAL_LIBRARY_DIR/libndnsf_tokenizer_bridge.a" symbols symbol
  [[ -f "$archive" ]] || return 1
  symbols="$(/usr/bin/nm -g --defined-only "$archive" 2>/dev/null)" || return 1
  for symbol in ndi_token_create ndi_token_encode ndi_token_decode \
      ndi_token_decode_stable ndi_token_free ndi_token_destroy; do
    /usr/bin/grep -Eq "[[:space:]]${symbol}$" <<< "$symbols" || return 1
  done
}

install_host_sdk_prerequisites() {
  local ort_version ort_url ort_sha ort_bytes ort_prefix
  local onnx_version onnx_url onnx_ref onnx_commit onnx_prefix
  local crate rust_minimum bridge_path workspace archive extracted pc_file
  local stage build_dir actual_commit rust_version bridge_stage
  ort_version="$(host_sdk_lock_field onnxRuntimeCpp.version)"
  ort_url="$(host_sdk_lock_field onnxRuntimeCpp.url)"
  ort_sha="$(host_sdk_lock_field onnxRuntimeCpp.sha256)"
  ort_bytes="$(host_sdk_lock_field onnxRuntimeCpp.bytes)"
  ort_prefix="$(host_sdk_lock_field onnxRuntimeCpp.installPrefix)"
  onnx_version="$(host_sdk_lock_field onnxFullProto.version)"
  onnx_url="$(host_sdk_lock_field onnxFullProto.url)"
  onnx_ref="$(host_sdk_lock_field onnxFullProto.ref)"
  onnx_commit="$(host_sdk_lock_field onnxFullProto.commit)"
  onnx_prefix="$(host_sdk_lock_field onnxFullProto.installPrefix)"
  crate="$(host_sdk_lock_field tokenizerBridge.crate)"
  rust_minimum="$(host_sdk_lock_field tokenizerBridge.rustMinimum)"
  bridge_path="$(host_sdk_lock_field tokenizerBridge.installPath)"
  workspace="$BUILD_DIR/host-sdk-work"
  mkdir -p -- "$workspace"

  if has_onnxruntime_sdk; then
    echo "==> ONNX Runtime SDK $(pkg_config --modversion onnxruntime) already installed; reusing"
  else
    if [[ -e "$ort_prefix" || -L "$ort_prefix" ]]; then
      echo "Existing ONNX Runtime prefix is incomplete or incompatible; refusing to overwrite: $ort_prefix" >&2
      return 1
    fi
    archive="$workspace/onnxruntime-${ort_version}-linux-x64.tgz"
    extracted="$workspace/onnxruntime-${ort_version}"
    stage="$workspace/onnxruntime-stage"
    run wget -O "$archive" "$ort_url"
    [[ "$(/usr/bin/stat -c '%s' "$archive")" == "$ort_bytes" ]] || {
      echo "ONNX Runtime archive size does not match the host SDK lock" >&2
      return 1
    }
    printf '%s  %s\n' "$ort_sha" "$archive" | /usr/bin/sha256sum --check --status || {
      echo "ONNX Runtime archive SHA-256 does not match the host SDK lock" >&2
      return 1
    }
    mkdir -p -- "$extracted" "$stage"
    run tar -xzf "$archive" --strip-components=1 -C "$extracted"
    [[ -f "$extracted/include/onnxruntime_c_api.h" && \
       -f "$extracted/lib/libonnxruntime.so" ]] || {
      echo "Downloaded ONNX Runtime archive is missing required C++ SDK files" >&2
      return 1
    }
    cp -a "$extracted/." "$stage/"
    mkdir -p -- "$stage/lib/pkgconfig"
    pc_file="$stage/lib/pkgconfig/onnxruntime.pc"
    printf '%s\n' \
      "prefix=$ort_prefix" \
      'exec_prefix=${prefix}' \
      'libdir=${exec_prefix}/lib' \
      'includedir=${prefix}/include' \
      'Name: onnxruntime' \
      'Description: ONNX Runtime CPU C++ SDK' \
      "Version: $ort_version" \
      'Libs: -L${libdir} -Wl,-rpath,${libdir} -lonnxruntime' \
      'Cflags: -I${includedir}' > "$pc_file"
    install_manifest_tree "$stage" "$ort_prefix" "onnxruntime-sdk:$ort_version"
    rm -rf -- "$archive" "$extracted" "$stage"
    echo "==> Installed locked ONNX Runtime SDK $ort_version at $ort_prefix"
  fi

  if has_onnx_full_proto_sdk; then
    echo "==> ONNX full-protobuf SDK $onnx_version already installed; reusing"
  else
    if [[ -e "$onnx_prefix" || -L "$onnx_prefix" ]]; then
      echo "Existing ONNX prefix is incomplete or not version $onnx_version; refusing to overwrite: $onnx_prefix" >&2
      return 1
    fi
    local source_dir="$workspace/onnx-$onnx_version-source"
    local install_stage="$workspace/onnx-$onnx_version-stage"
    build_dir="$workspace/onnx-$onnx_version-build"
    run git clone --depth=1 --no-tags --branch "$onnx_ref" "$onnx_url" "$source_dir"
    actual_commit="$(git -C "$source_dir" rev-parse HEAD)"
    [[ "$actual_commit" == "$onnx_commit" ]] || {
      echo "ONNX source commit $actual_commit differs from the host SDK lock $onnx_commit" >&2
      return 1
    }
    [[ "$(<"$source_dir/VERSION_NUMBER")" == "$onnx_version" ]] || {
      echo "ONNX source version does not match the host SDK lock" >&2
      return 1
    }
    record_source_checkout "onnx-full-proto" "$onnx_url" "$onnx_commit" \
      "$source_dir" "$workspace"
    mkdir -p -- "$install_stage"
    run env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
      -u CMAKE_PREFIX_PATH -u CMAKE_INCLUDE_PATH -u CMAKE_LIBRARY_PATH \
      PATH="$SYSTEM_PATH" CC=/usr/bin/gcc CXX=/usr/bin/g++ \
      cmake -S "$source_dir" -B "$build_dir" \
        -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$install_stage" \
        -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_ONNX_PYTHON=OFF \
        -DONNX_BUILD_TESTS=OFF -DONNX_GEN_PB_TYPE_STUBS=OFF \
        -DONNX_USE_LITE_PROTO=OFF
    run cmake --build "$build_dir" --parallel "$JOBS"
    run cmake --install "$build_dir" --prefix "$install_stage"
    local staged_config="$install_stage/lib/cmake/ONNX/ONNXConfigVersion.cmake"
    [[ -f "$staged_config" && -f "$install_stage/include/onnx/checker.h" && \
       -f "$install_stage/include/onnx/shape_inference/implementation.h" && \
       -f "$install_stage/lib/libonnx.a" && \
       -f "$install_stage/lib/libonnx_proto.a" ]] || {
      echo "ONNX build did not stage the full-protobuf C++ SDK" >&2
      return 1
    }
    install_manifest_tree "$install_stage" "$onnx_prefix" "onnx-full-proto-sdk:$onnx_version"
    rm -rf -- "$install_stage" "$build_dir"
    echo "==> Installed locked ONNX full-protobuf SDK $onnx_version at $onnx_prefix"
  fi

  if has_global_tokenizer_bridge; then
    echo "==> Rust tokenizer bridge already installed; reusing"
  else
    if [[ -e "$bridge_path" || -L "$bridge_path" ]]; then
      echo "Existing tokenizer bridge archive is incompatible; refusing to overwrite: $bridge_path" >&2
      return 1
    fi
    command -v cargo >/dev/null && command -v rustc >/dev/null || {
      echo "Rust/Cargo are required to build the locked tokenizer bridge" >&2
      return 1
    }
    rust_version="$(rustc --version | /usr/bin/awk '{print $2}')"
    if [[ "$(printf '%s\n' "$rust_minimum" "$rust_version" | /usr/bin/sort -V | /usr/bin/head -n 1)" != "$rust_minimum" ]]; then
      echo "Rust $rust_version is older than the tokenizer bridge requirement $rust_minimum" >&2
      return 1
    fi
    run env CARGO_HOME="$workspace/cargo-home" \
      CARGO_TARGET_DIR="$workspace/cargo-target" \
      PATH="$SYSTEM_PATH" cargo build --locked --release \
        --manifest-path "$ROOT/$crate/Cargo.toml"
    archive="$workspace/cargo-target/release/libndnsf_tokenizer_bridge.a"
    [[ -f "$archive" ]] || {
      echo "Cargo did not produce the locked tokenizer static archive" >&2
      return 1
    }
    bridge_stage="$workspace/tokenizer-bridge-stage"
    mkdir -p -- "$bridge_stage/lib"
    install -m 0644 "$archive" "$bridge_stage/lib/$(basename -- "$bridge_path")"
    install_manifest_tree "$bridge_stage" "$GLOBAL_DEPENDENCY_PREFIX" tokenizer-bridge
    rm -rf -- "$bridge_stage"
    has_global_tokenizer_bridge || {
      echo "Installed tokenizer bridge is missing expected C ABI symbols" >&2
      return 1
    }
    echo "==> Installed locked Rust tokenizer bridge at $bridge_path"
  fi
}

install_external_dependencies() {
  echo "==> Checking external NDN dependencies"
  echo "==> Dependency source directory: $DEPS_DIR"
  # SDKs are prepared from the dedicated version/hash lock before this gate.
  check_waf_inventory --sdk-only
  require_global_sdk_pkg "onnxruntime" "1.26.0" "libonnxruntime.so"
  require_global_file "$GLOBAL_ONNX_PREFIX/lib/libonnx.a"
  require_global_file "$GLOBAL_ONNX_PREFIX/lib/libonnx_proto.a"
  require_global_file "$GLOBAL_LIBRARY_DIR/libndnsf_tokenizer_bridge.a"
  require_global_file "$GLOBAL_ONNX_PREFIX/include/onnx/checker.h"
  require_global_file "$GLOBAL_ONNX_PREFIX/include/onnx/shape_inference/implementation.h"
  if [[ -f "$GLOBAL_IDENTITY_FILE" ]] && ! has_global_dependency_identity; then
    if ! has_global_onnx_identity; then
      echo "Global ONNX Runtime identity changed; this installer cannot rebuild ONNX Runtime. Reinstall the canonical global SDK and refresh the identity receipt before continuing." >&2
      exit 1
    fi
    echo "==> Global identity is stale; individual package probes determine what must be rebuilt"
  fi
  require_global_boost
  build_waf_dependency "ndn-cxx" "libndn-cxx" "$NDNCXX_REPO_URL" "$MIN_NDNCXX_VERSION"
  build_waf_dependency "ndn-svs" "libndn-svs" "$NDNSVS_REPO_URL" "$MIN_NDNSVS_VERSION"
  build_waf_dependency "NDNSD" "ndnsd" "$NDNSD_REPO_URL" "$MIN_NDNSD_VERSION"
  build_openabe_dependency
  build_cmake_dependency "NAC-ABE" "libnac-abe" "$NACABE_REPO_URL" "$MIN_NACABE_VERSION"
  write_global_dependency_identity
  require_global_external_closure
  check_waf_inventory
}

check_qwen_minindn_prerequisites() {
  local tool missing=0
  for tool in nfd nfdc ndnsec mn; do
    if ! PATH="$SYSTEM_PATH:/usr/local/bin" command -v "$tool" >/dev/null; then
      echo "Qwen MiniNDN prerequisite missing: $tool (install the owning project first)" >&2
      missing=1
    fi
  done
  if ! (cd / && env -u PYTHONPATH -u PYTHONHOME "$PYTHON_BIN" -I -c \
    'from minindn.minindn import Minindn; from minindn.helpers.ndn_routing_helper import NdnRoutingHelper; import mininet; import cryptography; import ndn.encoding'); then
    echo 'Qwen MiniNDN requires MiniNDN and its Python dependencies installed for the system Python.' >&2
    missing=1
  fi
  (( missing == 0 ))
}

check_qwen_installed_programs() {
  local program
  for program in bin/App_ServiceController bin/DI_NativeArtifactAuthority bin/DI_NativeRequester \
      bin/di-native-provider libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker bin/spec190-multiturn-oracle; do
    [[ -f "$GLOBAL_DEPENDENCY_PREFIX/$program" && -x "$GLOBAL_DEPENDENCY_PREFIX/$program" ]] || {
      echo "Missing installed Qwen executable: $GLOBAL_DEPENDENCY_PREFIX/$program" >&2
      return 1
    }
  done
  echo 'Qwen executables installed; this is NOT an inference PASS.'
  echo 'Next: prepare canonical ONNX/KV model artifacts and run LocalExperiment check with an installed-binary profile.'
  echo 'See docs/unified-stack-install.md#qwen-minindn-readiness for model and qualification boundaries.'
}

cd "$ROOT"

# One public entry point; configure.sh only translates legacy arguments.
if (( UNINSTALL_STACK )); then
  if [[ "$(readlink -f "$PYTHON_BIN")" != "$(readlink -f /usr/bin/python3)" ]]; then
    echo '--uninstall requires the system /usr/bin/python3 used by pip installation' >&2
    exit 2
  fi
  PYTHON_BIN=/usr/bin/python3
  uninstall_stack
  exit $?
fi

if (( PLAN_ONLY )); then
  if (( CONFIGURE_ONLY )); then
    echo "Mode: configure-only; OS package installation=$INSTALL_SYSTEM_PACKAGES"
  else
    echo "Mode: stack; source installation=$INSTALL_DEPENDENCIES; deps-only=$DEPS_ONLY; check-only=$CHECK_DEPENDENCIES"
    "$PYTHON_BIN" "$SOURCE_HELPER" plan --lock "$LOCK_FILE"
    echo "Install prefix: $GLOBAL_DEPENDENCY_PREFIX; source directory: $DEPS_DIR"
  fi
  echo 'The full install reuses or installs the host SDKs pinned in packaging/host-sdk-dependencies.lock.json.'
  echo 'Configure-only mode requires ONNX Runtime 1.26+, ONNX 1.17 full-protobuf, and the tokenizer bridge to be present.'
  echo 'MiniNDN/NFD/NLSR dependency flags and their source-build scope:'
  if (( INSTALL_MININDN_PACKAGES )); then
    echo '  Mini-NDN profile: Mininet/OVS and Python prerequisites; NFD/ndn-cxx are reused.'
  fi
  if (( INSTALL_YOLO_MININDN )); then
    echo "  Mini-NDN: $MININDN_REPO_URL @ $MININDN_COMMIT"
    echo "  PSync: $MININDN_PSYNC_URL @ $MININDN_PSYNC_COMMIT"
    echo "  NLSR: $MININDN_NLSR_URL @ $MININDN_NLSR_COMMIT"
    echo "  ndn-tools: $MININDN_TOOLS_URL @ $MININDN_TOOLS_COMMIT"
    echo "  Traffic generator: $MININDN_TRAFFIC_URL @ $MININDN_TRAFFIC_COMMIT"
    echo "  infoedit: $MININDN_INFOEDIT_URL @ $MININDN_INFOEDIT_COMMIT"
    echo '  NFD, ndn-cxx, and ndnsec are reused, never rebuilt by this profile.'
    echo '  MiniNet/OVS APT packages: mininet openvswitch-switch tcpdump iproute2 net-tools python3-pyroute2 python3-networkx python3-matplotlib python3-igraph python3-tqdm python3-joblib.'
    echo '  NDN source-build APT packages: libsystemd-dev libcap-dev libprotobuf-dev protobuf-compiler libsqlite3-dev libpcap-dev libsodium-dev.'
    python_minor="$("$PYTHON_BIN" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
    case "$python_minor" in
      3.8) torch_plan='2.4.1+cpu / torchvision 0.19.1+cpu' ;;
      3.14) torch_plan='2.14.0+cpu / torchvision 0.29.0+cpu' ;;
      *) torch_plan="unsupported Python $python_minor" ;;
    esac
  echo "  Python: $torch_plan; Ultralytics=$YOLO_ULTRALYTICS_VERSION."
    echo "  Model: yolo26n.pt SHA256=$YOLO_MODEL_SHA256."
    echo '  Launcher: ndnsf-yolo-minindn; per-run outputs/logs go under /var/tmp/ndnsf-yolo-minindn.'
    echo '  Inference topology: exactly two Mini-NDN hosts; controller/user share ucla with stage 0, and stage 1 runs on wustl.'
  fi
  if (( QWEN_MININDN )); then
    echo 'Qwen MiniNDN: precheck network tools/Python; install examples and fixtures; check installed programs.'
    echo 'Models are separate inputs: no download, export, quantization, or inference is performed.'
  fi
  printf 'Waf configure options:'; printf ' %q' "${WAF_CONFIGURE_ARGS[@]}"; printf '\n'
  echo 'Offline plan only: no apt, git fetch, builds, installation or Waf execution.'
  exit 0
fi

if (( CONFIGURE_ONLY )); then
  require_host_profile
  export PATH="$SYSTEM_PATH:/usr/local/bin:$PATH"
  install_common_system_packages
  require_system_toolchain
  prepare_build_dir
  run_waf_clean configure "${WAF_CONFIGURE_ARGS[@]}"
  exit 0
fi

# Resolve every source before any installation. URL-only overrides cannot
# silently change the pinned build identity.
for pair in 'ndn-cxx:NDNCXX_REPO_URL' 'ndn-svs:NDNSVS_REPO_URL' 'NDNSD:NDNSD_REPO_URL' 'openabe:OPENABE_REPO_URL' 'NAC-ABE:NACABE_REPO_URL'; do
  name="${pair%%:*}"; variable="${pair#*:}"
  locked_url="$(source_field "$name" url)"
  if env | /usr/bin/grep -q "^${variable}=" && [[ "${!variable}" != "$locked_url" ]]; then
    echo "$variable differs from --lock-file; update the lock instead" >&2
    exit 2
  fi
  printf -v "$variable" '%s' "$locked_url"
done

echo "==> NDNSF stack install root: $ROOT"
echo "==> Python: $PYTHON_BIN"
require_host_profile
if (( QWEN_MININDN )); then
  check_qwen_minindn_prerequisites
fi
if [[ "$CHECK_DEPENDENCIES" != "1" ]]; then
  install_common_system_packages
fi
require_system_toolchain

if [[ "$CHECK_DEPENDENCIES" == "1" ]]; then
  check_waf_inventory
  require_global_external_closure
  echo "==> Installed NDNSF global dependency closure is valid"
  exit 0
fi

prepare_build_dir
if [[ "$INSTALL_DEPENDENCIES" != "0" ]]; then
  install_host_sdk_prerequisites
fi

if [[ "$INSTALL_DEPENDENCIES" == "auto" ]]; then
  # Match Mini-NDN's default resolver shape: each dependency's installed
  # package/version/path/ABI detector decides whether its pinned build is
  # needed. Whole-closure hashes must not turn a compatible installed package
  # into a source rebuild.
  INSTALL_DEPENDENCIES=1
fi

if [[ "$INSTALL_DEPENDENCIES" == "1" ]]; then
  install_external_dependencies
else
  echo "==> Skipping external dependency installation"
  # --no-dependencies skips external source builds and requires a complete
  # preinstalled closure. The common OS build-package bootstrap above remains
  # independently controlled by --no-system-packages.
  require_global_external_closure
  check_waf_inventory
fi

if (( INSTALL_YOLO_MININDN )); then
  install_yolo_minindn_profile
fi

if (( DEPS_ONLY )); then
  echo '==> External dependency stage complete; NDNSF build/install not run'
  exit 0
fi

if [[ "$RUN_WAF_CONFIGURE" == "auto" ]]; then
  # Never infer that an existing cache is safe.  Reconfigure so Waf rechecks
  # every package, compiler and linker path against the installed closure.
  RUN_WAF_CONFIGURE=1
fi

if [[ "$RUN_WAF_CONFIGURE" == "0" ]]; then
  echo "--no-configure is not allowed for the global-closure installer; rerun with configure enabled" >&2
  exit 2
fi

if [[ "$RUN_WAF_CONFIGURE" == "1" ]]; then
  echo "==> Configuring waf project"
  run_waf_clean configure "${WAF_CONFIGURE_ARGS[@]}"
else
  echo "==> Skipping waf configure"
fi

echo "==> Building C++ libraries and bundled subprojects"
run_waf_clean -j"$JOBS"

if [[ "$RUN_SYSTEM_INSTALL" == "1" ]]; then
  echo "==> Installing C++ libraries and headers"
  local_install_stage="$BUILD_DIR/install-staging/ndnsf/usr/local"
  mkdir -p -- "$local_install_stage"
  run env \
    -u PKG_CONFIG_LIBDIR \
    -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
    -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH \
    -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH -u LDSHARED -u WAFDIR \
    -u PKGCONFIG -u LD -u AR -u AS -u RANLIB -u NM -u STRIP \
    -u OBJCOPY -u OBJDUMP -u READELF \
    -u BOOST_ROOT -u BOOST_INCLUDEDIR -u BOOST_LIBRARYDIR \
    PATH="$SYSTEM_PATH" PKGCONFIG="$PKG_CONFIG_BIN" \
    CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld \
    AR=/usr/bin/ar AS=/usr/bin/as RANLIB=/usr/bin/ranlib \
    NM=/usr/bin/nm STRIP=/usr/bin/strip \
    NDNSF_SKIP_DEV_PIP_INSTALL=1 \
    PKG_CONFIG_PATH="$GLOBAL_PKG_CONFIG_PATH" \
    NDNSF_LIBRARY_DIR="$GLOBAL_LIBRARY_DIR" \
    "$PYTHON_BIN" ./waf --out="$BUILD_DIR" --onnx-prefix="$GLOBAL_ONNX_PREFIX" \
    install "--destdir=$BUILD_DIR/install-staging/ndnsf" -j"$JOBS"
  install_manifest_tree "$local_install_stage" "$GLOBAL_DEPENDENCY_PREFIX" ndnsf-native
  rm -rf -- "$BUILD_DIR/install-staging/ndnsf"
  sudo_run /sbin/ldconfig
  require_global_file "$GLOBAL_LIBRARY_DIR/libndn-service-framework.so"
  require_global_file "$GLOBAL_LIBRARY_DIR/libndnsf-distributed-inference.so"
else
  echo "==> Skipping system C++ install"
  echo "==> Build-only mode stops before Python bindings because the global Core/DI install is required" >&2
  exit 2
fi

echo "==> Installing ndnsf Python wrapper"
export NDNSF_LIBRARY_DIR="$GLOBAL_LIBRARY_DIR"
NDNSF_GLOBAL_NATIVE_DIGESTS="$(native_digest_receipt)"
export NDNSF_GLOBAL_NATIVE_DIGESTS
unset NDNSF_RUNTIME_RPATH NDNSF_NDN_SVS_SOURCE_TREE NDNSF_NDN_SVS_BUILD_TREE
# Resolve repository-owned packages together so pip cannot substitute an
# index copy of ndnsf or of a split DI owner package.
pip_install "$ROOT/pythonWrapper" "$ROOT/NDNSF-DistributedRepo/pythonWrapper" \
  "$ROOT/NDNSF-DistributedInference/packaging/python/core" \
  "$ROOT/NDNSF-DistributedInference/packaging/python/sdk" \
  "$ROOT/NDNSF-DistributedInference/packaging/python/planner" \
  "$ROOT/NDNSF-DistributedInference/packaging/python/app" \
  "$ROOT/NDNSF-DistributedInference/packaging/python/ops" \
  "$ROOT/NDNSF-DistributedInference/packaging/python/compat"

echo "==> Running Python import smoke checks"
run env -u PYTHONPATH -u PYTHONHOME PYTHONNOUSERSITE=1 "$PYTHON_BIN" -I - <<'PY'
import ndnsf
import py_repoclient
import ndnsf_distributed_inference
from pathlib import Path
for module in (ndnsf, py_repoclient, ndnsf_distributed_inference):
    path = Path(module.__file__).resolve()
    assert str(path).startswith('/usr/local/'), (module.__name__, path)

manifest = py_repoclient.make_manifest(
    "/NDNSF/InstallSmoke/Object",
    "blob",
    b"install-smoke",
    1,
    [],
    "/Policy/install-smoke/v1",
)
assert manifest.sha256
assert ndnsf_distributed_inference.GenericRepoClient is py_repoclient.RepoClient
print("NDNSF_STACK_INSTALL_SMOKE_OK")
PY

if (( QWEN_MININDN )); then
  check_qwen_installed_programs
fi
echo "==> NDNSF stack installation complete"
