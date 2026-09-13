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

## Checkpoint 2 — 2026-09-12 temporary ONNX probe

The installed Python `onnx` 1.17.0 package supplied the full-protobuf headers
and sources. With the system `protoc 3.6.1`, a corrected generation of
`onnx-ml.proto`, `onnx-data.proto` and `onnx-operators-ml.proto` produced three
translation units. A bounded `make -j4` probe then built temporary
`libonnx_proto.a` (1.9 MB) and `libonnx.a` (47 MB) under
`/tmp/spec186-onnx-build4`; the probe used host-protoc enum compatibility
helpers and is not a packaged or source-sealed dependency input.

Reconfiguration with `/tmp/spec186-onnx-prefix2` and
`/tmp/spec186-nacabe-install` passed ONNX, NDN-SVS, protobuf, ONNX Runtime and
NAC-ABE checks, then stopped at the required pinned Rust tokenizer bridge:
`.codex-tmp/spec182-t001-dependencies/rust-prefix/bin/cargo` and its offline
cargo home are absent. The repository Waf build therefore still has no
candidate binary to inspect. This closes the ONNX *file-shape* probe only; it
does not change the `BLOCKED_AFTER_BOUNDARY` verdict.

## Checkpoint 3 — 2026-09-12 NAC-ABE export probe

Forcing the temporary NAC-ABE library ahead of `/usr/local/lib` did not close
the extension. `_ndnsf.so` still failed at
`ndn::nacabe::Consumer::clearCache(...)`; `ldd -r` reported the corresponding
refresh/public-parameter and policy-rotation symbols as unresolved. `nm -D`
showed that the temporary library exports no definitions for those header
declarations. The prefix therefore combines newer headers with an older
library and is rejected. A same-revision NAC-ABE rebuild is required before
the Waf build can produce a candidate.

## Checkpoint 4 — 2026-09-12 pinned Rust toolchain and Cargo cache

The exact Rust 1.90.0 `rustc`, `cargo` and `rust-std` archives from the
Spec182 dependency contract were downloaded into the ignored `.codex-tmp`
workspace and matched their locked SHA-256 values. The official installers
produced an independent `rust-prefix-r3`; `rustc -vV` reports
`x86_64-unknown-linux-gnu` and release `1.90.0`.

Reconfiguration with the matching `.deps/nac-abe-spec179-official` headers and
library plus the temporary ONNX probe reached the tokenizer bridge, then
stopped before compilation because the isolated Cargo home contains no
`tokenizers` package and offline resolution cannot satisfy `Cargo.lock`.
This remains a dependency boundary: no candidate binary or runtime evidence
is promoted, and T006 stays `BLOCKED_AFTER_BOUNDARY` pending cache recovery.

## Checkpoint 5 — 2026-09-12 NDN-SVS source/build pairing

The first targeted C++ build reached `ServiceProvider.cpp` and exposed an
installed-version mismatch: `/usr/local/include` and `/usr/local/lib` provide
the older `SVSPubSub::subscribeToProducer` API, while the Spec186 source calls
`subscribeToProducerWithCatchUp`. The maintained NDN-SVS source/build pair at
`/home/tianxing/NDN/ndn-svs` declares and exports the required method.

Waf was reconfigured with both explicit options:

```text
--ndn-svs-source-tree=/home/tianxing/NDN/ndn-svs
--ndn-svs-build-tree=/home/tianxing/NDN/ndn-svs/build
```

Its explicit header/library closure check passed, and the resumed targeted
build reached 24/97 before this checkpoint. No runtime candidate is promoted
until the shared libraries link and pass import, `--help`, RPATH and `ldd -r`.
