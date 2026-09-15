# NDNSF build dependency boundaries

## Audit scope

This is the Spec186 static dependency audit performed on 2026-09-15. It maps
the current root `wscript`, `examples/wscript`, Python package metadata and the
Qwen tokenizer bridge. It describes the build graph that exists today; it is
not a claim that a Core-only build profile already works.

## Confirmed current graph

| Boundary | Current source/target | Required inputs today | Finding |
| --- | --- | --- | --- |
| Framework Core | root `wscript`: `ndn-service-framework` | NDN-CXX, NDN-SVS, Boost, Protobuf, NAC-ABE, NDNSD, OpenSSL, `libdl` | A coherent native Core link closure. |
| DI mechanism and adapters | root `wscript`: `ndnsf-distributed-inference` | DI mechanism sources plus all ONNX, YOLO and Qwen adapter sources; the shared target also links the Rust tokenizer bridge | The installable native DI library is monolithic across model adapters. |
| ONNX native adapter | `ndnsf-di-adapter-onnx-objects` and ONNX runner/assembler consumers | ONNX full-protobuf headers and archives, ONNX Runtime when available, Protobuf, Boost and NDN-CXX | ONNX is a real native link boundary, not merely a Python packaging dependency. |
| YOLO native adapter | `ndnsf-di-adapter-yolo-objects` and `NativeYoloMergeRunner` consumers | Boost and NDN-CXX, plus the DI library when linked by an executable | YOLO itself does not add ONNX at the adapter object group, but current DI and example targets can still pull the monolithic library. |
| Qwen native adapter | `ndnsf-di-adapter-qwen-objects` and Qwen session consumers | Boost plus the Rust tokenizer static archive for the shared DI library | The Rust bridge is statically linked; it is not a runtime `dlopen` library. |
| Native assembly worker | `examples/wscript`: `di-native-assembly-worker` | ONNX full-protobuf, ONNX Runtime, NDN-CXX, NDN-SVS, Protobuf, Boost, OpenSSL and `libdl` | It is created before the `WITH_EXAMPLES` return, so the current examples recursion does not provide a Core-only escape. |
| Python Core/SDK | `packaging/python/core` and `sdk` | Core has no third-party runtime dependency; SDK depends on the matching Core package | Python metadata expresses a smaller, separable boundary. |
| Python ONNX/Qwen adapters | `packaging/python/adapters/onnx` and `qwen` | ONNX adapter: `onnx` plus optional CPU/GPU ONNX Runtime; Qwen adapter: `tokenizers` | These are explicit optional application profiles, but they do not yet control the root C++ Waf graph. |

Two unconditional checks explain why a request that appears to be Core-only
can still fail on DI inputs: `configure()` requires an explicit ONNX
full-protobuf prefix, and it calls `_ensure_tokenizer_bridge()` without a
profile guard. The build then always declares the DI object groups and recurses
into `examples`; the assembly worker is declared before its examples guard.
The checks are therefore detecting real current coupling, rather than inventing
dependencies from the SIF layer.

## Required delivery rule now

Until build profiles are implemented, do not delete or weaken the ONNX, Rust,
Protobuf or assembly-worker checks. The selected Spec186 candidate must declare
the exact prefixes, Cargo cache, source seal and resulting native targets in its
manifest. The Rust toolchain and Cargo cache are supplied explicitly through
`NDNSF_RUST_PREFIX` and `NDNSF_CARGO_HOME`; no `.codex-tmp/spec182…` path is a
valid default. The build preflight must verify the definition's explicit
`SPEC186_BASE_CAPABILITY_BEGIN/END` block and every builder source path
before native compilation. Predicates outside that block may validate tools
installed by the builder itself (for example `clang-10` from APT) and are
not base requirements.

## Follow-up design (plan only)

The next packaging change should introduce explicit target profiles with an
ownership table generated from the Waf target graph:

1. `core`: framework Core and its direct ABI closure;
2. `di-core`: model-independent DI mechanism and its native ABI closure;
3. `di-onnx`: ONNX adapter, full-protobuf SDK and CPU/GPU ONNX Runtime;
4. `di-qwen`: Qwen adapter, explicitly sealed Rust tokenizer bridge and its
   Cargo inputs;
5. `app/experiments`: native workers, Python wheels, launchers and model data.

Each profile must list source files, generated headers, pkg-config/prefix
inputs, static/shared libraries, RPATH/`ldd` closure, installed artifacts and
the gate that proves them. A profile may omit a dependency only after the Waf
target graph and an actual clean build prove that no selected source or target
consumes it. The manifest should be generated from the selected target graph so
that packaging cannot silently lag behind source evolution.

This follow-up is deliberately not implemented in the current Spec186 repair;
making it a code change requires a new SpecKit task and a clean rebuild of all
affected consumers.
