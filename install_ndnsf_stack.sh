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

NDNCXX_REPO_URL="${NDNCXX_REPO_URL:-git@github.com:matianxing1992/ndn-cxx.git}"
NDNSD_REPO_URL="${NDNSD_REPO_URL:-git@github.com:matianxing1992/NDNSD.git}"
NDNSVS_REPO_URL="${NDNSVS_REPO_URL:-git@github.com:matianxing1992/ndn-svs.git}"
NACABE_REPO_URL="${NACABE_REPO_URL:-git@github.com:matianxing1992/NAC-ABE.git}"

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
  - If dependency installation is enabled, the script checks pkg-config names:
    libndn-cxx, ndnsd, libndn-svs, and libnac-abe.
  - Existing dependency source trees under --deps-dir are reused. Missing trees
    are cloned from the matianxing1992 GitHub repositories.
  - Repository URLs can be overridden with NDNCXX_REPO_URL, NDNSD_REPO_URL,
    NDNSVS_REPO_URL, and NACABE_REPO_URL.
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
  run bash -lc "cd '$dir' && cmake -S . -B build && cmake --build build -j\$(nproc)"
  echo "==> Installing dependency $name"
  sudo_run bash -lc "cd '$dir' && cmake --install build"
  sudo_run ldconfig
}

install_external_dependencies() {
  echo "==> Checking external NDN dependencies"
  echo "==> Dependency source directory: $DEPS_DIR"
  build_waf_dependency "ndn-cxx" "libndn-cxx" "$NDNCXX_REPO_URL"
  build_waf_dependency "NDNSD" "ndnsd" "$NDNSD_REPO_URL"
  build_waf_dependency "ndn-svs" "libndn-svs" "$NDNSVS_REPO_URL"
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
