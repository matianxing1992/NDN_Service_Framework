# B189-0/B189-5 Convergence Evidence

**Status**: IN_PROGRESS / BLOCKED_FOR_NATIVE_EXECUTION
**Updated**: 2026-09-18 02:22 -0500

Spec189 documents and the active pointer have been created. The structural
checker passed and `verify-spec-kit-sync.py --require-entrypoints` passed
(`11/11` local entrypoints plus personal shared skill). The candidate tuple,
real Qwen artifacts, affected native build receipt and r01-r21 MiniNDN logs are
retained. The complete production caller/symbol map, C++ full-path oracle,
post-grant execution boundary and repeat convergence are not complete.

## Four miss classes

- static: the initial plan lacked mandatory policy/credential/module/disk/digest preflight and a stable post-grant marker exit; the V3 endpoint projection issue was fixed after r18, but its C++ regression is still missing;
- compile/link: the affected DI target closure was rebuilt with the recorded receipt, but no Spec189 full-path oracle target has run;
- runtime/test: real ACK/Selection and protected-grant verification are observed in r21; no fetch, assembly, runner, execution, terminal or cleanup result is observed;
- unobserved: the first post-grant boundary, native Repo commit ownership, placement-bound fetch/assembly, hidden-state handoff, terminal output, drain and repeat.

## Five-lane coverage

| Lane | State | Evidence / gap |
| --- | --- | --- |
| production entry/callers | `covered-partial` | real requester/provider path and Selection callers are observed; prepare/Repo and post-grant execution closure are incomplete |
| implementation/wire | `covered-partial` | candidate policy, credentials and V3 projection fixes are present; C++ oracle and resource guard are absent |
| test/harness/oracle | `gap` | no registered Spec189 C++ full-path selector or repeat checker has run |
| build/source closure | `covered-partial` | affected DI binaries have a receipt; Spec189 oracle source/link map is missing |
| migration/evidence | `covered-partial` | immutable candidate and r01-r21 records are retained; repeat and cleanup evidence are missing |

## Closure decision

`OPEN_FOR_NEXT_BATCH`: implement and review the candidate preflight, provider
post-grant marker sequence and C++ V3 endpoint-preservation regression; then
run a fresh candidate to classify the first post-grant boundary before
attempting full execution. No task is complete from the current evidence.

## T001 native example/source closure r2 — 2026-09-18 11:20 -0500

The registered Spec189 example targets were rebuilt from the global dependency
configuration in `build-spec189-b189-3-global-r3` with `-j4`:

| Target | Source registration | SHA-256 | Loader boundary |
| --- | --- | --- | --- |
| `DI_NativeArtifactAuthority` | `examples/DI_NativeArtifactAuthority.cpp` / `examples/wscript:274-279` | `230b17f47cfb6cc55d346d704af393837315876954678258a44a2ce43f2756df` | `RUNPATH=/usr/local/lib:$ORIGIN/..`; DI/Core/NDN-CXX resolved through the global install |
| `DI_NativeRequester` | `examples/DI_NativeRequester.cpp` / `examples/wscript:253-258` | `8422a72d22344ca4f8f5f66896546864e93771bc68e82d190c88146bd43a7a19` | same global host RUNPATH; `PreparedModel::request` is an unresolved production-library symbol |
| `di-native-provider` | registered provider example / `examples/wscript:531-536` | `0dfab1316ceda264ff889be21647e75b53ae09db259493d825862975c6d680b0` | global DI/Core/NDN-CXX, Boost 1.71 and ONNX Runtime closure; no checkout or temporary prefix |
| `spec189-two-provider-oracle` | `examples/Spec189TwoProviderOracle.cpp` / `examples/wscript:285-290` | `2607cdfb2050edbf5109e73c46ad53f304f78ea39ccc62d103eca1818ee2dec1` | standalone C++ oracle with the same global RUNPATH policy |

The production symbol map is present in
`.codex-tmp/spec189-t001-example-closure/symbol-and-identity.log` and points
to `Runtime.cpp` (`User::prepare`), `PreparedModel.cpp`
(`PreparedModel::request`), `NativeRequestEnvelope.cpp`
(`encodeNativeRequestEnvelope`), `NativeCanonicalArtifactPublisher.cpp`
(`prepare`, `publishUncached`, `bindPrepared`), and
`NativeProviderHandler.cpp` (provider execution/runner preparation). The
example closure build passed in
`.codex-tmp/spec189-t001-example-closure-build.log`. The exact hash-to-loader
association is recorded separately in
`.codex-tmp/spec189-t001-example-closure/loader-identity.log` (SHA-256
`9a536b77dc542e0a22ac4c2912d9979940d48fe788ba5e660e647260e47edc84`): each
of the four listed hashes was checked with `readelf` and `ldd`; NDN/NDNSF
libraries resolve from `/usr/local`, Boost 1.71 from `/lib/x86_64-linux-gnu`,
and ONNX Runtime from `/opt/onnxruntime`, with no `not found`, checkout or
temporary path.

This closes the registered source/build portion of T001 and confirms that the
native examples use the documented global dependency roots. It does **not**
close the candidate handoff identity, a real Qwen receipt through
`Runtime::prepare`, or the post-grant execution path; T001 remains `PARTIAL`
until those production-path facts are recorded.

The frozen documentation snapshot
`.codex-tmp/spec189-t001-convergence-review-r2/diff.patch` (SHA-256
`2a2b7019da75383e1290f5c4f21f7f9f372a6b4d0424fd310b5eb425d02f6109`) received
the official read-only review-agent result `STATIC_PASS`, with no P0/P1/P2
finding. The review did not build or run the selector.
