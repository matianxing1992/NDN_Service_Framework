# Spec170 build-closure preflight correction — 2026-08-14

## Finding

The earlier image-build failures were not caused by MiniNDN. They were sealed
release-closure failures: the two ONNX smoke targets did not carry identical
`NDN_CXX`/`NDN_SVS` dependencies, later Python profiles could package stale
`build/lib` output, and the manually enumerated native Provider targets did not
declare the dynamic-loader library explicitly. A host linker may resolve
`dlopen`/`dlsym` transitively while the sealed linker does not; that difference
must be rejected before dispatch.

## Correction

`packaging/ndnsf-di-container/oci/scripts/preflight-gpu-build.py` now:

- scans every `bld.program(...)` call in `examples/wscript` with balanced
  parentheses and records the target name and explicit `use=` list;
- requires `BOOST NDN_CXX NDN_SVS ONNXRUNTIME DL` for both ONNX smoke targets
  and both native Provider targets;
- verifies the ONNX smoke source files exist; and
- fails closed when a Python profile contains a generated `build` directory
  without an explicit Docker cleanup command; the report records any source
  trees that the image will remove.

The corresponding `examples/wscript` targets now declare `DL` explicitly.

## Checks

The isolated candidate reports `WAF_CLOSURE_PASS targets=32`. The focused
regression file `tests/python/test_spec170_build_closure_preflight.py` passes
4/4 tests. The full preflight accepts the current checkout's generated trees
only because `Dockerfile.gpu` contains the explicit cleanup command. Removing
that command produces the expected negative marker:

```text
PREFLIGHT_STALE_PYTHON_BUILD_CLEANUP_MISSING:.../app/build,.../planner/build,.../sdk/build
```

Those generated trees must be removed before profile installation. This
commit is a source/preflight correction only; the already materialized SIF
bound to `920552ec...` is unchanged and must not be presented as containing
this correction. A new sealed OCI/SIF identity is required before using this
preflight change for a formal Tiger acceptance gate.
