# T008 Host-Unit Qualification Evidence

**Status**: BUILD PASS (component level; not runtime qualification)
**Date**: 2026-09-07
**Driver**: `Experiments/TigerCluster/tools/spec183_host_build.sh`
**Constraint**: clean build root `/tmp/t008-build-root`, at most `-j2` per
active build tree, isolated install prefix, every build tree recreated per run.

## Build sequence

| Stage | Command | Status |
|-------|---------|--------|
| NAC-ABE (Experimental 5ed23e68) | cmake `-O1 -fPIC`, `--parallel 2`, install | PASS |
| NDN-SVS (Experimental 9f2d8a47, system Boost 1.71) | `./waf configure --prefix`, `build -j2`, `install` | PASS |
| NDNSD (Experimental 375a35c5) | `./waf configure --prefix`, `build -j2`, `install` | PASS |
| NDNSF Core | `./waf configure --nac-abe-prefix`, `build -j2`, `install` | PASS |
| pythonWrapper ext (`_ndnsf`) | explicit closure env, `build_ext --inplace` | PASS |
| py_repoclient ext (`_py_repoclient`) | `PKG_CONFIG_PATH` clean root, `build_ext --inplace` | PASS |

## Toolchain notes (recorded per user ruling in AGENTS.md)

- NDN-SVS: the pinned Experimental revision declares Boost >= 1.71; the host
  system Boost 1.71 (Ubuntu 20.04) is the matching toolchain.  No isolated
  Boost prefix, no version-gate patch.
- NAC-ABE: GCC 9.4 ICEs at -O2 on the ABE cipher-text translation unit; -O1
  is the stable setting.
- pythonWrapper: `-g0` (via CFLAGS, not CXXFLAGS) dodges the GCC 9.4 leb128
  assembler failure on the large pybind11 unit; sources are relative to
  setup.py so pip-based installs accept them (spec181 T002 regression fix).
- The extensions link the exact candidate libraries through the maintained
  explicit-closure env (NDNSF_LIBRARY_DIR / NDNSF_NAC_ABE_PREFIX /
  NDNSF_NDN_SVS_*), never a same-SONAME installed library.

## Gates

- [x] ldd closure of both extensions resolves nac/svs/ndnsd/framework to
      `/tmp/t008-build-root/lib` and ndn-cxx to the system `/usr/local/lib`;
      0 unresolved symbols.
- [x] both Python extensions import outside any source tree
      (`NDNSF_EXT_OK`, `PY_REPOCLIENT_EXT_OK`).
- [x] clean-root symbol inventory: core 1567 defined symbols, nac-abe 132.
- [ ] `App_ServiceController --help` entrypoint executes on host (T009 scope)
- [ ] NDNSF unit tests pass in the clean root (T009 scope)

## Record

Build log: `Experiments/TigerCluster/.cache/t008-host-build/build.log`
(exit 0, `T008_HOST_BUILD_EXIT=0`).
