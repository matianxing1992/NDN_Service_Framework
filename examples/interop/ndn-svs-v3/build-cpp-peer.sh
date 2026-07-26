#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
NDN_SVS_SOURCE=${NDN_SVS_SOURCE:-/home/tianxing/NDN/ndn-svs}
OUTPUT=${1:-$ROOT/build/svs3-peer}

test -f "$NDN_SVS_SOURCE/ndn-svs/svsync.hpp"
test -f "$NDN_SVS_SOURCE/build/libndn-svs.so"
mkdir -p "$(dirname "$OUTPUT")"

read -r -a NDN_CXX_FLAGS <<<"$(pkg-config --cflags --libs libndn-cxx)"
g++ -std=c++17 -Og -g -fsanitize=address -fno-omit-frame-pointer \
  -I"$NDN_SVS_SOURCE/ndn-svs" \
  -I"$NDN_SVS_SOURCE/build" \
  "$ROOT/cpp/svs3-peer.cpp" \
  -L"$NDN_SVS_SOURCE/build" -Wl,-rpath,"$NDN_SVS_SOURCE/build" \
  -lndn-svs "${NDN_CXX_FLAGS[@]}" -o "$OUTPUT"

printf '%s\n' "$OUTPUT"
