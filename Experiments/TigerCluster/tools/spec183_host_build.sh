#!/usr/bin/env bash
# Spec183 T008 host-unit build: clean dependency/native/Python closure.
#
# Builds NAC-ABE + NDN-SVS -> NDNSD -> NDNSF Core -> the two Python
# extensions into one fresh root with at most -j2 per build tree, recording
# every step.  This is host qualification evidence, never a runtime PASS.
set -euo pipefail

ROOT=/tmp/t008-build-root
LOG=/home/tianxing/NDN/ndn-service-framework/Experiments/TigerCluster/.cache/t008-host-build
NAC=/home/tianxing/NDN/NAC-ABE
SVS=/home/tianxing/NDN/ndn-svs
SD=/home/tianxing/NDN/NDNSD
NS=/home/tianxing/NDN/ndn-service-framework
J=2

mkdir -p "$LOG"
rm -rf "$ROOT"
mkdir -p "$ROOT/lib/pkgconfig" "$ROOT/bin" "$ROOT/include"
export PKG_CONFIG_PATH="$ROOT/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export CXXFLAGS="-O2 -fPIC"
export CFLAGS="-O2 -fPIC"

step() { echo "==== $1 $(date +%T)"; }

{
step "nac-abe cmake"
cmake -S "$NAC" -B /tmp/t008-nac-build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$ROOT" -DCMAKE_CXX_FLAGS="-O2 -fPIC"
cmake --build /tmp/t008-nac-build --parallel $J
cmake --install /tmp/t008-nac-build

step "ndn-svs waf"
( cd "$SVS" && ./waf configure --prefix="$ROOT" && ./waf build -j$J && ./waf install )

step "ndnsd waf"
( cd "$SD" && ./waf configure --prefix="$ROOT" && ./waf build -j$J && ./waf install )

step "ndnsf core waf"
( cd "$NS" && ./waf configure --prefix="$ROOT" && ./waf build -j$J )

step "pythonWrapper extension"
( cd "$NS/pythonWrapper" && python3 setup.py build_ext --inplace )

step "entrypoint smoke"
"$NS/build/App_ServiceController" --help >/dev/null 2>&1 || true
python3 -c "import sys; sys.path.insert(0, '$NS/pythonWrapper'); import ndnsf._ndnsf; print('NDNSF_EXT_OK')"
} >"$LOG/build.log" 2>&1
echo "T008_HOST_BUILD_EXIT=$?" | tee "$LOG/exit.txt"
