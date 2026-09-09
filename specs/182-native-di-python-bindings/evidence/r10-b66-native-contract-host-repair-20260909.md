# R10-B66 Native Contract and Provider Host Repair

## Scope

本批次只修复 R10-B65 静态审查确认的三个本地边界：公开 C++ requester
构造路径与 JSON generation-mode 契约不一致、长期 fire-and-forget client 的
过期 operation 弱引用累积，以及 Provider 首次 `serve` 失败时半初始化 host
被发布。真实 Qwen 请求、跨进程 worker、YOLO caller migration、legacy
zero-use 和 T016 资格验收仍属于后续批次。

Baseline: `82589d54` (R10-B65 audit checkpoint). Working-tree changes were
reviewed as one complete diff; the two pre-existing design-document edits were
excluded from this batch.

## Five-lane review coverage

| Lane | Inspected boundary | Result |
| --- | --- | --- |
| production entry/callers | `NativeInferenceClient` direct contract constructor and request submission; `NativeInferenceProvider::serve` first-host path | 修复已覆盖公开入口和首次 host 生命周期；planner-first/default caller、跨进程接线仍开放 |
| implementation/wire | `isSupportedNativeGenerationMode`, envelope encoding/parser, weak operation table, host lease/runtime/core registration ordering | direct C++ and JSON paths now share the same two-mode predicate; host publication is delayed until registration succeeds |
| test/harness/oracle | `Spec182ClientState` and `Spec182ProviderHost` suites, including new unsupported-mode, compaction, and first-serve rollback cases | 全套 18 + 7 cases passed |
| build/source closure | Waf `unit-tests` target and changed C++ source inventory; `NativeRequestEnvelope.cpp` remains in the registered native source closure | `-j4` build and link passed in 59.180 s |
| migration/evidence | R10-B65 findings, current `tasks.md`, and remaining T013/T015/T016/T017 gates | 本地边界关闭供后续验证；不宣称生产请求或资格闭合 |

## Changes and review findings

1. `NativeRequestEnvelope` now owns one `isSupportedNativeGenerationMode`
   predicate. Both JSON parsing and envelope encoding use it, and the direct
   `NativeInferenceClient(const NativeRequestContract&)` constructor rejects
   unsupported modes with `INVALID_CLIENT_CONFIGURATION`.
2. `NativeInferenceClient` removes expired weak operation entries while holding
   its mutex before adding a new operation. Live handles remain observable;
   abandoned fire-and-forget entries no longer grow without bound.
3. `NativeInferenceProvider::serve` builds a new host, fixed lease, runtime,
   observer, guarded handler, Core registration, and target entry before
   publishing `m_host`. On first-serve failure it closes the fixed lease and
   leaves the provider retryable. Existing-host target failures retain the
   existing host as required by the shared-host contract.

The complete diff was checked for lock scope, ownership, exception rollback,
wire consistency, caller reachability, and source registration. No introduced
control-flow defect was found by the static review. The existing cppcheck
iterator diagnostic in `NativeEpochCoordinator.cpp:713` remains a classified
false positive; existing maintainability diagnostics in the touched files are
unchanged.

## Validation

Commands and results:

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH CXX=/usr/bin/g++ CC=/usr/bin/gcc \
  ./waf -o build-nac182 build --targets=unit-tests -j4
  PASS; Waf build/link elapsed 59.180 s

./.codex-tmp/spec182-r4-b2/build/unit-tests \
  --run_test='Spec182ClientState' --log_level=test_suite
  PASS; 18 cases

./.codex-tmp/spec182-r4-b2/build/unit-tests \
  --run_test='Spec182ProviderHost' --log_level=test_suite
  PASS; 7 cases

cppcheck --enable=warning,performance,portability --error-exitcode=1 \
  <changed native sources>
  PASS; no new error-level finding

git diff --check
  PASS
```

`vmstat 1 2` showed no swap-in/out in the second sample (`si=0`, `so=0`).
The host still has allocated swap from prior activity; this is not a memory
qualification result. No network, MiniNDN, SIF, cross-process, or no-Python
run was performed.

## Batch retrospective

- **Static review:** caught the direct-constructor bypass, unbounded weak-entry
  retention, and first-serve publication ordering before runtime validation;
  review covered production code, tests, ownership, locks, rollback, and Waf
  source closure.
- **Compile/build:** the single `unit-tests` build caught no additional source
  or link miss; source registration was checked explicitly.
- **Runtime/tests:** the changed client and Provider host suites passed. They
  prove local state and lifecycle boundaries only, not a real requester → Core
  → Provider worker or model execution.
- **Unobserved:** request identity mapping (F-05), real native-config Qwen or
  YOLO process flow (F-06), maintained caller migration, conversation owner,
  legacy retirement, I02–I08, and T016 remain open.

## Closure decision

`CLOSED_FOR_VALIDATION`: this bounded repair is complete and locally verified.
The parent Spec tasks remain `PARTIAL`; the next exit is a real native-config
Qwen requester → Core → Provider stream with explicit owner request identity,
followed by the corresponding worker, caller-migration, and qualification
gates.
