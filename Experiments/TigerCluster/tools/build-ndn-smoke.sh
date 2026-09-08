#!/usr/bin/env bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/local/bin
[[ $# == 1 ]] || { echo 'usage: build-ndn-smoke.sh OUTPUT_DIRECTORY' >&2; exit 2; }
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p -- "$1"
out=$(cd -- "$1" && pwd)
[[ ! -e "$out/ndn-smoke" ]] || { echo 'output binary already exists' >&2; exit 2; }
pkg-config --modversion libndn-cxx > "$out/ndn-cxx-version.txt"
# Link only direct ndn-cxx symbols. Its transitive libraries come from the SIF
# at execution; in-image ldd and a real request/response must verify this ABI.
read -r -a flags <<< "$(pkg-config --cflags libndn-cxx)"
read -r -a library_dirs <<< "$(pkg-config --libs-only-L libndn-cxx)"
/usr/bin/g++ -B/usr/bin/ -std=c++17 -O2 -Wall -Wextra "${flags[@]}" "$root/apps/ndn-smoke.cpp" \
  "${library_dirs[@]}" -lndn-cxx -pthread \
  -Wl,-rpath,/opt/ndnsf-di/current/lib -o "$out/ndn-smoke"
sha256sum "$root/apps/ndn-smoke.cpp" "$out/ndn-smoke" > "$out/build-sha256.txt"
g++ --version > "$out/compiler.txt"
readelf -d "$out/ndn-smoke" > "$out/dynamic.txt"
