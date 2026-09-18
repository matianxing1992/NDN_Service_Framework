#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_BIN="${PYTHON:-python3}"
PIP_ARGS=()
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
  2. NDNSF C++ core and bundled C++ subprojects through waf
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
  --no-configure           Skip ./waf configure.
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
      PYTHON_BIN="$2"
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
  run "$PYTHON_BIN" -m pip install "${install_args[@]}"
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
    PATH="$SYSTEM_PATH" PKGCONFIG="$PKG_CONFIG_BIN" \
    CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld \
    AR=/usr/bin/ar AS=/usr/bin/as RANLIB=/usr/bin/ranlib \
    NM=/usr/bin/nm STRIP=/usr/bin/strip \
    NDNSF_LIBRARY_DIR="$GLOBAL_LIBRARY_DIR" ./waf "$@"
}

require_system_toolchain() {
  local tool
  for tool in gcc g++ ld ar as ranlib nm strip "$PKG_CONFIG_BIN"; do
    if [[ "$tool" == /* ]]; then
      [[ -x "$tool" ]] || { echo "Missing canonical host tool: $tool" >&2; exit 1; }
    else
      [[ -x "/usr/bin/$tool" ]] || { echo "Missing canonical host tool: /usr/bin/$tool" >&2; exit 1; }
    fi
  done
}

is_pkg_installed() {
  local package="$1"
  local minimum="${2:-0}"
  local version prefix
  if ! env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR "$PKG_CONFIG_BIN" --exists "$package"; then
    return 1
  fi
  version="$(env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR "$PKG_CONFIG_BIN" --modversion "$package" 2>/dev/null)" || return 1
  if [[ "$(printf '%s\n' "$minimum" "$version" | /usr/bin/sort -V | /usr/bin/head -n 1)" != "$minimum" ]]; then
    return 1
  fi
  prefix="$(env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR "$PKG_CONFIG_BIN" --variable=prefix "$package" 2>/dev/null)" || return 1
  prefix="$(/usr/bin/readlink -f "$prefix" 2>/dev/null || true)"
  [[ "$prefix" == "$GLOBAL_DEPENDENCY_PREFIX" || "$prefix" == "$GLOBAL_DEPENDENCY_PREFIX"/* ]]
}

require_global_pkg_flags() {
  "$PYTHON_BIN" - "$@" <<'PY'
import os
import pathlib
import shlex
import subprocess
import sys

roots = tuple(pathlib.Path(item).resolve()
              for item in ("/usr", "/usr/local", "/lib", "/lib64"))
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
  require_global_pkg "libndn-cxx" "$MIN_NDNCXX_VERSION" "libndn-cxx.so"
  require_global_pkg "ndnsd" "$MIN_NDNSD_VERSION" "libndnsd.so"
  require_global_pkg "libndn-svs" "$MIN_NDNSVS_VERSION" "libndn-svs.so"
  require_global_pkg "libnac-abe" "$MIN_NACABE_VERSION" "libnac-abe.so"
  require_global_file "$GLOBAL_LIBRARY_DIR/libopenabe.so"
  require_global_file "$GLOBAL_LIBRARY_DIR/libonnx.a"
  require_global_file "$GLOBAL_LIBRARY_DIR/libonnx_proto.a"
  require_global_file "$GLOBAL_LIBRARY_DIR/libndnsf_tokenizer_bridge.a"
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
      liblog4cxx-dev sqlite3
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
  if [[ -d "$dir/.git" ]]; then
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
  run bash -lc "cd '$dir' && env \
    -u PKG_CONFIG_PATH -u CXXFLAGS -u CFLAGS -u CPPFLAGS -u LDFLAGS \
    -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH \
    -u CPLUS_INCLUDE_PATH bash -c '. ./env && make -C deps/openssl && \
      make -C deps/relic && make -C deps/gtest && \
      BISON=\$(command -v bison) FLEX=\$(command -v flex) make'"
  echo "==> Installing OpenABE into the global prefix $OPENABE_PREFIX"
  sudo_run env \
    -u PKG_CONFIG_PATH -u CXXFLAGS -u CFLAGS -u CPPFLAGS -u LDFLAGS \
    -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH \
    -u CPLUS_INCLUDE_PATH bash -lc \
    "cd '$dir' && . ./env && make INSTALL_PREFIX='$OPENABE_PREFIX' install"
  sudo_run ldconfig
}

build_waf_dependency() {
  local name="$1"
  local pkg="$2"
  local url="$3"
  local minimum="$4"
  local dir

  if [[ "$FORCE_DEPENDENCIES" != "1" ]] && is_pkg_installed "$pkg" "$minimum"; then
    echo "==> $name already installed ($pkg); skipping"
    return
  fi

  dir="$(ensure_source_tree "$name" "$url" | tail -n 1)"
  echo "==> Building dependency $name"
  run bash -lc "cd '$dir' && \
    env -u PKG_CONFIG_PATH -u CXXFLAGS -u CFLAGS -u CPPFLAGS -u LDFLAGS \
      -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH \
      -u CPLUS_INCLUDE_PATH ./waf configure --prefix='$GLOBAL_DEPENDENCY_PREFIX' && \
    env -u PKG_CONFIG_PATH -u CXXFLAGS -u CFLAGS -u CPPFLAGS -u LDFLAGS \
      -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH \
      -u CPLUS_INCLUDE_PATH ./waf -j\$(nproc)"
  echo "==> Installing dependency $name"
  sudo_run bash -lc "cd '$dir' && ./waf install"
  sudo_run ldconfig
}

build_cmake_dependency() {
  local name="$1"
  local pkg="$2"
  local url="$3"
  local minimum="$4"
  local dir

  if [[ "$FORCE_DEPENDENCIES" != "1" ]] && is_pkg_installed "$pkg" "$minimum"; then
    echo "==> $name already installed ($pkg); skipping"
    return
  fi

  dir="$(ensure_source_tree "$name" "$url" | tail -n 1)"
  echo "==> Building dependency $name"
  if [[ "$name" == "NAC-ABE" && -n "$OPENABE_PREFIX" && -f "$OPENABE_PREFIX/lib/libopenabe.so" ]]; then
    run bash -lc "cd '$dir' && env \
      -u PKG_CONFIG_PATH \
      CMAKE_PREFIX_PATH='$OPENABE_PREFIX' \
      CMAKE_INCLUDE_PATH='$OPENABE_PREFIX/include' \
      CMAKE_LIBRARY_PATH='$OPENABE_PREFIX/lib' \
      CPPFLAGS='-I$OPENABE_PREFIX/include' \
      CXXFLAGS='-I$OPENABE_PREFIX/include' \
      LDFLAGS='-L$OPENABE_PREFIX/lib -Wl,-rpath,$OPENABE_PREFIX/lib' \
      LD_LIBRARY_PATH='$OPENABE_PREFIX/lib' \
      cmake -S . -B build \
        -DCMAKE_INSTALL_PREFIX='$GLOBAL_DEPENDENCY_PREFIX' \
        -DCMAKE_BUILD_RPATH='$OPENABE_PREFIX/lib' \
        -DCMAKE_INSTALL_RPATH='$OPENABE_PREFIX/lib' && \
      env -u PKG_CONFIG_PATH -u CMAKE_PREFIX_PATH -u CMAKE_INCLUDE_PATH \
        -u CMAKE_LIBRARY_PATH -u CMAKE_FRAMEWORK_PATH -u CMAKE_APPBUNDLE_PATH \
        -u CXXFLAGS -u CFLAGS -u CPPFLAGS -u LDFLAGS -u LD_LIBRARY_PATH \
        -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH \
        cmake --build build -j\$(nproc)"
  else
    run bash -lc "cd '$dir' && env \
      -u PKG_CONFIG_PATH -u CMAKE_PREFIX_PATH -u CMAKE_INCLUDE_PATH -u CMAKE_LIBRARY_PATH \
      -u CMAKE_FRAMEWORK_PATH -u CMAKE_APPBUNDLE_PATH -u CXXFLAGS \
      -u CFLAGS -u CPPFLAGS -u LDFLAGS -u LD_LIBRARY_PATH \
      -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH \
      cmake -S . -B build \
      -DCMAKE_INSTALL_PREFIX='$GLOBAL_DEPENDENCY_PREFIX' && \
      env -u PKG_CONFIG_PATH -u CMAKE_PREFIX_PATH -u CMAKE_INCLUDE_PATH \
        -u CMAKE_LIBRARY_PATH -u CMAKE_FRAMEWORK_PATH -u CMAKE_APPBUNDLE_PATH \
        -u CXXFLAGS -u CFLAGS -u CPPFLAGS -u LDFLAGS -u LD_LIBRARY_PATH \
        -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH \
        cmake --build build -j\$(nproc)"
  fi
  echo "==> Installing dependency $name"
  sudo_run bash -lc "cd '$dir' && cmake --install build"
  sudo_run ldconfig
}

install_external_dependencies() {
  echo "==> Checking external NDN dependencies"
  echo "==> Dependency source directory: $DEPS_DIR"
  if [[ "$FORCE_DEPENDENCIES" == "1" ]] || \
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
run_waf_clean

if [[ "$RUN_SYSTEM_INSTALL" == "1" ]]; then
  echo "==> Installing C++ libraries and headers"
  sudo_run env \
    -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR \
    -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
    -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH \
    -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH -u LDSHARED -u WAFDIR \
    -u PKGCONFIG -u LD -u AR -u AS -u RANLIB -u NM -u STRIP \
    -u OBJCOPY -u OBJDUMP -u READELF \
    PATH="$SYSTEM_PATH" PKGCONFIG="$PKG_CONFIG_BIN" \
    CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld \
    AR=/usr/bin/ar AS=/usr/bin/as RANLIB=/usr/bin/ranlib \
    NM=/usr/bin/nm STRIP=/usr/bin/strip \
    NDNSF_LIBRARY_DIR="$GLOBAL_LIBRARY_DIR" ./waf install
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
