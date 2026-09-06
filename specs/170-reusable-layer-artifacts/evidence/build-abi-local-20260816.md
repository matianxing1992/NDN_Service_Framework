# Spec 170 local build and ABI evidence (2026-08-16)

This record covers the current checkout after the native NDNSF_DATA_V1 and
provider-target closure changes.  It is local evidence only; it does not
promote the older Tiger SIF.

## Toolchain

```text
NDN-SVS prefix: .codex-tmp/spec173-toolchain/ndn-svs-prefix
NDN-SVS commit: 1d432a5b1ffde64544963a86fda9163ccc26610e
Boost: .local-boost171 (1.71)
ONNX Runtime: /opt/onnxruntime -> /opt/onnxruntime-1.26.0
Python: 3.8.10
```

The Waf configure/build environment used the Experimental NDN-SVS pkg-config
files and Boost 1.71.  The seven Spec170 DI targets completed successfully in
8m42.181s:

```text
di-native-plan-manifest-smoke
di-native-plan-schema-smoke
di-native-plan-onnx-smoke
di-native-onnxruntime-smoke
di-native-provider
di-native-fault-provider
di-native-provider-session-smoke
```

The all-examples build is not a pass: it reaches the unrelated host
`UavDroneApp` GTK/GLib linker failure.  The DI target closure above is
independently complete.

## Python/native closure

The first in-place build was rejected because the pre-existing
`pythonWrapper/build` directory is root-owned.  A second build used only
temporary user-owned `--build-temp` and `--build-lib` paths and copied the
resulting extension into the package:

```text
extension: pythonWrapper/ndnsf/_ndnsf.cpython-38-x86_64-linux-gnu.so
extension sha256: d6181df0a87bcc321c8879b1eb8c270763a38a712335b5cdec86853e44b3f2b4
native library: build/libndn-service-framework.so
native sha256: c1014840301f3bd229955e64e8dc29e2a1f1be85e60cc012f12cf3d2cc9502fd
```

Import and symbol probe:

```text
python 3.8.10
module .../pythonWrapper/ndnsf/_ndnsf.cpython-38-x86_64-linux-gnu.so
has_predictive_name True
```

`ldd` resolves NDN-SVS from the Experimental prefix, Boost 1.71, the current
native library, NDN-CXX, NAC-ABE, and ndnsd; it reports no `not found` entry.
`readelf -d` reports a RUNPATH to `build`, and no stale `VERS_1.20.1` string was
found.  The extension's required native libraries include
`libndn-service-framework.so.0.1.0`, `libndn-cxx.so.0.9.0`, `libnac-abe.so`,
and `libndnsd.so`.

The runner/import gate passed for:

```text
python3 packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-qwen-reference.py --help
bash packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-ndnsf-qwen.sh --help
```

The first command returned 0 and printed its argparse interface.  The second
returned 2 because the shell wrapper intentionally requires its six runtime
paths; that is an expected argument-validation response, not an import or
link failure.  The package module probe
`python3 -m ndnsf_distributed_inference.qwen_pilot --help` returned 0.

## Regression results

```text
python3 -m pytest -q tests/python/test_spec170_*.py
58 passed, 2 skipped, 1 warning in 6.09s

./build/unit-tests --log_level=test_suite
479 test cases; *** No errors detected

./build/integration-tests --log_level=test_suite
12 test cases; *** No errors detected
```

The two Python skips are explicit opt-in real-Qwen/multi-request and
real-MiniNDN markers.  They are not silently counted as passes.

## Preflight

`preflight-gpu-build.py` returned `status: PASS`; the exact JSON is retained in
`preflight-gpu-build-20260816.json`.  It enumerated 32 native targets, 38
Python packages, 4 repository targets, and 11 system CUDA requirements.

## Boundary

These checks establish a reproducible local source/toolchain closure.  They do
not yet prove a new sealed SIF or a TigerCluster multi-Provider workload.  A
new source seal and one local SIF must be produced before the next Tiger run.
