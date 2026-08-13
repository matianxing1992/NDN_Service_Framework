# Runtime closure fix checkpoint (2026-08-13)

The previous GPU-release preflight stopped while scanning Pillow's native
extension because `pillow.libs/` is a sibling of the importing `PIL/`
directory. The closure scanner only added the ELF's immediate parent to
`LD_LIBRARY_PATH`, so it reported the vendored `libwebpmux` DSO as missing
even though it was present in the wheel.

Both `derive-runtime-packages.py` and `verify-runtime-closure.py` now add
ancestor-adjacent directories whose names end in `.libs`, while retaining the
original immediate-parent lookup and fail-closed behavior. A regression test
models `PIL/_imaging.so` plus `pillow.libs/libspec170_wheel.so.1`; direct `ldd`
fails, while both closure paths resolve the DSO. Missing vendored DSOs still
fail closed.

## Verification

```text
PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
python3 -m pytest -q \
  tests/python/test_spec170*.py \
  tests/container/unit/test_spec170*.py \
  tests/container/itiger-qwen-live/unit/test_runtime_package_closure.py
69 passed, 2 skipped, 1 warning in 4.23s
```

This is a build-pipeline correction only. It does not create an OCI image or
SIF, close Gate C, freeze T029, or authorize a TigerCluster D gate.
