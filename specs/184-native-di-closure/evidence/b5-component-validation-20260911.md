# Spec184 B5 Component Validation Evidence

**Date**: 2026-09-11  
**Status**: `PARTIAL` / component gates pass; qualification candidate is not promoted  
**Scope**: T006 component selector binding, native Provider-host lifetime correction, and
candidate build provenance.

## Source and candidate boundary

The source baseline before this unit was `b826abdedfab7f81803f7ced904b4db645a2fa72`.
The only product source change in this unit is
`NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.cpp`:
`HostState -> ExecutionLeaseService -> resolver` now uses a `weak_ptr` to break the
self-cycle; the fixed lease router and collaboration handler retain the host while the
runtime handler may still use the host-owned lease table. Source diff SHA-256:
`bf6ff942d158cb5ff40c8c1d8b64155c219654d026e13547bef6c588992c93e5`.

The candidate normal tree was rebuilt with `WAFLOCK=.lock-spec184-b5`, system-first
`/usr/bin/g++ -B/usr/bin`, Boost 1.71, the pinned NAC-ABE/SVS prefixes, and the existing
ONNX prefix. The targeted rebuild completed 312/312 tasks in 22.116 seconds. Current
artifact hashes are:

```text
unit-tests                    7a8375e44999a4447f8352cb3c5092a21f7426d5bd472ff7adf0db122016b422
integration-tests             aad4846909b088f6285a3dcf6352482fec416ee89ce1822fbdf839783d4c8212
DI_NativeOnnxAssemblyWorker   336ce668a0924ba03aafe6f077bc1b54cb77e4f6d5c6bd4200fb6fbd58ce31b7
```

The independent ASan/UBSan tree was rebuilt for the same source change; its executable
hash is `2f0ecd190864050599214b43732d96455de79b5bda99d3ccf1ff9022193d206b`.

## Static review gate

The read-only review covered the full changed function path (`serve`,
`makeHostSlotResolver`, `makeLeaseRouter`, `ExecutionLeaseService::handle`, and
`ServiceProvider::ServiceRegistration::close`) plus the Provider-host test call sites.
The review checked ownership, lock order, delayed Face cleanup, expired-host behavior,
runtime handler raw table references, and selector/error mapping. The first attempted
all-weak design was rejected by the ASan UAF boundary and was not retained. The final
resolver-only weak capture has no finding in the changed path.

## Native C++ selectors

With the candidate directory first in `LD_LIBRARY_PATH`, the following normal unit suites
passed (all exit code 0):

```text
Spec182NativePlanning/*       30
Spec182PlanSealer/*           12
Spec182Preparation/*          18
Spec182NativeTokenizer/*       3
Spec182TokenizerFull/*         7
Spec182NativeAssembly/*        3
Spec182GrantAuthority/*        6
Spec182GrantIssuer/*           4
Spec182GrantClient/*           8
Spec182OfferAdmission/*        5
Spec182ProviderHost/*          8
Spec182Registration/*          6
Spec182SharedLease/*            3
Spec182V3Placement/*           10
Spec182NativeInferenceClient/*  2
Spec182Conversation/*           7
Spec182StreamAcceptance/*       7
Spec182Sampling/*               4
```

The Provider-host suite was also run under the rebuilt unsuppressed ASan/UBSan executable
and under the independent clang TSan executable; all 8 cases passed with no sanitizer
report and no LeakSanitizer residue. The raw TSan output is
`.codex-tmp/spec184-b5-provider-tsan-20260911.log`. The prior
unsuppressed run is retained at
`.codex-tmp/spec184-b5-candidate-asan-20260911/Spec182ProviderHost.log` and first
identified 11,042 bytes in 122 allocations retained by the resolver cycle. A diagnostic
all-weak retry is retained at
`.codex-tmp/spec184-b5-provider-fix-asan-20260911.log`; it exposed a UAF in
`NativeProviderHandlerState::~NativeProviderHandlerState` because the handler still
references the host table. The final resolver-only change is validated by
`.codex-tmp/spec184-b5-provider-fix2-asan-20260911.log`.

The focused normal integration selectors also passed:

```text
Spec182GrantClientFlow/Spec184AuthorityIoOwnership
Spec170NdnsfDiCoreFlow/Spec184DurableOutcome
Spec170NdnsfDiCoreFlow/Spec182R10B37RealProviderNativeStreamRequest
Spec170NdnsfDiCoreFlow/Spec182R10B73NativeConfigQwenRealProviderStream
Spec170NdnsfDiCoreFlow/Spec182R10B80NativeConfigQwenRealProviderConversation
Spec170NdnsfDiCoreFlow/Spec182R11B8G3RealProviderDynamicConversationPlacement
```

The durable outcome rerun emitted `SPEC182_NATIVE_DI_REQUEST_RESULT_OK`; its raw output is
`.codex-tmp/spec184-b5-provider-fix2-integration-20260911.log`.

## Bounded dynamic parameter matrix

| Class | Parameters / budget | Expected native assertion | Selector and result |
| --- | --- | --- | --- |
| Host normal | one/two targets, one compute slot, lease on/off, close and re-serve | sibling routing survives close; duplicate active serve is rejected | `Spec182ProviderHost/*`; normal and ASan/UBSan PASS |
| Host lifetime | provider destruction after close, delayed Core detach | no resolver self-cycle, no UAF, no leaked host table | `Spec182ProviderHost/*`; ASan/UBSan PASS and TSan PASS |
| Planning | rank 1, invalid rank, graph cut, budget truncation, deterministic tie | legal candidate or bounded rejection with stable order | `Spec182NativePlanning/*`; normal and ASan/UBSan PASS |
| Sealing | complete and foreign/incomplete artifact covers | canonical bytes and fail-closed rejection | `Spec182PlanSealer/*`; normal and ASan/UBSan PASS |
| Preparation/assembly | valid cold role set, duplicate cover, missing artifact | certified ownership, no interpreter, bounded failure | `Spec182Preparation/*`, `Spec182NativeAssembly/*`; normal and ASan/UBSan PASS |
| Tokenizer | ASCII, Unicode, byte fallback, unknown/out-of-range IDs | exact text parity and bounded malformed-input rejection | `Spec182NativeTokenizer/*`, `Spec182TokenizerFull/*`; normal and ASan/UBSan PASS |
| Caller routes | authority IO, durable outcome, native stream/Qwen/conversation | production marker or terminal status, no hidden fallback | six parent-qualified integration selectors above; PASS |

The matrix is bounded to the current component exits. Full process-tree, no-Python, real-model,
negative collector, and candidate identity rows remain handed to T006/T007; they are not silently
counted as covered by these unit selectors.

## Harness regression and setup boundaries

The C++-owned Python harness regression remains green: 71 tests passed in
`.codex-tmp/spec184-b5-python-harness-20260911/pytest.log`. Python is used only for
runner/observation assertions; no Python planner or runtime owner is promoted by this
record.

Two initial Boost selectors were submitted without their parent suite and exited 200
before test setup (`.codex-tmp/spec184-b5-candidate-integration-focused-20260911/`).
The corrected parent-qualified selectors above are the only integration results counted.
The earlier comma-separated unit filter had the same setup boundary and is retained at
`.codex-tmp/spec184-b5-components-20260911/unit-tests.log`.

## Full candidate sweeps

After the component gate, the same candidate unit executable was run without a selector:
`build-spec184-b5-candidate/unit-tests --log_level=test_suite` with the candidate-first native
library path. It exited `0` after 146.304 seconds and ended with `*** No errors detected`.
The raw log is `.codex-tmp/spec184-b5-full-unit-20260911.log` (SHA-256
`143ecc81f846b9ef888e37563560aecd2d67bfae88f5378c9e34ae6902d167e3`).

The same full integration executable was then run with the same library path and a 900-second
bound. It exited `1` with **48 Boost test failures**. The first observed failures are the known
legacy D2b/D2h121/D2h212 zero-response boundaries and the Spec175 tiny-ONNX stream collector
reporting `stream event gap exceeded retry budget`; the Spec184 authority selectors still pass in
the same executable. This is a current-candidate integration sweep, not a qualification result.
The raw log is `.codex-tmp/spec184-b5-full-integration-20260911.log` (SHA-256
`5244434ca84d77f2b6ae6909d237d96cf1f3a34b9282b14a090e85e173701793`).

The sweep therefore provides a clean full-unit exit but leaves the integration and process
qualification lanes `PARTIAL`; it does not justify marking T006 or T007 complete.

## T006/T007 boundary

These selectors close component and ownership evidence only. They do not freeze a
promotion candidate, prove the complete 80-row qualification matrix, or qualify
MiniNDN/no-Python process rows. The matrix therefore remains `PARTIAL`; T006 must still
close exact source/config/fixture identities and fresh convergence before T007 can run.
