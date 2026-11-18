#!/usr/bin/env bash
set -euo pipefail

single_backend=
if [[ "${1:-}" == --single ]]; then
  disabled_normal=${2:?usage: $0 --single PREFIX enabled|disabled [output-dir]}
  single_backend=${3:?usage: $0 --single PREFIX enabled|disabled [output-dir]}
  case "$single_backend" in
    enabled|disabled) ;;
    *) echo "BACKEND_VARIANT_INVALID:$single_backend" >&2; exit 2 ;;
  esac
  output_dir=${4:-"${TMPDIR:-/tmp}/di-installed-api"}
else
  disabled_normal=${1:?usage: $0 <disabled-normal-prefix> <disabled-asan-prefix> <enabled-normal-prefix> <enabled-asan-prefix> [output-dir]}
  disabled_asan=${2:?usage: $0 <disabled-normal-prefix> <disabled-asan-prefix> <enabled-normal-prefix> <enabled-asan-prefix> [output-dir]}
  enabled_normal=${3:?usage: $0 <disabled-normal-prefix> <disabled-asan-prefix> <enabled-normal-prefix> <enabled-asan-prefix> [output-dir]}
  enabled_asan=${4:?usage: $0 <disabled-normal-prefix> <disabled-asan-prefix> <enabled-normal-prefix> <enabled-asan-prefix> [output-dir]}
  output_dir=${5:-"${TMPDIR:-/tmp}/di-installed-api"}
fi
source_file=$(cd "$(dirname "$0")" && pwd)/di-installed-consumer.cpp
repo_root=$(git -C "$(dirname "$0")/../.." rev-parse --show-toplevel 2>/dev/null || true)

# The header roster comes from the install prefix's own manifest, not from an ignored Spec file and
# not from a fixed count. The previous form read an api-exposure.json that is Git-ignored, so this
# check could not run from a fresh clone at all, and it asserted exactly 68 headers -- a number that
# goes stale whenever an unrelated header is added while saying nothing about whether the installed
# SDK is complete.
manifest_file="${NDNSF_INSTALL_MANIFEST:-$disabled_normal/share/ndnsf/stack-install-manifest.json}"
if [[ ! -f "$manifest_file" ]]; then
  echo "INSTALL_MANIFEST_MISSING:$manifest_file" >&2
  exit 6
fi

mapfile -t installed_headers < <(python3 - "$manifest_file" <<'PY'
import json
import sys

manifest = json.load(open(sys.argv[1], encoding="utf-8"))
for path in sorted(manifest["files"]):
    if not path.endswith((".hpp", ".h")):
        continue
    if "/include/" in path:
        print(path[path.index("/include/") + len("/include/"):])
PY
)
if [[ "${#installed_headers[@]}" -eq 0 ]]; then
  echo "INSTALL_MANIFEST_HAS_NO_HEADERS:$manifest_file" >&2
  exit 6
fi

mkdir -p "$output_dir"

run_one() {
  local mode=$1 sanitizer=$2 prefix=$3
  prefix=$(cd "$prefix" && pwd)
  local pkg_root="$output_dir/pkg-$mode-$sanitizer"
  mkdir -p "$pkg_root"
  local system_pkg_dirs
  system_pkg_dirs=$(env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR \
    -u PKG_CONFIG_SYSROOT_DIR pkg-config --variable=pc_path pkg-config)
  export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$prefix/lib64/pkgconfig:$system_pkg_dirs"
  unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_TOP_BUILD_DIR
  local package metadata_dir
  for package in libndn-service-framework ndnsf-distributed-inference; do
    metadata_dir=$(pkg-config --variable=pcfiledir "$package")
    case "$metadata_dir" in
      "$prefix"/*) ;;
      *) echo "PKG_CONFIG_PREFIX_MISMATCH:$package:$metadata_dir" >&2; exit 5 ;;
    esac
  done
  local package_libdir
  package_libdir=$(pkg-config --variable=libdir ndnsf-distributed-inference)
  case "$package_libdir" in
    "$prefix"/*) ;;
    *) echo "PKG_CONFIG_PREFIX_MISMATCH:$package_libdir" >&2; exit 5 ;;
  esac
  read -r -a cflags <<< "$(pkg-config --cflags ndnsf-distributed-inference)"
  read -r -a libs <<< "$(pkg-config --libs ndnsf-distributed-inference)"
  for flag in "${cflags[@]}"; do
    if [[ -n "$repo_root" && "$flag" == *"$repo_root"* ]]; then
      echo "SOURCE_TREE_INCLUDE_LEAK:$flag" >&2
      exit 4
    fi
    case "$flag" in
      */ndn-svs|*/ndn-svs/*|*/ndn-svs/build|*/ndn-svs/build/*)
        echo "UNINSTALLED_NDNSF_SVS_INCLUDE_LEAK:$flag" >&2
        exit 4 ;;
    esac
  done
  local library
  library=$(find "$package_libdir" -maxdepth 1 -name 'libndnsf-distributed-inference.so*' -type f | sort | head -n 1)
  test -n "$library"
  local svs_library svs_libdir
  svs_libdir=$(pkg-config --variable=libdir libndn-svs)
  svs_library=$(find "$svs_libdir" -maxdepth 1 -name 'libndn-svs.so*' -type f | sort | head -n 1)
  test -n "$svs_library"
  local name="${mode}-${sanitizer}"
  # The ONNX headers the DI SDK installs are generated protobuf headers, and `onnx_pb.h` selects
  # `onnx-ml.pb.h` only when ONNX_ML is defined. The Waf target already passes both defines
  # (tests/installed-api/wscript: ONNX_DEFINES); without them every onnx-including installed header
  # fails to compile against an SDK that is in fact complete.
  local args=(-std=c++17 -Wall -Wextra -Werror -DONNX_ML=1 -DONNX_NAMESPACE=onnx)
  local compiler=${CXX:-g++}
  [[ "$mode" == enabled ]] && args+=(-DNDNSF_DI_ENABLE_ONNXRUNTIME_CPP)
  if [[ "$sanitizer" == asan ]]; then
    args+=(-fsanitize=address -fno-omit-frame-pointer)
  fi
  local matrix_dir="$pkg_root/header-matrix"
  mkdir -p "$matrix_dir"
  local header_index=0 header_failures=0
  for header in "${installed_headers[@]}"; do
    local stem
    stem=$(printf '%s' "$header" | tr '/.' '__')
    local header_source="$matrix_dir/$stem.cpp"
    local header_log="$matrix_dir/$stem.log"
    printf '#include "%s"\nint main() { return 0; }\n' "$header" >"$header_source"
    if ! "$compiler" "${args[@]}" "${cflags[@]}" -fsyntax-only "$header_source" >"$header_log" 2>&1; then
      echo "STANDALONE_HEADER_FAIL:$header" >&2
      header_failures=$((header_failures + 1))
      continue
    fi
    header_index=$((header_index + 1))
  done
  echo "standalone_headers_pass=$header_index" >"$pkg_root/header-matrix.summary"
  echo "standalone_headers_fail=$header_failures" >>"$pkg_root/header-matrix.summary"
  if [[ "$header_failures" -ne 0 ]]; then
    return 6
  fi
  local negative_source="$matrix_dir/excluded-native-request-envelope.cpp"
  local negative_log="$matrix_dir/excluded-native-request-envelope.log"
  printf '#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"\nint main() { return 0; }\n' >"$negative_source"
  if "$compiler" "${args[@]}" "${cflags[@]}" -fsyntax-only "$negative_source" >"$negative_log" 2>&1; then
    echo "EXCLUDED_HEADER_NEGATIVE_GATE_MISSED" >&2
    return 7
  fi
  grep -F 'NativeRequestEnvelope.hpp' "$negative_log" >/dev/null
  echo "excluded_header_negative=PASS" >>"$pkg_root/header-matrix.summary"
  "$compiler" "${args[@]}" "${cflags[@]}" "$source_file" "${libs[@]}" \
    -Wl,-rpath,"$package_libdir" -o "$pkg_root/$name"
  readelf -d "$pkg_root/$name" >"$pkg_root/$name.readelf"
  LD_LIBRARY_PATH="$package_libdir" \
    ldd "$pkg_root/$name" >"$pkg_root/$name.ldd"
  grep -F 'NEEDED' "$pkg_root/$name.readelf" | grep -F 'libndnsf-distributed-inference.so' >/dev/null
  local loaded_library
  loaded_library=$(awk -v dir="$package_libdir/" \
    '$1 ~ /^libndnsf-distributed-inference\.so/ && $2 == "=>" && index($3, dir) == 1 { print $3; exit }' \
    "$pkg_root/$name.ldd")
  test -n "$loaded_library" && test -f "$loaded_library"
  test "$(readlink -f "$library")" = "$(readlink -f "$loaded_library")"
  local loaded_core
  loaded_core=$(awk -v dir="$package_libdir/" \
    '$1 ~ /^libndn-service-framework\.so/ && $2 == "=>" && index($3, dir) == 1 { print $3; exit }' \
    "$pkg_root/$name.ldd")
  test -n "$loaded_core" && test -f "$loaded_core"
  local loaded_svs
  loaded_svs=$(awk -v dir="$svs_libdir/" \
    '$1 ~ /^libndn-svs\.so/ && $2 == "=>" && index($3, dir) == 1 { print $3; exit }' \
    "$pkg_root/$name.ldd")
  test -n "$loaded_svs" && test -f "$loaded_svs"
  test "$(readlink -f "$svs_library")" = "$(readlink -f "$loaded_svs")"
  {
    echo "selected_library=$library"
    echo "loaded_library=$loaded_library"
    sha256sum "$loaded_library"
    echo "loaded_core_library=$loaded_core"
    sha256sum "$loaded_core"
    echo "selected_svs_library=$svs_library"
    echo "loaded_svs_library=$loaded_svs"
    sha256sum "$loaded_svs"
  } >"$pkg_root/library.sha256"
  LD_LIBRARY_PATH="$package_libdir" \
    "$pkg_root/$name" >"$pkg_root/$name.log" 2>&1
  echo "$name PASS prefix=$prefix library=$(cat "$pkg_root/library.sha256")"
}

if [[ -n "$single_backend" ]]; then
  echo "qualification_scope=single-normal-$single_backend prefix=$disabled_normal"
  run_one "$single_backend" normal "$disabled_normal"
  exit 0
fi

# The disabled prefixes must be real ONNX-disabled installations. They are not synthesised here: a
# prefix built with the backend enabled would make the "disabled" case assert the wrong layer, which
# is how this check first reported EXIT=3 against an enabled library.
if [[ "$(realpath "$disabled_normal")" == "$(realpath "$enabled_normal")" ||
      "$(realpath "$disabled_asan")" == "$(realpath "$enabled_asan")" ]]; then
  echo "BACKEND_VARIANT_PREFIX_REUSED: distinct enabled and disabled installations are required" >&2
  exit 5
fi
run_one disabled normal "$disabled_normal"
run_one disabled asan "$disabled_asan"
run_one enabled normal "$enabled_normal"
run_one enabled asan "$enabled_asan"
