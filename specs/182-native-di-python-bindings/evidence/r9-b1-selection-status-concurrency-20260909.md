# R9-B1 Selection Status Concurrency Ownership

日期：2026-09-09。此批次修复 D2h212 本地生产 selector 在并发 Provider worker 更新
selection operation status 时的内存所有权边界；不宣称跨进程或 T016 qualification。

## Scope and first failure boundary

R6-B9 后续重复仍偶发 `double free or corruption`。用 `LD_PRELOAD` 的 ASAN allocator
在第一次复现中给出确定栈：一个 worker 在
`ServiceProvider::reportSelectionOperationStatus` 扩展 `parent.memberStatuses` 时释放
旧 vector 存储，另一个 worker 同时写入该旧元素（heap-use-after-free）。同一状态 map
也可被 Face 查询线程读取，原实现没有统一的并发保护。该边界位于 Provider selection
status bookkeeping，不是 freshness、DATA_V1 解密或模型执行协议。

## Implementation result

`ServiceProvider` 增加专用 `m_selectionExecutionStatusMutex`，并在
`reportSelectionOperationStatus`、`updateSelectionExecutionStatus` 和
`getSelectionExecutionStatus` 中保护 map、`memberStatuses` vector 以及返回快照。没有
改变 status wire、状态转换、Selection digest 绑定或 Core callback 契约。

## Minimum Review Record

| Lane | Files / symbols | Query or check | Result |
| --- | --- | --- | --- |
| `production entry/callers` | `ServiceProvider::CollaborationContext::reportOperationStatus`, `NativeProviderHandler` worker status reports, `replySelectionExecutionStatus` | `rg -n "reportOperationStatus|reportSelectionOperationStatus|replySelectionExecutionStatus"` | worker writes and Face reads share the new status owner |
| `implementation and wire` | `ServiceProvider.cpp/.hpp` status map, `m_selectionExecutionStatusMutex` | `codegraph explore "ServiceProvider reportSelectionOperationStatus getSelectionExecutionStatus"`; diff review | lock covers map/vector lifetime; status payload and binding rules unchanged |
| `test/harness/oracle` | `SelectionSnapshotConcurrentMembersRemainOwned`; D2h212 production selector | unit selector plus named integration selector; `rg -n "SelectionSnapshotConcurrentMembersRemainOwned|ProductionNativeHandlersRunD2h212"` | concurrent 8×8 member insertion and complete D2h oracle exercised |
| `build/source closure` | `tests/wscript` `unit-tests`/`integration-tests`, `ServiceProvider.cpp` | `./waf build --targets=unit-tests -j4`; `./waf build --targets=integration-tests -j4` | current source compiled and linked in both targets |
| `migration/evidence` | this record, R6-B9 evidence, `tasks.md`, `docs/failure-log.md` | `git diff --check`; preserved ASAN/raw repeat directories | local status UAF closed for validation; T013/T016 and cross-process gaps remain |

## Batch Retrospective

- `static`: the prior review checked status binding and stale sequence rules but did not model
  concurrent vector growth; this batch adds the required worker/Face ownership lane.
- `compile/link`: the dedicated mutex is header-visible and caused the expected transitive
  rebuild; both Waf targets linked from the current source closure.
- `runtime/test`: the new unit selector passed; D2h212 passed `50/50` fresh processes after
  the fix. ASAN allocator preloading with type-size mismatch checks disabled passed `20/20`
  without a sanitizer memory error. The earlier `2/20` historical failures remain preserved.
- `unobserved`: long-running map retention, cross-process status publication, real NFD namespace
  execution and T016 qualification were not observed.

## Validation and closure

`unit-tests` build: `188/188`, Waf `3m3.636s`; integration build: `118/118`, Waf `1m44.786s`;
vmstat samples showed no sustained swap-out. Focused unit selector exited 0. D2h212 production
selector exited 0 in `50/50` fresh processes; raw logs are under
`.codex-tmp/spec182-r9-b1/d2h212-postfix-repeat50/`. ASAN-preload follow-up passed `20/20`
with `new_delete_type_mismatch=0` and `alloc_dealloc_mismatch=0`; raw logs are under
`.codex-tmp/spec182-r9-b1/d2h212-asan-postfix/`.

`STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for the local
selection-status ownership boundary. T010/T013/T016 remain `PARTIAL`; no qualification claim.
