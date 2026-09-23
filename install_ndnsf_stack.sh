#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_BIN="${PYTHON:-python3}"
JOBS="${NDNSF_BUILD_JOBS:-4}"
WAF_CONFIGURE_ARGS=()
RUN_WAF_CONFIGURE=auto
RUN_SYSTEM_INSTALL=1
USE_USER_FLAG=auto
INSTALL_EDITABLE=1
INSTALL_DEPENDENCIES=auto
FORCE_DEPENDENCIES=0
DEPS_DIR="$ROOT/dependencies"
INSTALL_SYSTEM_PACKAGES=1
INSTALL_TEST_PACKAGES=0
INSTALL_MININDN_PACKAGES=0
INSTALL_NFD_NLSR_PACKAGES=0
CHECK_DEPENDENCIES=0
# Every host-side NDNSF dependency is installed into one canonical prefix.
# The source checkout below is only a build input; it must never become a
# runtime or pkg-config dependency path.
GLOBAL_DEPENDENCY_PREFIX="/usr/local"
OPENABE_PREFIX="$GLOBAL_DEPENDENCY_PREFIX"
GLOBAL_LIBRARY_DIR="$GLOBAL_DEPENDENCY_PREFIX/lib"
SYSTEM_PATH="/usr/bin:/bin:/usr/sbin:/sbin"
PKG_CONFIG_BIN="/usr/bin/pkg-config"
# Host SDKs may have a versioned global prefix, but they must still be
# installed outside the repository and visible to every consumer.  ONNX
# Runtime is the only NDNSF dependency currently using such a versioned SDK;
# all NDN/NDNSF outputs remain in /usr/local.
GLOBAL_SDK_ROOTS=("/usr" "/usr/local" "/opt/onnxruntime" "/opt/onnxruntime-1.26.0")
BOOST_INCLUDE_DIR="/usr/include"
BOOST_LIBRARY_DIR="/usr/lib/x86_64-linux-gnu"
BOOST_VERSION_NUMBER="107100"
GLOBAL_IDENTITY_DIR="/usr/local/share/ndnsf"
GLOBAL_IDENTITY_FILE="$GLOBAL_IDENTITY_DIR/global-dependency-identity.json"

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
OPENABE_REPO_URL="${OPENABE_REPO_URL:-https://github.com/zeutro/openabe.git}"

usage() {
  cat <<'EOF'
Usage: ./install_ndnsf_stack.sh [options]

Build and install the NDNSF stack in dependency order:

  1. Missing external NDN dependencies from matianxing1992 GitHub repos
  2. NDNSF-owned C++ core, Repo/DI modules, examples, and native tests through
     the repository waf; external dependencies use their own build entrypoints
  3. ndnsf Python wrapper
  4. py_repoclient Python binding for NDNSF-DistributedRepo
  5. ndnsf-distributed-inference Python package

Options:
  --install-dependencies   Build/install missing external dependencies (default).
  --no-dependencies        Do not check or install external dependencies.
  --force-dependencies     Rebuild/install external dependencies even if found.
  --deps-dir PATH          Clone dependency sources under PATH (default: ./dependencies).
  --install-system-packages Install common apt build packages when available (default).
  --no-system-packages     Do not install OS packages.
  --with-system-tests-deps Install extra OS packages used by tests/docs.
  --with-minindn-deps      Install OS packages commonly needed by MiniNDN experiments.
  --with-nfd-nlsr-deps     Install OS packages commonly needed to build NFD/NLSR.
  --check-dependencies     Verify the installed global closure and exit.
  --configure              Always run ./waf configure before building.
  --no-configure           Unsupported: every install must revalidate configure.
  --jobs N                 Build parallelism (default: NDNSF_BUILD_JOBS or 4).
  --with-examples          Pass --with-examples to ./waf configure.
  --with-tests             Pass --with-tests to ./waf configure.
  --no-system-install      Skip ./waf install; useful for source-tree testing.
  --system-install         Run ./waf install after build (default).
  --user                   Pass --user to pip install.
  --no-user                Do not pass --user to pip install.
  --no-editable            Use normal pip installs instead of editable installs.
  --python PATH            Python executable to use (default: python3 or $PYTHON).
  -h, --help               Show this help.

Notes:
  - If ./waf install is enabled and needs root, sudo is used automatically.
  - If dependency installation is enabled, the script first installs default
    build/runtime OS packages for ndn-cxx, NDNSD, ndn-svs, OpenABE, NAC-ABE,
    and NDNSF. apt skips packages that are already installed.
  - Optional OS package groups are available for tests/docs, MiniNDN, and
    NFD/NLSR builds.
  - The script checks pkg-config names: libndn-cxx, ndnsd, libndn-svs, and
    libnac-abe.
  - The host Boost 1.71 headers and libraries must be the matching system pair
    /usr/include and /usr/lib/x86_64-linux-gnu. A repository or temporary Boost
    tree is rejected before configure.
  - The installed ONNX Runtime pkg-config closure must resolve to the declared
    global /opt/onnxruntime SDK (the versioned /opt/onnxruntime-1.26.0 target
    may be its real directory). Missing or stale ONNX Runtime stops the script;
    it is never replaced with a model checkout or temporary prefix.
  - If libopenabe is missing, OpenABE is built from dependencies/openabe and
    installed globally under /usr/local before NAC-ABE. The resulting
    libopenabe/relic/OpenSSL closure is checked through the system loader; a
    private checkout prefix is not a runtime dependency.
  - Existing dependency source trees under --deps-dir are reused. Missing trees
    are cloned from the matianxing1992 GitHub repositories.
  - Repository URLs can be overridden with NDNCXX_REPO_URL, NDNSD_REPO_URL,
    NDNSVS_REPO_URL, NACABE_REPO_URL, and OPENABE_REPO_URL.
  - Python extension builds are fail-closed on the installed global
    /usr/local/lib NDNSF Core/DI closure. A checkout or per-run build
    directory is never accepted as a substitute.
  - --no-system-install is build-only and intentionally stops before Python
    bindings; it cannot produce a complete stack without the global Core/DI
    install.
  - This is a host installer for the system Boost 1.71 / x86-64 closure, not
    a portable clean-machine or SIF bootstrap script. Preinstall ONNX Runtime
    1.26+, ONNX full-protobuf headers/archives and the Rust tokenizer archive.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --install-dependencies)
      INSTALL_DEPENDENCIES=1
      shift
      ;;
    --no-dependencies)
      INSTALL_DEPENDENCIES=0
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
      shift
      ;;
    --with-minindn-deps)
      INSTALL_MININDN_PACKAGES=1
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
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo '--jobs must be a positive integer' >&2; exit 2; }
if [[ "$RUN_WAF_CONFIGURE" == "0" ]]; then
  echo '--no-configure is not allowed for the global-closure installer' >&2
  exit 2
fi
# Resolve user-relative paths before changing to the repository root.
DEPS_DIR="$(realpath -m -- "$DEPS_DIR")"
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

pip_install() {
  local path="$1"
  local install_args=()
  if [[ "$INSTALL_EDITABLE" == "1" ]]; then
    install_args+=("-e")
  fi
  if [[ "$USE_USER_FLAG" == "1" ]]; then
    install_args+=("--user")
  fi
  install_args+=("$path")
  run env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
    -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH \
    -u CPLUS_INCLUDE_PATH -u LDSHARED -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR \
    PATH="$SYSTEM_PATH" CC=/usr/bin/gcc CXX=/usr/bin/g++ \
    "$PYTHON_BIN" -m pip install "${install_args[@]}"
}

run_waf_clean() {
  # The top-level configure/build must not inherit a checkout, per-run, or
  # user linker/pkg-config override.  Waf itself performs the detailed global
  # path checks; this wrapper guarantees that those checks see a clean host
  # toolchain and environment every time.
  env \
    -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR \
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
    NDNSF_LIBRARY_DIR="$GLOBAL_LIBRARY_DIR" ./waf "$@"
}

require_system_toolchain() {
  local tool
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
    local soname="${library}.1.71.0"
    if ! /usr/bin/readelf -d "$resolved" 2>/dev/null |
        /usr/bin/grep -Fq "Library soname: [$soname]"; then
      return 1
    fi
  done
  return 0
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
    echo "Install/rebuild the system Boost pair before configuring; no temporary prefix is allowed." >&2
    exit 1
  fi
  echo "==> Global Boost 1.71 ($BOOST_INCLUDE_DIR, $BOOST_LIBRARY_DIR)"
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
  if ! env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR "$PKG_CONFIG_BIN" --exists "$package"; then
    echo "Global SDK dependency is missing: $package >= $minimum" >&2
    echo "Install/rebuild it globally before continuing; no checkout or temporary prefix is allowed." >&2
    exit 1
  fi
  version="$(env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR "$PKG_CONFIG_BIN" --modversion "$package" 2>/dev/null || true)"
  if [[ "$(printf '%s\n' "$minimum" "$version" | /usr/bin/sort -V | /usr/bin/head -n 1)" != "$minimum" ]]; then
    echo "Global SDK dependency is too old: $package $version (need >= $minimum)" >&2
    echo "Install/rebuild it globally; do not point the build at a temporary prefix." >&2
    exit 1
  fi
  prefix="$(env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR "$PKG_CONFIG_BIN" --variable=prefix "$package" 2>/dev/null || true)"
  if ! is_global_sdk_prefix "$prefix"; then
    echo "$package resolves outside the declared global SDK roots: $prefix" >&2
    exit 1
  fi
  require_global_pkg_flags "$package"
  local libdir library_path resolved_library
  libdir="$(env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR "$PKG_CONFIG_BIN" --variable=libdir "$package" 2>/dev/null || true)"
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
  local onnx_libdir
  onnx_libdir="$(env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR "$PKG_CONFIG_BIN" \
    --variable=libdir onnxruntime 2>/dev/null || true)"
  [[ -n "$onnx_libdir" ]] || {
    echo "Cannot determine the global ONNX Runtime library directory" >&2
    return 1
  }
  "$PYTHON_BIN" - "$GLOBAL_LIBRARY_DIR" "$onnx_libdir" <<'PY'
import hashlib
import json
import pathlib
import re
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
onnx_root = pathlib.Path(sys.argv[2])
paths = {
    "libndn-cxx.so": root / "libndn-cxx.so",
    "libndnsd.so": root / "libndnsd.so",
    "libndn-svs.so": root / "libndn-svs.so",
    "libnac-abe.so": root / "libnac-abe.so",
    "libopenabe.so": root / "libopenabe.so",
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
  local receipt temp_file
  receipt="$(global_dependency_identity_receipt)" || exit 1
  sudo_run install -d -m 0755 "$GLOBAL_IDENTITY_DIR"
  temp_file="$GLOBAL_IDENTITY_DIR/.global-dependency-identity.json.$$"
  if [[ "${EUID}" -eq 0 ]]; then
    printf '%s\n' "$receipt" > "$temp_file"
    chmod 0644 "$temp_file"
    mv -f "$temp_file" "$GLOBAL_IDENTITY_FILE"
  else
    printf '%s\n' "$receipt" | sudo -n tee "$temp_file" >/dev/null
    sudo -n chmod 0644 "$temp_file"
    sudo -n mv -f "$temp_file" "$GLOBAL_IDENTITY_FILE"
  fi
  echo "==> Global dependency identity receipt: $GLOBAL_IDENTITY_FILE"
}

require_global_dependency_identity() {
  local onnx_libdir
  [[ -f "$GLOBAL_IDENTITY_FILE" ]] || {
    echo "Global dependency identity receipt is missing: $GLOBAL_IDENTITY_FILE" >&2
    echo "Run the installer to install/rebuild the global dependency closure." >&2
    exit 1
  }
  onnx_libdir="$(env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR "$PKG_CONFIG_BIN" \
    --variable=libdir onnxruntime 2>/dev/null || true)"
  "$PYTHON_BIN" - "$GLOBAL_IDENTITY_FILE" "$GLOBAL_LIBRARY_DIR" "$onnx_libdir" <<'PY'
import hashlib
import json
import pathlib
import re
import subprocess
import sys

receipt_path = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
onnx_root = pathlib.Path(sys.argv[3])
try:
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
except (OSError, ValueError) as error:
    raise SystemExit(f"cannot read global dependency identity receipt: {error}")
if receipt.get("schema") != 1 or not isinstance(receipt.get("libraries"), dict):
    raise SystemExit(f"invalid global dependency identity receipt: {receipt_path}")
expected_paths = {
    "libndn-cxx.so": root / "libndn-cxx.so",
    "libndnsd.so": root / "libndnsd.so",
    "libndn-svs.so": root / "libndn-svs.so",
    "libnac-abe.so": root / "libnac-abe.so",
    "libopenabe.so": root / "libopenabe.so",
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
  onnx_libdir="$(env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR "$PKG_CONFIG_BIN" \
    --variable=libdir onnxruntime 2>/dev/null || true)"
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
  if ! env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR "$PKG_CONFIG_BIN" --exists "$package"; then
    return 1
  fi
  version="$(env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR "$PKG_CONFIG_BIN" --modversion "$package" 2>/dev/null)" || return 1
  if [[ "$(printf '%s\n' "$minimum" "$version" | /usr/bin/sort -V | /usr/bin/head -n 1)" != "$minimum" ]]; then
    return 1
  fi
  prefix="$(env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR "$PKG_CONFIG_BIN" --variable=prefix "$package" 2>/dev/null)" || return 1
  prefix="$(/usr/bin/readlink -f "$prefix" 2>/dev/null || true)"
  [[ "$prefix" == "$GLOBAL_DEPENDENCY_PREFIX" || "$prefix" == "$GLOBAL_DEPENDENCY_PREFIX"/* ]] || return 1
  case "$package" in
    libndn-cxx) library="libndn-cxx.so" ;;
    ndnsd) library="libndnsd.so" ;;
    libndn-svs) library="libndn-svs.so" ;;
    libnac-abe) library="libnac-abe.so" ;;
    *) return 0 ;;
  esac
  libdir="$(env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR "$PKG_CONFIG_BIN" --variable=libdir "$package" 2>/dev/null)" || return 1
  library_path="$libdir/$library"
  resolved_library="$(/usr/bin/readlink -f "$library_path" 2>/dev/null || true)"
  [[ -f "$library_path" && "$resolved_library" == "$GLOBAL_LIBRARY_DIR"/* ]]
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
  version="$(env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR "$PKG_CONFIG_BIN" --modversion "$package")"
  prefix="$(/usr/bin/readlink -f "$(env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR "$PKG_CONFIG_BIN" --variable=prefix "$package")")"
  require_global_pkg_flags "$package"
  require_global_file "$GLOBAL_LIBRARY_DIR/$library"
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
  case "$resolved" in
    "$GLOBAL_DEPENDENCY_PREFIX"/*) ;;
    *)
      echo "Global dependency resolves outside $GLOBAL_DEPENDENCY_PREFIX: $path -> $resolved" >&2
      exit 1
      ;;
  esac
}

require_global_external_closure() {
  require_global_boost
  require_global_pkg "libndn-cxx" "$MIN_NDNCXX_VERSION" "libndn-cxx.so"
  require_global_pkg "ndnsd" "$MIN_NDNSD_VERSION" "libndnsd.so"
  require_global_pkg "libndn-svs" "$MIN_NDNSVS_VERSION" "libndn-svs.so"
  require_global_pkg "libnac-abe" "$MIN_NACABE_VERSION" "libnac-abe.so"
  require_global_sdk_pkg "onnxruntime" "1.26.0" "libonnxruntime.so"
  require_global_file "$GLOBAL_LIBRARY_DIR/libopenabe.so"
  require_global_file "$GLOBAL_LIBRARY_DIR/libonnx.a"
  require_global_file "$GLOBAL_LIBRARY_DIR/libonnx_proto.a"
  require_global_file "$GLOBAL_LIBRARY_DIR/libndnsf_tokenizer_bridge.a"
  require_global_file "$GLOBAL_DEPENDENCY_PREFIX/include/onnx/checker.h"
  require_global_file "$GLOBAL_DEPENDENCY_PREFIX/include/onnx/shape_inference/implementation.h"
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
        grep -Eq " ${symbol}\\("; then
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
    sudo_run apt-get update
    local packages=(
      build-essential git pkg-config cmake python3 python3-pip wget curl \
      python3-dev python3-setuptools python3-wheel python3-venv \
      autoconf automake libtool m4 bison flex ninja-build \
      libgmp-dev libssl-dev \
      libboost-all-dev libsqlite3-dev libpcap-dev libsodium-dev libz-dev \
      liblog4cxx-dev sqlite3 libprotobuf-dev protobuf-compiler libgtkmm-3.0-dev
    )
    if [[ "$INSTALL_TEST_PACKAGES" == "1" ]]; then
      packages+=(libgtest-dev doxygen graphviz)
    fi
    if [[ "$INSTALL_MININDN_PACKAGES" == "1" ]]; then
      packages+=(
        mininet openvswitch-switch tcpdump iproute2 net-tools
        python3-pyroute2 python3-networkx python3-matplotlib
      )
    fi
    if [[ "$INSTALL_NFD_NLSR_PACKAGES" == "1" ]]; then
      packages+=(
        libsystemd-dev libcap-dev libprotobuf-dev protobuf-compiler
        libboost-all-dev libsqlite3-dev libpcap-dev libsodium-dev
      )
    fi
    sudo_run env DEBIAN_FRONTEND=noninteractive apt-get install -y "${packages[@]}"
  else
    echo "==> No supported OS package manager detected; assuming build tools are installed"
  fi
}

ensure_source_tree() {
  local name="$1"
  local url="$2"
  local dir="$DEPS_DIR/$name"

  mkdir -p "$DEPS_DIR"
  if [[ -e "$dir/.git" ]]; then
    echo "==> Reusing dependency source: $dir"
  elif [[ -e "$dir" ]]; then
    echo "Dependency path exists but is not a git repository: $dir" >&2
    exit 1
  else
    echo "==> Cloning $name from $url into $dir"
    run git clone "$url" "$dir"
  fi

  printf '%s\n' "$dir"
}

build_openabe_dependency() {
  local dir

  if [[ "$FORCE_DEPENDENCIES" != "1" ]] && has_openabe; then
    echo "==> OpenABE already installed; skipping"
    return
  fi

  dir="$(ensure_source_tree "openabe" "$OPENABE_REPO_URL" | tail -n 1)"
  echo "==> Building OpenABE from the dependency source tree"
  run bash -e -c "cd \"\$1\" && . ./env && \
    unset PKG_CONFIG_PATH PKG_CONFIG_LIBDIR CFLAGS CXXFLAGS CPPFLAGS LDFLAGS \
      LD_LIBRARY_PATH LIBRARY_PATH CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH \
      BOOST_ROOT BOOST_INCLUDEDIR BOOST_LIBRARYDIR && \
    export PATH='$SYSTEM_PATH' CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld \
      AR=/usr/bin/ar RANLIB=/usr/bin/ranlib NM=/usr/bin/nm STRIP=/usr/bin/strip \
      BOOST_ROOT= BOOST_INCLUDEDIR= BOOST_LIBRARYDIR= && \
    { make clean >/dev/null 2>&1 || true; } && \
    make -j'$JOBS' -C deps/openssl && make -j'$JOBS' -C deps/relic && make -j'$JOBS' -C deps/gtest && \
    BISON=\$(command -v bison) FLEX=\$(command -v flex) make -j'$JOBS'" _ "$dir"
  echo "==> Installing OpenABE into the global prefix $OPENABE_PREFIX"
  sudo_run env \
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
      NM=/usr/bin/nm STRIP=/usr/bin/strip && make INSTALL_PREFIX='$OPENABE_PREFIX' install" _ "$dir"
  sudo_run ldconfig
}

build_waf_dependency() {
  local name="$1"
  local pkg="$2"
  local url="$3"
  local minimum="$4"
  local dir build_dir

  if [[ "$FORCE_DEPENDENCIES" != "1" ]] && is_pkg_installed "$pkg" "$minimum"; then
    echo "==> $name already installed ($pkg); skipping"
    return
  fi

  dir="$(ensure_source_tree "$name" "$url" | tail -n 1)"
  echo "==> Building dependency $name"
  run bash -e -c "cd \"\$1\" || exit; \
    env -u PKG_CONFIG_PATH -u CXXFLAGS -u CFLAGS -u CPPFLAGS -u LDFLAGS \
      -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH \
      -u CPLUS_INCLUDE_PATH -u BOOST_ROOT -u BOOST_INCLUDEDIR -u BOOST_LIBRARYDIR \
      PATH='$SYSTEM_PATH' PKGCONFIG='$PKG_CONFIG_BIN' \
      CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld AR=/usr/bin/ar \
      AS=/usr/bin/as RANLIB=/usr/bin/ranlib NM=/usr/bin/nm STRIP=/usr/bin/strip \
      ./waf distclean >/dev/null 2>&1 || true; \
    env -u PKG_CONFIG_PATH -u CXXFLAGS -u CFLAGS -u CPPFLAGS -u LDFLAGS \
      -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH \
      -u CPLUS_INCLUDE_PATH -u BOOST_ROOT -u BOOST_INCLUDEDIR -u BOOST_LIBRARYDIR \
      PATH='$SYSTEM_PATH' PKGCONFIG='$PKG_CONFIG_BIN' \
      CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld AR=/usr/bin/ar \
      AS=/usr/bin/as RANLIB=/usr/bin/ranlib NM=/usr/bin/nm STRIP=/usr/bin/strip \
      ./waf configure --prefix='$GLOBAL_DEPENDENCY_PREFIX' && \
    env -u PKG_CONFIG_PATH -u CXXFLAGS -u CFLAGS -u CPPFLAGS -u LDFLAGS \
      -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH \
      -u CPLUS_INCLUDE_PATH -u BOOST_ROOT -u BOOST_INCLUDEDIR -u BOOST_LIBRARYDIR \
      PATH='$SYSTEM_PATH' PKGCONFIG='$PKG_CONFIG_BIN' \
      CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld AR=/usr/bin/ar \
      AS=/usr/bin/as RANLIB=/usr/bin/ranlib NM=/usr/bin/nm STRIP=/usr/bin/strip \
      ./waf -j'$JOBS'" _ "$dir"
  echo "==> Installing dependency $name"
  sudo_run env -u BOOST_ROOT -u BOOST_INCLUDEDIR -u BOOST_LIBRARYDIR \
    PATH="$SYSTEM_PATH" PKGCONFIG="$PKG_CONFIG_BIN" \
    CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld AR=/usr/bin/ar \
    AS=/usr/bin/as RANLIB=/usr/bin/ranlib NM=/usr/bin/nm STRIP=/usr/bin/strip \
    bash -e -c 'cd "$1" && ./waf install' _ "$dir"
  sudo_run ldconfig
}

build_cmake_dependency() {
  local name="$1"
  local pkg="$2"
  local url="$3"
  local minimum="$4"
  local dir build_dir

  if [[ "$FORCE_DEPENDENCIES" != "1" ]] && is_pkg_installed "$pkg" "$minimum"; then
    echo "==> $name already installed ($pkg); skipping"
    return
  fi

  dir="$(ensure_source_tree "$name" "$url" | tail -n 1)"
  build_dir="$dir/.ndnsf-global-build-${name//[^A-Za-z0-9]/_}-$$"
  echo "==> Building dependency $name"
  if [[ "$name" == "NAC-ABE" && -n "$OPENABE_PREFIX" && -f "$OPENABE_PREFIX/lib/libopenabe.so" ]]; then
    run bash -e -c "cd \"\$1\" && env \
      -u PKG_CONFIG_PATH \
      -u BOOST_ROOT -u BOOST_INCLUDEDIR -u BOOST_LIBRARYDIR \
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
  sudo_run bash -e -c "cd \"\$1\" && env -u BOOST_ROOT -u BOOST_INCLUDEDIR \
    -u BOOST_LIBRARYDIR PATH='$SYSTEM_PATH' CC=/usr/bin/gcc CXX=/usr/bin/g++ \
    LD=/usr/bin/ld AR=/usr/bin/ar RANLIB=/usr/bin/ranlib \
    cmake --install \"\$2\"" _ "$dir" "$build_dir"
  rm -rf "$build_dir"
  sudo_run ldconfig
}

install_external_dependencies() {
  echo "==> Checking external NDN dependencies"
  echo "==> Dependency source directory: $DEPS_DIR"
  # These SDKs are not built here. Fail before changing other installed libraries.
  require_global_sdk_pkg "onnxruntime" "1.26.0" "libonnxruntime.so"
  require_global_file "$GLOBAL_LIBRARY_DIR/libonnx.a"
  require_global_file "$GLOBAL_LIBRARY_DIR/libonnx_proto.a"
  require_global_file "$GLOBAL_LIBRARY_DIR/libndnsf_tokenizer_bridge.a"
  require_global_file "$GLOBAL_DEPENDENCY_PREFIX/include/onnx/checker.h"
  require_global_file "$GLOBAL_DEPENDENCY_PREFIX/include/onnx/shape_inference/implementation.h"
  if [[ -f "$GLOBAL_IDENTITY_FILE" ]] && ! has_global_dependency_identity; then
    if ! has_global_onnx_identity; then
      echo "Global ONNX Runtime identity changed; this installer cannot rebuild ONNX Runtime. Reinstall the canonical global SDK and refresh the identity receipt before continuing." >&2
      exit 1
    fi
    echo "==> Global dependency identity is stale; rebuilding the installed closure"
    FORCE_DEPENDENCIES=1
  fi
  if [[ "$INSTALL_SYSTEM_PACKAGES" == "1" ]] || [[ "$FORCE_DEPENDENCIES" == "1" ]] || ! has_global_boost || \
     ! is_pkg_installed "libndn-cxx" "$MIN_NDNCXX_VERSION" || \
     ! is_pkg_installed "ndnsd" "$MIN_NDNSD_VERSION" || \
     ! is_pkg_installed "libndn-svs" "$MIN_NDNSVS_VERSION" || \
     ! is_pkg_installed "libnac-abe" "$MIN_NACABE_VERSION" || \
     ! has_openabe; then
    install_common_system_packages
  else
    echo "==> External dependencies already present; skipping OS package installation"
  fi
  build_waf_dependency "ndn-cxx" "libndn-cxx" "$NDNCXX_REPO_URL" "$MIN_NDNCXX_VERSION"
  build_waf_dependency "NDNSD" "ndnsd" "$NDNSD_REPO_URL" "$MIN_NDNSD_VERSION"
  build_waf_dependency "ndn-svs" "libndn-svs" "$NDNSVS_REPO_URL" "$MIN_NDNSVS_VERSION"
  build_openabe_dependency
  build_cmake_dependency "NAC-ABE" "libnac-abe" "$NACABE_REPO_URL" "$MIN_NACABE_VERSION"
  write_global_dependency_identity
  require_global_external_closure
}

cd "$ROOT"

echo "==> NDNSF stack install root: $ROOT"
echo "==> Python: $PYTHON_BIN"
require_system_toolchain

if [[ "$CHECK_DEPENDENCIES" == "1" ]]; then
  require_global_external_closure
  echo "==> Installed NDNSF global dependency closure is valid"
  exit 0
fi

if [[ "$INSTALL_DEPENDENCIES" == "auto" ]]; then
  INSTALL_DEPENDENCIES=1
fi

if [[ "$INSTALL_DEPENDENCIES" == "1" ]]; then
  install_external_dependencies
else
  echo "==> Skipping external dependency installation"
  # --no-dependencies only skips cloning/building sources. It never permits
  # a missing or stale global dependency to enter the NDNSF build.
  require_global_external_closure
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
  sudo_run env \
    -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR \
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
    NDNSF_LIBRARY_DIR="$GLOBAL_LIBRARY_DIR" ./waf install -j"$JOBS"
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
pip_install "$ROOT/pythonWrapper"

echo "==> Installing py_repoclient Python binding"
pip_install "$ROOT/NDNSF-DistributedRepo/pythonWrapper"

echo "==> Installing NDNSF-DistributedInference Python package"
pip_install "$ROOT/NDNSF-DistributedInference"

echo "==> Running Python import smoke checks"
run "$PYTHON_BIN" - <<'PY'
import ndnsf
import py_repoclient
import ndnsf_distributed_inference

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

echo "==> NDNSF stack installation complete"
