# B1 Runtime Evidence

**Date**: 2026-09-12
**Batch**: B1 (`T001 → T002`)
**Status**: PASS for the implemented B1 scope; later preparation, request, conversation, Provider, qualification, Python, and document batches remain open.

## Scope and review trace

The batch base was `fc10eafd` (B0C checkpoint). The frozen task snapshots were:

| Gate | Snapshot | Result |
| --- | --- | --- |
| T001 static | `.codex-tmp/spec185-t001-review-v7`, working SHA `358d464a...` | `STATIC_PASS` |
| T002 static | `.codex-tmp/spec185-t002-review-v4`, working SHA `6b6fdd1a...` | `STATIC_PASS` |
| B1 composition | T002 v3 snapshot `.codex-tmp/spec185-t002-review-v3`, SHA `06aff768e4035a839cfa21efb72300e880b6e7a7034e0309b5210068107ae3b2` | `B1_COMPOSITION_PASS` |

The read-only reviewer used the installed official skill at `/home/tianxing/.codex/skills/review-agent/SKILL.md` (SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`). T002 v1/v2 findings were repaired and reviewed again: implicit close in `drainAsync`, false-mode phase changes, copyability, API manifest coverage, and the JSON iteration compile error. No controlled finding remained at the v4 gate. The frozen snapshots include the untracked Runtime and installed-consumer files and exclude unrelated worktree changes.

## Implemented boundary

`Runtime::open` now owns the Core `Face`, `io_context`, key/trust objects, and `OperationRuntime`; it freezes and validates the requester-v1 configuration, identities, limits, digests, model registry, and offer admission before returning. `Runtime::user` exposes the current default profile and rejects a closed runtime. `Runtime::close` is idempotent and non-blocking; `drain` is a finite cleanup barrier with retryable timeout and owner-thread deadlock mapping. `drainAsync` uses the Core non-closing quiescence notification, does not implicitly close an Open runtime, and preserves callback/State lifetime. Runtime copy/move is deleted and `Subscription` is exported through the public umbrella. Core `OperationRuntime` retains its existing closing drain API and adds the explicit non-closing composition path.

## Five-lane coverage

| Lane | B1 evidence |
| --- | --- |
| production entry/callers | `NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp/.cpp`: `Runtime::open`, `user`, `close`, `drain`, `drainAsync`; `api.hpp` umbrella; installed `runtime-api-consumer.cpp` calls `open → user → close/drain`. |
| implementation and wire | `RuntimeState` phase/owner ordering and `CoreRuntimeOwner` shutdown in `Runtime.cpp`; `OperationRuntime::drainAsync(..., closeRuntime)` and `isQuiescentLocked` in `ndn-service-framework/OperationRuntime.*`; no protocol wire change. |
| test/harness/oracle | C++ `tests/unit-tests/di-runtime.t.cpp` (configuration, close/drain, callback-close, non-closing async); C++ `tests/unit-tests/core-operation-runtime.t.cpp` (non-closing quiescence); `tests/installed-api/runtime-api-consumer.cpp` (installed open/user). All are registered in `tests/wscript`. |
| build/source closure | Root `wscript` DI library source/export closure and `tests/wscript` Runtime/Core selectors; normal and TSan binaries were built from `build-spec185-b0c-normal` and `build-spec185-b0c-tsan-clang` with `-j4`; installed consumer linked the staged DI library and public headers. |
| migration/evidence | `contracts/api-exposure.json` adds `Subscription` and the transitive Core header; `contracts/core-app-boundary.md` records Core ownership and the non-closing internal entry; this record and `tasks.md` are the sole B1 progress authority. No Python, SIF, or Tiger qualification was counted. |

## Build and runtime results

The first normal build attempt entered an unintended full-tree Waf graph and failed at the B1 JSON iterator (`.items().first`); the exact failure was preserved in the session/raw build log and repaired before the v4 static gate. A subsequent normal DI/Core build with `WAFLOCK=.lock-spec185-b0c-normal`, system-first PATH, and `-j4` succeeded in 92.86 s. The first install attempt rebuilt unrelated targets and failed at the pre-existing `spec181-assembly-parity` link boundary; the second target-limited install reached the library install but was interrupted by the unrelated Python editable-install hook. Both boundaries are retained in `.codex-tmp/spec185-b1-runtime-consumer/install*.log` and are not B1 product failures.

Focused C++ selectors then passed:

```text
build-spec185-b0c-normal/spec185-runtime --run_test=Spec185Runtime: 7/7
build-spec185-b0c-normal/spec185-core-operation: 34/34
build-spec185-b0c-tsan-clang/spec185-runtime --run_test=Spec185Runtime: 7/7, twice
build-spec185-b0c-tsan-clang/spec185-core-operation: 34/34, twice (B0C raw logs)
```

Raw selector logs are in `.codex-tmp/spec185-b1-runtime-20260912/`; the earlier B0C TSan Core logs are in `.codex-tmp/spec185-b0c-runtime-20260912/`. No TSan diagnostic was reported. An isolated installed-prefix C++ consumer compiled all 68 manifest headers, rejected the excluded NativeRequestEnvelope header, linked the staged `libndnsf-distributed-inference.so`, verified `readelf`/`ldd` identity, and exercised `Runtime::open → user`:

```text
Spec185RuntimeConsumer PASS prefix=/tmp/spec185-b1-runtime-prefix
```

The consumer run used a command-local `/usr/bin/g++ -B/usr/bin` wrapper with `--copy-dt-needed-entries` because the manually staged prefix was produced after Waf's unrelated Python install hook; this is recorded as an install harness qualification detail, not a claim about the full-tree install. The checked-in installed-consumer script itself was not changed to hide that boundary.

## Closure and miss retrospective

`T001` and `T002` are complete for B1 and may be marked `PASS`. `static` coverage found the lifecycle and ownership defects before batch construction; `compile-link` caught the JSON iterator issue and the attempted full-tree/install boundaries; `runtime-test` covered normal and repeated TSan lifecycle selectors plus the installed C++ consumer. `unobserved` remains: real preparation/fetch, request/ACK/selection/execution, conversation persistence/recovery, Provider assembly, owner-thread public prepare/request paths (not yet exposed), full Waf installation including Python packaging, and process qualification. Those are explicit B2E–B9 exits and must not be inferred from B1.

**Closure decision**: B1 is closed for validation and the next dependency-satisfied unit is T016/B2E. No Python binding or external MiniNDN/SIF result is included.
