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
OPENABE_PREFIX=""

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
  - If libopenabe is missing, OpenABE is cloned into dependencies/openabe and
    installed privately under dependencies/local/openabe. Its bundled OpenSSL
    1.1.1-dev dependency is used for OpenABE/NAC-ABE without replacing the
    system OpenSSL.
  - Existing dependency source trees under --deps-dir are reused. Missing trees
    are cloned from the matianxing1992 GitHub repositories.
  - Repository URLs can be overridden with NDNCXX_REPO_URL, NDNSD_REPO_URL,
    NDNSVS_REPO_URL, NACABE_REPO_URL, and OPENABE_REPO_URL.
  - Python extension builds search the local ./build directory first, so source
    tree installs work before system-wide installation.
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

is_pkg_installed() {
  pkg-config --exists "$1"
}

has_openabe() {
  [[ -n "$OPENABE_PREFIX" && -f "$OPENABE_PREFIX/lib/libopenabe.so" ]] && return 0
  ldconfig -p 2>/dev/null | grep -q 'libopenabe\.so' && return 0
  [[ -f /usr/local/lib/libopenabe.so ]] && return 0
  return 1
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
  OPENABE_PREFIX="$DEPS_DIR/local/openabe"

  if [[ "$FORCE_DEPENDENCIES" != "1" ]] && has_openabe; then
    echo "==> OpenABE already installed; skipping"
    return
  fi

  dir="$(ensure_source_tree "openabe" "$OPENABE_REPO_URL" | tail -n 1)"
  echo "==> Building OpenABE with private OpenSSL 1.1 dependency"
  run bash -lc "cd '$dir' && . ./env && make -C deps/openssl && make -C deps/relic && make -C deps/gtest && BISON=\$(command -v bison) FLEX=\$(command -v flex) make"
  echo "==> Installing OpenABE under $OPENABE_PREFIX"
  run bash -lc "cd '$dir' && . ./env && make INSTALL_PREFIX='$OPENABE_PREFIX' install"
}

build_waf_dependency() {
  local name="$1"
  local pkg="$2"
  local url="$3"
  local dir

  if [[ "$FORCE_DEPENDENCIES" != "1" ]] && is_pkg_installed "$pkg"; then
    echo "==> $name already installed ($pkg); skipping"
    return
  fi

  dir="$(ensure_source_tree "$name" "$url" | tail -n 1)"
  echo "==> Building dependency $name"
  run bash -lc "cd '$dir' && ./waf configure && ./waf -j\$(nproc)"
  echo "==> Installing dependency $name"
  sudo_run bash -lc "cd '$dir' && ./waf install"
  sudo_run ldconfig
}

build_cmake_dependency() {
  local name="$1"
  local pkg="$2"
  local url="$3"
  local dir

  if [[ "$FORCE_DEPENDENCIES" != "1" ]] && is_pkg_installed "$pkg"; then
    echo "==> $name already installed ($pkg); skipping"
    return
  fi

  dir="$(ensure_source_tree "$name" "$url" | tail -n 1)"
  echo "==> Building dependency $name"
  if [[ "$name" == "NAC-ABE" && -n "$OPENABE_PREFIX" && -f "$OPENABE_PREFIX/lib/libopenabe.so" ]]; then
    run bash -lc "cd '$dir' && env \
      CMAKE_PREFIX_PATH='$OPENABE_PREFIX':\${CMAKE_PREFIX_PATH:-} \
      CMAKE_INCLUDE_PATH='$OPENABE_PREFIX/include':\${CMAKE_INCLUDE_PATH:-} \
      CMAKE_LIBRARY_PATH='$OPENABE_PREFIX/lib':\${CMAKE_LIBRARY_PATH:-} \
      CXXFLAGS='-I$OPENABE_PREFIX/include '\${CXXFLAGS:-} \
      LDFLAGS='-L$OPENABE_PREFIX/lib -Wl,-rpath,$OPENABE_PREFIX/lib '\${LDFLAGS:-} \
      LD_LIBRARY_PATH='$OPENABE_PREFIX/lib':\${LD_LIBRARY_PATH:-} \
      cmake -S . -B build \
        -DCMAKE_BUILD_RPATH='$OPENABE_PREFIX/lib' \
        -DCMAKE_INSTALL_RPATH='$OPENABE_PREFIX/lib' && \
      cmake --build build -j\$(nproc)"
  else
    run bash -lc "cd '$dir' && cmake -S . -B build && cmake --build build -j\$(nproc)"
  fi
  echo "==> Installing dependency $name"
  sudo_run bash -lc "cd '$dir' && cmake --install build"
  sudo_run ldconfig
}

install_external_dependencies() {
  echo "==> Checking external NDN dependencies"
  echo "==> Dependency source directory: $DEPS_DIR"
  if [[ "$FORCE_DEPENDENCIES" == "1" ]] || \
     ! is_pkg_installed "libndn-cxx" || \
     ! is_pkg_installed "ndnsd" || \
     ! is_pkg_installed "libndn-svs" || \
     ! is_pkg_installed "libnac-abe" || \
     ! has_openabe; then
    install_common_system_packages
  else
    echo "==> External dependencies already present; skipping OS package installation"
  fi
  build_waf_dependency "ndn-cxx" "libndn-cxx" "$NDNCXX_REPO_URL"
  build_waf_dependency "NDNSD" "ndnsd" "$NDNSD_REPO_URL"
  build_waf_dependency "ndn-svs" "libndn-svs" "$NDNSVS_REPO_URL"
  build_openabe_dependency
  build_cmake_dependency "NAC-ABE" "libnac-abe" "$NACABE_REPO_URL"
}

cd "$ROOT"

echo "==> NDNSF stack install root: $ROOT"
echo "==> Python: $PYTHON_BIN"

if [[ "$INSTALL_DEPENDENCIES" == "auto" ]]; then
  INSTALL_DEPENDENCIES=1
fi

if [[ "$INSTALL_DEPENDENCIES" == "1" ]]; then
  install_external_dependencies
else
  echo "==> Skipping external dependency installation"
fi

if [[ "$RUN_WAF_CONFIGURE" == "auto" ]]; then
  if [[ ! -f "$ROOT/build/config.hpp" ]]; then
    RUN_WAF_CONFIGURE=1
  else
    RUN_WAF_CONFIGURE=0
  fi
fi

if [[ "$RUN_WAF_CONFIGURE" == "1" ]]; then
  echo "==> Configuring waf project"
  run ./waf configure "${WAF_CONFIGURE_ARGS[@]}"
else
  echo "==> Skipping waf configure"
fi

echo "==> Building C++ libraries and bundled subprojects"
run ./waf

if [[ "$RUN_SYSTEM_INSTALL" == "1" ]]; then
  echo "==> Installing C++ libraries and headers"
  sudo_run ./waf install
else
  echo "==> Skipping system C++ install"
fi

echo "==> Installing ndnsf Python wrapper"
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
