#!/usr/bin/env bash
# Spec183 T008 host-unit build: clean dependency/native/Python closure.
#
# Builds NAC-ABE + NDN-SVS -> NDNSD -> NDNSF Core -> the two Python
# extensions into one fresh root with at most -j4 per build tree, recording
# every step.  This is host qualification evidence, never a runtime PASS.
set -euo pipefail

ROOT=/tmp/t008-build-root
LOG=/home/tianxing/NDN/ndn-service-framework/Experiments/TigerCluster/.cache/t008-host-build
NAC=/home/tianxing/NDN/NAC-ABE
SVS=/home/tianxing/NDN/ndn-svs
SD=/home/tianxing/NDN/NDNSD
NS=/home/tianxing/NDN/ndn-service-framework
J=4
# The pinned Experimental ndn-svs checks for Boost >= 1.71 (its 1.74 gate
# exists only on other lines); Ubuntu 20.04 ships 1.71, so the system Boost
# is the matching toolchain -- no isolated Boost prefix.

mkdir -p "$LOG"
# Waf build trees are incremental; a dependency ABI change otherwise keeps
# stale linked objects (a libnac-abe symbol change left the Core .so with an
# undefined clearCache reference).  Recreate every build tree each run.
rm -rf "$ROOT" /tmp/t008-nac-build "$SVS/build" "$SD/build" "$NS/build"
mkdir -p "$ROOT/lib/pkgconfig" "$ROOT/bin" "$ROOT/include"
export PKG_CONFIG_PATH="$ROOT/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export CXXFLAGS="-O2 -fPIC"
export CFLAGS="-O2 -fPIC"

step() { echo "==== $1 $(date +%T)"; }

{
step "nac-abe cmake"
# GCC 9.4 ICEs at -O2 on the ABE cipher-text translation unit; -O1 is the
# stable host toolchain setting for this dependency.
cmake -S "$NAC" -B /tmp/t008-nac-build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$ROOT" -DCMAKE_CXX_FLAGS="-O1 -fPIC"
cmake --build /tmp/t008-nac-build --parallel $J
cmake --install /tmp/t008-nac-build

step "ndn-svs waf"
( cd "$SVS" && ./waf configure --prefix="$ROOT" \
  && ./waf build -j$J && ./waf install )

step "ndnsd waf"
( cd "$SD" && ./waf configure --prefix="$ROOT" && ./waf build -j$J && ./waf install )

step "ndnsf core waf"
( cd "$NS" && ./waf configure --prefix="$ROOT" \
  --nac-abe-prefix="$ROOT" --disable-local-dependency-prefix \
  && ./waf build -j$J && ./waf install )

step "pythonWrapper extension"
# The pybind11 translation unit is too large for GCC 9.4's debug emission
# (leb128 assembler error); distutils reads CFLAGS, not CXXFLAGS, and -g0
# overrides the sysconfig -g.  The explicit closure env binds the exact
# candidate libraries instead of whatever -lnac-abe/-lndn-svs resolve to.
# distutils build/ is incremental: without removing it, a changed closure
# keeps the previously linked extension (and its old RUNPATH).
rm -rf "$NS/pythonWrapper/build"
( cd "$NS/pythonWrapper" \
  && CFLAGS="-O2 -fPIC -g0" \
  NDNSF_LIBRARY_DIR="$ROOT/lib" \
  NDNSF_NAC_ABE_PREFIX="$ROOT" \
  NDNSF_NDN_SVS_SOURCE_TREE="$SVS" \
  NDNSF_NDN_SVS_BUILD_TREE="$SVS/build" \
  python3 setup.py build_ext --inplace )

step "py_repoclient extension"
rm -rf "$NS/NDNSF-DistributedRepo/pythonWrapper/build"
( cd "$NS/NDNSF-DistributedRepo/pythonWrapper" \
  && PKG_CONFIG_PATH="$ROOT/lib/pkgconfig" python3 setup.py build_ext --inplace )

step "entrypoint smoke"
"$NS/build/App_ServiceController" --help >/dev/null 2>&1 || true
python3 -c "import sys; sys.path.insert(0, '$NS/pythonWrapper'); import ndnsf._ndnsf; print('NDNSF_EXT_OK')"
} >"$LOG/build.log" 2>&1
echo "T008_HOST_BUILD_EXIT=$?" | tee "$LOG/exit.txt"
