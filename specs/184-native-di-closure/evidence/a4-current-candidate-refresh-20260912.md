# Spec184 T007 A4 Current Candidate Refresh

**Date**: 2026-09-12  
**Status**: `PARTIAL` / source review and same-tree build passed; current YOLO qualification must be rerun

## Scope

This checkpoint reviews the pending native changes in the working tree that affect
registration lifetime, request-scoped Provider identity binding, collaboration dispatch,
key cleanup, and native process shutdown. The review is read-only with respect to the
parallel changes; the only additional fixes in this checkpoint are the weak registration
owner guard in `utils.cpp`, the direct `<memory>` include in `utils.hpp`, and the empty-buffer
guard in `ServiceUser.cpp`.

The registration retry callback previously retained a strong handle while also retaining
callbacks that captured the owning object. That could keep a failed registration alive after
the owner had been destroyed and invoke `this` from a delayed retry. The callback now keeps a
`weak_ptr`, checks it before constructing a timer, and locks it before retrying. Empty request
input now returns the no-discovery-payload path without constructing a string from a possibly
null buffer. The Provider-specific input name and per-Provider key state remain bound to the
same authenticated selection identity.

The installed `review-agent` read the complete diff and surrounding call sites. No remaining
actionable defect was found after these two corrections. The review did not claim that a
standalone destructor race reproducer or a full process qualification had run; those remain
runtime evidence gaps.

## Same-tree build and receipt

The affected native targets were rebuilt from `build-spec184-b5-candidate-r4` with the
registered Waf names and `/usr/bin/g++ -B/usr/bin`:

```text
ndnsf-distributed-inference
DI_NativeRequester
DI_NativeArtifactAuthority
di-native-assembly-worker
di-native-provider
```

The direct target build exited `0` after 6m55.728s. The receipt helper initially stopped
before the probe because the candidate Waf environment was omitted; after preserving that
failure, the retry supplied the candidate `WAFDIR`/`WAFLOCK` and put the candidate NAC-ABE,
NDN-SVS, and build directories first in `LD_LIBRARY_PATH`. Receipt generation and verification
then both exited `0`:

```text
SPEC180_NATIVE_IDENTITY_OK build-spec184-b5-candidate-r4/spec180-native-build.json
```

Current receipt SHA-256:

```text
219780a01753551d801ac6190ac334799ee948c5fbb7662e73041d40936f5e68
```

Current native hashes are:

| Artifact | SHA-256 |
| --- | --- |
| framework | `f107c897166f3d0375deb4e9779e1a5c88b63538ab3bbd75cad9630d55ff5bf5` |
| DI | `8c53e0fa457e2df5f8a4e4fb66151ff220ad136da78c6ddde09b39aa32291a3b` |
| requester | `c9e8dfa7d0eddd2c570dde4e4db23f4709a64e194135634b863bedaa36b1ae99` |
| artifact authority | `e8540479e6a23c97aacbfb40f10f57c8d4375a68ad140e7f8f328eb90405de33` |
| provider | `ba7545a623285ee89816e9213cfcb06c54acd0229bea009f1dbc3df4fe1e91f4` |
| assembly worker | `5675aba103f7df152742d56e3287f0e48e61ffe912f6c37e756f719d7bb86a27` |

The receipt records `binding_reused=false` and the candidate library paths. Compiler warnings
were limited to existing initializer-order, missing-field-initializer, and unused-helper
warnings; no new error or unresolved symbol remained in the same-tree build.

## Qualification disposition

The source and binary identity changed from the candidate used by YOLO runs `r51`, `r37`, and
`r50`. Those earlier results remain durable historical evidence, but their `PASS_FOR_ROW`
labels cannot be applied to this refreshed candidate until the root Y-A, Y-B, and Y-N runs are
repeated. The exact `Qwen/Qwen3.6-27B` row remains `WAITING_EXTERNAL_INPUT`; local
`Qwen3-0.6B` is smoke/ABI-only and is not a substitute. A4 inherited negative/retirement rows
remain `PARTIAL`, and T008 remains blocked by T007.

Raw build and receipt outputs are retained under `.codex-tmp/`:

```text
spec184-a4-target-build-20260912-r3.log
spec184-a4-native-receipt-build-20260912.log
spec184-a4-native-receipt-build-20260912-r2.log
spec184-a4-native-receipt-build-20260912-r3.log
spec184-a4-native-receipt-verify-20260912.log
```

## Binding refresh and A1 result (2026-09-12)

The first current-candidate Y-A launch exposed that the Python extension identity was stale after
the native source refresh. The binding was rebuilt from the same configured candidate with the
candidate NAC-ABE, NDN-SVS and DI libraries first in the loader path. Build log:
`.codex-tmp/spec184-python-binding-rebuild-20260912.log`, SHA-256
`bd74baa072ab553711bd2a5e03626ee18e704ee459594868ddc4e0f2675b5d02`. The refreshed receipt build
and verify logs are `.codex-tmp/spec184-native-receipt-after-binding-20260912.log` and
`.codex-tmp/spec184-native-receipt-verify-after-binding-20260912.log`; verify exited `0`.
The extension hash is `9e41958e73cb5b805e4d8261a901da9c43aea0ada85bfcf472808d7709ff2d2b`, and the
receipt hash is `2fbb8f40b2c51e1d3c0334387759112f90112264a2983157abe9421202b02b15`.

Root MiniNDN Y-A then passed against this refreshed identity. Output is
`.codex-tmp/spec184-yolo-Y-A-output-20260912-r58/`; launcher log is
`.codex-tmp/spec184-yolo-Y-A-run-20260912-r58.log`, SHA-256
`bc2320fea3a85682eea468cb2d59599b76bda6c57474a647fb1a7a9ba4b867cb`. The C++ numerical oracle
matched with shape `[1,50,6]`, terminal response and child cleanup completed. This refresh closes
T007-A1 only; Y-B/Y-N remain pending and A4 remains partial.
