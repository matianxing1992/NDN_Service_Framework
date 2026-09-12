#!/usr/bin/env bash
set -euo pipefail

prefix=${1:?usage: $0 <installed-prefix> [output-dir]}
output_dir=${2:-"${TMPDIR:-/tmp}/spec185-core-operation-consumer"}
prefix=$(cd "$prefix" && pwd)
source_file=$(cd "$(dirname "$0")" && pwd)/core-operation-consumer.cpp
repo_root=$(git -C "$(dirname "$0")/../.." rev-parse --show-toplevel)
pkg_root="$output_dir/installed"
mkdir -p "$pkg_root"

export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$prefix/lib64/pkgconfig"
unset PKG_CONFIG_PATH
package_libdir=$(pkg-config --variable=libdir libndn-service-framework)
case "$package_libdir" in
  "$prefix"/*) ;;
  *) echo "PKG_CONFIG_PREFIX_MISMATCH:$package_libdir" >&2; exit 5 ;;
esac
read -r -a cflags <<< "$(pkg-config --cflags libndn-service-framework)"
read -r -a libs <<< "$(pkg-config --libs libndn-service-framework)"
for flag in "${cflags[@]}"; do
  case "$flag" in
    *"$repo_root"*) echo "SOURCE_TREE_INCLUDE_LEAK:$flag" >&2; exit 4 ;;
    */ndn-svs|*/ndn-svs/*|*/ndn-svs/build|*/ndn-svs/build/*)
      echo "UNINSTALLED_NDNSF_SVS_INCLUDE_LEAK:$flag" >&2; exit 4 ;;
  esac
done
library=$(find "$package_libdir" -maxdepth 1 -name 'libndn-service-framework.so*' -type f | sort | head -n 1)
test -n "$library"
g++ -std=c++17 -Wall -Wextra -Werror "${cflags[@]}" "$source_file" "${libs[@]}" \
  -Wl,-rpath,"$package_libdir" -o "$pkg_root/spec185-core-operation-consumer"
readelf -d "$pkg_root/spec185-core-operation-consumer" > "$pkg_root/readelf"
LD_LIBRARY_PATH="$package_libdir:${LD_LIBRARY_PATH:-}" \
  ldd "$pkg_root/spec185-core-operation-consumer" > "$pkg_root/ldd"
grep -F 'NEEDED' "$pkg_root/readelf" | grep -F 'libndn-service-framework.so' >/dev/null
loaded=$(awk -v dir="$package_libdir/" \
  '$1 ~ /^libndn-service-framework\.so/ && $2 == "=>" && index($3, dir) == 1 { print $3; exit }' \
  "$pkg_root/ldd")
test -n "$loaded" && test "$(readlink -f "$library")" = "$(readlink -f "$loaded")"
LD_LIBRARY_PATH="$package_libdir:${LD_LIBRARY_PATH:-}" \
  "$pkg_root/spec185-core-operation-consumer" > "$pkg_root/run.log" 2>&1
echo "Spec185CoreOperationConsumer PASS prefix=$prefix library=$loaded"
