# Spec186 native/build closure checkpoint 1

**Date:** 2026-09-12
**Baseline:** `575b43cc93bbed29932303caf3d09974f1585af7`
**Verdict:** `BLOCKED_AFTER_BOUNDARY`

## Build attempts

| Command | Result | First boundary |
| --- | --- | --- |
| `./waf list` | failed before graph creation | absent optional `standalone/spec182-worker-tools/*.cpp` was passed as `None` |
| `./waf build -j2` after optional guard | failed at `ndn-service-framework/ServiceUser.cpp` | selected `/tmp/t008-build-root` NAC-ABE headers lack the methods used by this source |
| `./waf configure --nac-abe-prefix=/tmp/spec186-nacabe-install ...` | failed before build | no official ONNX 1.17 full-protobuf prefix (`checker.h`, `libonnx.a`, `libonnx_proto.a`) is available |

The optional fixture guard is now committed in the test build description. The
matching NAC-ABE source was identified at `/home/tianxing/NDN/NAC-ABE` and a
temporary install was materialized at `/tmp/spec186-nacabe-install`; its
headers contain the expected refresh/cache API. The next clean build still
requires an ONNX 1.17 full-protobuf prefix and matching ONNX Runtime.

## Existing artifacts (not promoted)

| Artifact | Observation | Decision |
| --- | --- | --- |
| `build/examples/di-native-provider` | `--help` exits 127 with unresolved `DeploymentControlMessage` vtable | reject as runtime candidate |
| `pythonWrapper/build/.../_ndnsf.cpython-38-x86_64-linux-gnu.so` | import selects `/usr/local/lib/libndn-service-framework.so.0.1.0` and fails on unresolved `ndnsd::discovery` symbols | reject; host loader closure is not sealed |
| same extension `ldd -r` | `RUNPATH` contains `/home/tianxing/NDN/ndn-svs/build:/tmp/t008-build-root/lib` and reports unresolved framework/SVS symbols | rebuild with packaged matching libraries |
| cached SIF `base-runtime-controller-version-j4-v22-stable-20260909.sif` | SHA-256 `2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5` | do not reuse until source/ABI seal is proven |

No pre-existing binary, SIF or old Spec183 evidence is used to close Spec186.
T006 remains open until the clean dependency build, `import`, `--help`,
`readelf -d`, `ldd -r`, RPATH and SONAME checks pass for one candidate tuple.
