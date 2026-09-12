# B0C Core Operation and DI Delegation Evidence

**Date**: 2026-09-12
**Status**: `CLOSED_FOR_VALIDATION`
**Tasks**: T017, T018
**Contracts**: C-09 `CB01`–`CB04`, `PO-C1`–`PO-C4`

## Scope and closure

B0C extracts the generic operation runtime/state and makes
`NativeInferenceClient` consume it while retaining DI model, attempt,
conversation, authorization, and durable-commit semantics. The Core target has
no DI, Python, or ONNX source dependency. The DI target uses the complete
recursive DI/adapters source closure and exercises real Provider ACK, durable
cancellation, and conversation-commit failure paths.

T017 and T018 are closed only for the validation scope below. This record does
not claim full Spec185 completion, Python binding acceptance, process
qualification, SIF/Tiger execution, or the later prepared-model batches.

## Five-lane review matrix

### production entry/callers

- `ndn-service-framework/OperationRuntime.hpp/.cpp` and
  `OperationState.hpp` provide the generic ticket, worker, close/drain,
  completion, observer, and bounded-reader primitives.
- `NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.cpp`
  creates the Core state/runtime, delegates dispatch/status/result/observe,
  and keeps DI terminal claims separate from Core completion.
- `tests/installed-api/core-operation-consumer.cpp` is an external-style
  Core-only consumer; `tests/integration-tests/di-core-operation.t.cpp` and
  the exported real-provider wrappers are the production-path callers.

### implementation and wire

- The Core state machine covers pending, streaming, completed, failed,
  cancelled, close, drain, reader completion, and observer retirement.
- `publishIf` performs the DI terminal-admission check while holding the Core
  state lock, then publishes the event; the DI mutex is acquired only by the
  admission callback. This closes the Core-to-DI lock-order boundary and
  prevents a late event from being accepted after a DI terminal claim.
- `markTerminal` claims DI terminal state under the DI mutex and completes or
  fails Core state after releasing it. Result-copy exceptions are converted to
  the terminal error path.
- No `SerialRequestExecutor` or `NativeOperationExecutor` remains in
  `NativeInferenceClient`; no stream-final event is treated as durable
  conversation completion.

### test/harness/oracle

- `Spec185CoreOperation`: 33 C++ unit/lifecycle/concurrency cases, including
  external terminal admission, close/drain, reader gaps, cancellation, timer
  retirement, observer behavior, and bounded history.
- `Spec185DiCoreOperation`: 5 C++ integration cases covering fail-closed
  production behavior, terminal stability after cancel, real Provider unary
  ACK/commit/response, durable cancellation, and conversation commit failure.
- `Spec170NdnsfDiCoreFlow`: 59 existing C++ Core flow/cooperation/registration
  regression cases.
- The TSan Core selector was run twice from the dedicated Clang tree; both
  runs passed all 33 cases.
- `tests/installed-api/run-spec185-core-operation-consumer.sh` compiles,
  links, loads, and runs the Core-only consumer from a staged install prefix.

### build/source closure

- Normal build: `build-spec185-b0c-normal`, system-first `/usr/bin` toolchain,
  explicit system Boost, NAC-ABE prefix, ONNX install, and NDN-SVS source/build
  pair. Command:

  ```text
  WAFLOCK=.lock-spec185-b0c-normal PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH ./waf build --targets=spec185-core-operation,spec185-di-core-operation -j4 -v
  ```

  The full DI/adapters recursive source closure linked successfully. The
  final Core test-only rebuild after the test-harness fix completed in 7.485 s.
- TSan build: `build-spec185-b0c-tsan-clang`, Clang 10,
  `-fsanitize=thread`, dedicated WAF lock `.lock-spec185-b0c-tsan`.
- Installed consumer uses the built Core library plus the exact public Core
  headers and generated `config.hpp` staged under `/tmp/spec185-b0c-install-v28`;
  its package has no source-tree include or library leak. This is a bounded
  Core-only installed-API check, not a claim that the interrupted whole-tree
  Waf install completed.

### migration/evidence

- The old DI executor and duplicate generic wait state were removed from the
  requester; the public handle signatures remain compatible for this scope.
- The single result file is this document. Task state is synchronized in
  `tasks.md`; later B1–B9 work remains `NOT_STARTED`.
- No SIF/Tiger or Python qualification was started in B0C.

## Static review trace

Official skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`
Skill SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`

Task snapshots were immutable and included newly untracked files. T017 passed
v19 and v24. v25 and v26 were correctly rejected: v25 had a timeout applied to
the wrong test; v26 found terminal-admission/lock-order and timer issues. v27
passed after the terminal admission fix and timer correction. v28 passed after
the Core-to-DI lock inversion fix. v29 passed after changing asynchronous test
callbacks to promise/atomic handoff so Boost.Test assertions did not run on
worker threads.

- Final T017/T018 snapshot: `.codex-tmp/spec185-t017-t018-review-v29`
- Final base: `9524177a43c68badaace9352970dfb7d82bb8f79`
- Final diff SHA-256:
  `87286117cf9ec8efb76f7b44b118751d373096337c219ea045998ad0bdf53ced`
- Batch composition snapshot: `.codex-tmp/spec185-b0c-combo-review-v1`
- Batch composition diff SHA-256:
  `f1c457c696a0e3860848ce70c6b559f9341920a9c402f54e8a1c64dddc69a232`
- Final v29 result: `STATIC_PASS`, no control defects or unresolved five-lane
  gaps. Batch composition review v1: `PASS`.

## Compile/link and runtime results

| Candidate / selector | Result | Durable log |
| --- | --- | --- |
| Normal `spec185-core-operation` | 33 cases, `*** No errors detected`, exit 0 | `.codex-tmp/spec185-b0c-runtime-20260912/core-selector-v29.log` |
| Normal `spec185-di-core-operation` | 5 cases, exit 0; real Provider/ACK paths exercised | `.codex-tmp/spec185-b0c-runtime-20260912/di-selector-v29.log` |
| Normal `Spec170NdnsfDiCoreFlow` | 59 cases, `*** No errors detected`, exit 0 | `.codex-tmp/spec185-b0c-runtime-20260912/core-regressions-v28.log` |
| TSan Core selector, repetition 1 | 33 cases, exit 0 | `.codex-tmp/spec185-b0c-runtime-20260912/tsan-core-selector-repeat1-v29.log` |
| TSan Core selector, repetition 2 | 33 cases, exit 0 | `.codex-tmp/spec185-b0c-runtime-20260912/tsan-core-selector-repeat2-v29.log` |
| staged Core-only installed consumer | compile/link/load/runtime `PASS` | `.codex-tmp/spec185-b0c-runtime-20260912/installed-consumer-v29.log` |

Final SHA-256 identities:

```text
build-spec185-b0c-normal/spec185-core-operation 014de9e36f4461cb8f10fe6a2f78281fa7bc6a124c2f8fa7e777b328a19e8799
build-spec185-b0c-normal/spec185-di-core-operation e51a90c5fc12d8d00dd03a97403d4627a44c4056c267944be5472c075615b24a
build-spec185-b0c-normal/libndn-service-framework.so f6866ed06add18842d6a5b1bacb676e0f09fac7aff4c194766f040d89fa904a2
build-spec185-b0c-tsan-clang/spec185-core-operation 7140807eef433e3cea2290ede312fb042d510876ee8e5bda2af54d651185d9ce
build-spec185-b0c-tsan-clang/libndn-service-framework.so 0fe961019ec730ccbda2dc25c68631a4bc0a4e722ebcd6666e0bc744228ae7fa
```

## Miss retrospective and boundaries

- `static`: v25/v26 caught real timer, terminal-admission, and lock-order
  defects before batch execution; v29 found a test-thread assertion race.
- `compile-link`: normal and TSan builds caught no remaining B0C compile or
  link defect after the reviewed fixes; the target used the complete DI source
  closure.
- `runtime-test`: normal selectors, 59 regressions, TSan repetition, and the
  installed consumer all passed. The first TSan run exited 66 at the
  `PendingReaderCompletionRetiresItsTimerBeforeDrain` test because asynchronous
  Boost.Test assertions raced with the test thread; this was a harness boundary,
  repaired with promise/atomic handoff, then rerun twice.
- `unobserved`: full-tree Waf installation was intentionally interrupted in
  post-install pip work after the bounded library install; full Python binding,
  Runtime/preparation/request/conversation/provider qualification, and
  process/SIF/Tiger behavior are outside B0C and remain for later batches.

## Closure decision

`PO-C1`–`PO-C4` are covered by the C++ selectors, TSan repetition, real DI
fixtures, and the bounded installed Core consumer. B0C is therefore closed for
validation, and the next dependency-satisfied unit is T001 in B1 (after the
T017/T018 checkpoint).
