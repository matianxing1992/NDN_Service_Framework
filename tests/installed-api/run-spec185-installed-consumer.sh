#!/usr/bin/env bash
set -euo pipefail

disabled_normal=${1:?usage: $0 <disabled-normal-prefix> <disabled-asan-prefix> <enabled-normal-prefix> <enabled-asan-prefix> [output-dir]}
disabled_asan=${2:?usage: $0 <disabled-normal-prefix> <disabled-asan-prefix> <enabled-normal-prefix> <enabled-asan-prefix> [output-dir]}
enabled_normal=${3:?usage: $0 <disabled-normal-prefix> <disabled-asan-prefix> <enabled-normal-prefix> <enabled-asan-prefix> [output-dir]}
enabled_asan=${4:?usage: $0 <disabled-normal-prefix> <disabled-asan-prefix> <enabled-normal-prefix> <enabled-asan-prefix> [output-dir]}
output_dir=${5:-"${TMPDIR:-/tmp}/spec185-installed-api"}
source_file=$(cd "$(dirname "$0")" && pwd)/spec185-installed-consumer.cpp
repo_root=$(git -C "$(dirname "$0")/../.." rev-parse --show-toplevel)
manifest_file="$repo_root/specs/185-prepared-model-runtime/contracts/api-exposure.json"

mapfile -t installed_headers < <(python3 - "$manifest_file" <<'PY'
import json
import sys

manifest = json.load(open(sys.argv[1], encoding="utf-8"))
paths = set(manifest["legacy_install_glob"]["enumerated_headers"])
paths.update(entry["path"] for entry in manifest["entries"])
for path in sorted(path for path in paths if path.endswith(".hpp")):
    print(path)
PY
)
test "${#installed_headers[@]}" -eq 68

mkdir -p "$output_dir"

run_one() {
  local mode=$1 sanitizer=$2 prefix=$3
  prefix=$(cd "$prefix" && pwd)
  local pkg_root="$output_dir/pkg-$mode-$sanitizer"
  mkdir -p "$pkg_root"
  export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$prefix/lib64/pkgconfig"
  unset PKG_CONFIG_PATH
  local package_libdir
  package_libdir=$(pkg-config --variable=libdir ndnsf-distributed-inference)
  case "$package_libdir" in
    "$prefix"/*) ;;
    *) echo "PKG_CONFIG_PREFIX_MISMATCH:$package_libdir" >&2; exit 5 ;;
  esac
  read -r -a cflags <<< "$(pkg-config --cflags ndnsf-distributed-inference)"
  read -r -a libs <<< "$(pkg-config --libs ndnsf-distributed-inference)"
  for flag in "${cflags[@]}"; do
    case "$flag" in
      *"$repo_root"*) echo "SOURCE_TREE_INCLUDE_LEAK:$flag" >&2; exit 4 ;;
      */ndn-svs|*/ndn-svs/*|*/ndn-svs/build|*/ndn-svs/build/*)
        echo "UNINSTALLED_NDNSF_SVS_INCLUDE_LEAK:$flag" >&2
        exit 4 ;;
    esac
  done
  local library
  library=$(find "$package_libdir" -maxdepth 1 -name 'libndnsf-distributed-inference.so*' -type f | sort | head -n 1)
  test -n "$library"
  local svs_library
  svs_library=$(find "$package_libdir" -maxdepth 1 -name 'libndn-svs.so*' -type f | sort | head -n 1)
  test -n "$svs_library"
  local name="${mode}-${sanitizer}"
  local args=(-std=c++17 -Wall -Wextra -Werror)
  [[ "$mode" == enabled ]] && args+=(-DNDNSF_DI_ENABLE_ONNXRUNTIME_CPP)
  if [[ "$sanitizer" == asan ]]; then
    args+=(-fsanitize=address -fno-omit-frame-pointer)
  fi
  local matrix_dir="$pkg_root/header-matrix"
  mkdir -p "$matrix_dir"
  local header_index=0
  for header in "${installed_headers[@]}"; do
    local stem
    stem=$(printf '%s' "$header" | tr '/.' '__')
    local header_source="$matrix_dir/$stem.cpp"
    local header_log="$matrix_dir/$stem.log"
    printf '#include "%s"\nint main() { return 0; }\n' "$header" >"$header_source"
    if ! g++ "${args[@]}" "${cflags[@]}" -fsyntax-only "$header_source" >"$header_log" 2>&1; then
      echo "STANDALONE_HEADER_FAIL:$header" >&2
      return 6
    fi
    header_index=$((header_index + 1))
  done
  echo "standalone_headers_pass=$header_index" >"$pkg_root/header-matrix.summary"
  local negative_source="$matrix_dir/excluded-native-request-envelope.cpp"
  local negative_log="$matrix_dir/excluded-native-request-envelope.log"
  printf '#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"\nint main() { return 0; }\n' >"$negative_source"
  if g++ "${args[@]}" "${cflags[@]}" -fsyntax-only "$negative_source" >"$negative_log" 2>&1; then
    echo "EXCLUDED_HEADER_NEGATIVE_GATE_MISSED" >&2
    return 7
  fi
  grep -F 'NativeRequestEnvelope.hpp' "$negative_log" >/dev/null
  echo "excluded_header_negative=PASS" >>"$pkg_root/header-matrix.summary"
  g++ "${args[@]}" "${cflags[@]}" "$source_file" "${libs[@]}" \
    -Wl,-rpath,"$package_libdir" -o "$pkg_root/$name"
  readelf -d "$pkg_root/$name" >"$pkg_root/$name.readelf"
  LD_LIBRARY_PATH="$package_libdir:${LD_LIBRARY_PATH:-}" \
    ldd "$pkg_root/$name" >"$pkg_root/$name.ldd"
  grep -F 'NEEDED' "$pkg_root/$name.readelf" | grep -F 'libndnsf-distributed-inference.so' >/dev/null
  local loaded_library
  loaded_library=$(awk -v dir="$package_libdir/" \
    '$1 ~ /^libndnsf-distributed-inference\.so/ && $2 == "=>" && index($3, dir) == 1 { print $3; exit }' \
    "$pkg_root/$name.ldd")
  test -n "$loaded_library" && test -f "$loaded_library"
  test "$(readlink -f "$library")" = "$(readlink -f "$loaded_library")"
  local loaded_svs
  loaded_svs=$(awk -v dir="$package_libdir/" \
    '$1 ~ /^libndn-svs\.so/ && $2 == "=>" && index($3, dir) == 1 { print $3; exit }' \
    "$pkg_root/$name.ldd")
  test -n "$loaded_svs" && test -f "$loaded_svs"
  test "$(readlink -f "$svs_library")" = "$(readlink -f "$loaded_svs")"
  {
    echo "selected_library=$library"
    echo "loaded_library=$loaded_library"
    sha256sum "$loaded_library"
    echo "selected_svs_library=$svs_library"
    echo "loaded_svs_library=$loaded_svs"
    sha256sum "$loaded_svs"
  } >"$pkg_root/library.sha256"
  LD_LIBRARY_PATH="$package_libdir:${LD_LIBRARY_PATH:-}" \
    "$pkg_root/$name" >"$pkg_root/$name.log" 2>&1
  echo "$name PASS prefix=$prefix library=$(cat "$pkg_root/library.sha256")"
}

run_one disabled normal "$disabled_normal"
run_one disabled asan "$disabled_asan"
run_one enabled normal "$enabled_normal"
run_one enabled asan "$enabled_asan"
