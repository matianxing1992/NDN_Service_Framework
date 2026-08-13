# ONNX Runtime Python lock correction (2026-08-13)

The first current-Experimental OCI diagnostic reached the Python dependency
install and failed with:

```text
ERROR: Could not find a version that satisfies the requirement onnxruntime-gpu==1.20.1
```

The official PyPI JSON index was checked directly. `onnxruntime-gpu` has no
`1.20.1` release; the CPython 3.10 x86-64 wheel for `1.20.0` is published
with SHA-256
`601e2acd192b4d66ed4d82a9a2fd8c1546fc3400cf9d202e36c0e5a7cef843d0`.

The lock now records `onnxruntime-gpu==1.20.0` in both
`oci/locks/gpu.lock` and `oci/layered/locks/ml-runtime.lock.json`. The native
ONNX Runtime C++ SDK remains pinned to 1.20.1 with its existing tarball digest;
the Python wheel and native SDK are distinct locked inputs and must not be
pretended to share an unavailable patch release.

This correction has not yet been rebuilt in GitHub Actions. It does not close
Gate C or authorize a TigerCluster D gate.
