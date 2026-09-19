#!/usr/bin/env bash
set -euo pipefail

# Install exactly one already-configured Waf target into the canonical host
# prefix. The complete install_ndnsf_stack.sh remains the entry point for
# dependency repair, configure, and Python binding installation.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${NDNSF_BUILD_DIR:-}"
TARGET=""
JOBS="${NDNSF_BUILD_JOBS:-4}"
SYSTEM_PATH="/usr/bin:/bin:/usr/sbin:/sbin"
SEEN_BUILD_DIR=0
SEEN_TARGET=0
SEEN_JOBS=0

usage() {
  cat <<'EOF'
Usage: scripts/install-global-target.sh --build-dir PATH --target NAME [--jobs N]

Build and install exactly one target from an already configured, verified Waf
tree. This command does not install external dependencies, configure Waf, or
install Python bindings.

Options:
  --build-dir PATH  Existing Waf build directory (required unless
                    NDNSF_BUILD_DIR is set).
  --target NAME     One Waf target name; commas are rejected.
  --jobs N          Build parallelism (default: NDNSF_BUILD_JOBS or 4).
  -h, --help        Show this help.

ONNX Runtime is an external global SDK at /opt/onnxruntime, not a Waf target.
This command therefore refuses libonnxruntime.so/onnxruntime; install or
replace that SDK separately, then rebuild its affected NDNSF consumers.
EOF
}

die() {
  echo "install-global-target.sh: $*" >&2
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      [[ $# -ge 2 ]] || die "--build-dir requires a path"
      (( SEEN_BUILD_DIR == 0 )) || die "--build-dir may be specified only once"
      SEEN_BUILD_DIR=1
      BUILD_DIR="$2"
      shift 2
      ;;
    --target)
      [[ $# -ge 2 ]] || die "--target requires a name"
      (( SEEN_TARGET == 0 )) || die "--target may be specified only once"
      SEEN_TARGET=1
      TARGET="$2"
      shift 2
      ;;
    --jobs)
      [[ $# -ge 2 ]] || die "--jobs requires a positive integer"
      (( SEEN_JOBS == 0 )) || die "--jobs may be specified only once"
      SEEN_JOBS=1
      JOBS="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1"
      ;;
  esac
done

[[ -n "$BUILD_DIR" ]] || die "--build-dir is required"
[[ -n "$TARGET" ]] || die "--target is required"
[[ "$TARGET" != *,* ]] || die "exactly one target is allowed; commas are not accepted"
[[ "$TARGET" != */* && "$TARGET" != *..* ]] || die "target must be a Waf name, not a path"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "--jobs must be a positive integer"

case "$TARGET" in
  onnxruntime|onnxruntime-cpu|libonnxruntime.so|libonnxruntime.so.*)
    die "ONNX Runtime is an external SDK at /opt/onnxruntime; it is not compiled by this target installer"
    ;;
esac

BUILD_DIR="$(readlink -f "$BUILD_DIR" 2>/dev/null || true)"
[[ -n "$BUILD_DIR" && -d "$BUILD_DIR" ]] || die "build directory does not exist"
[[ -f "$BUILD_DIR/.lock-waf_linux_build" ]] || \
  die "build directory is not a configured Waf tree: $BUILD_DIR"

WAF="$ROOT/waf"
[[ -x "$WAF" ]] || die "missing executable Waf entry point: $WAF"

clean_env=(
  env
  -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR
  -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS
  -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH
  -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH -u LDSHARED -u WAFDIR
  -u PKGCONFIG -u LD -u AR -u AS -u RANLIB -u NM -u STRIP
  -u OBJCOPY -u OBJDUMP -u READELF
  -u BOOST_ROOT -u BOOST_INCLUDEDIR -u BOOST_LIBRARYDIR
  "PATH=$SYSTEM_PATH" "PKGCONFIG=/usr/bin/pkg-config"
  "CC=/usr/bin/gcc" "CXX=/usr/bin/g++" "LD=/usr/bin/ld"
  "AR=/usr/bin/ar" "AS=/usr/bin/as" "RANLIB=/usr/bin/ranlib"
  "NM=/usr/bin/nm" "STRIP=/usr/bin/strip"
  "NDNSF_LIBRARY_DIR=/usr/local/lib"
  "NDNSF_SKIP_DEV_PIP_INSTALL=1"
)

echo "==> Checking the canonical global dependency receipt"
(cd "$ROOT" && "${clean_env[@]}" "$ROOT/install_ndnsf_stack.sh" \
  --check-dependencies --python /usr/bin/python3)

echo "==> Checking the configured Waf dependency paths and RPATH"
(cd "$ROOT" && "${clean_env[@]}" /usr/bin/python3 \
  "$ROOT/scripts/spec180_native_build.py" preflight \
  --root "$ROOT" --build-dir "$BUILD_DIR")

echo "==> Building one global target: $TARGET"
(cd "$BUILD_DIR" && "${clean_env[@]}" "$WAF" build --targets="$TARGET" -j"$JOBS")

echo "==> Installing one global target: $TARGET"
if [[ "${EUID}" -eq 0 ]]; then
  (cd "$BUILD_DIR" && "${clean_env[@]}" "$WAF" install --targets="$TARGET" -j"$JOBS")
else
  (cd "$BUILD_DIR" && sudo -n "${clean_env[@]}" "$WAF" install --targets="$TARGET" -j"$JOBS")
fi

echo "==> Installed target: $TARGET"
echo "==> Python bindings were not rebuilt; use install_ndnsf_stack.sh only when their ABI/input changed"
