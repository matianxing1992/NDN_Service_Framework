# R6-B9 Legacy D2b Freshness Repair

**Date**: 2026-09-09
**Scope**: current-source compatibility repair for the legacy Spec170 D2b `DATA_V1` ingress
**Status**: `PARTIAL`; this record closes the local bounded behavior only and does not close T013-B or T016

## Scope and allocation basis

本批只处理 `ServiceProvider::isFresh` 在同一 producer session 内多个合法 Selection
publication 乱序到达时的接收边界。生产入口是旧 Spec170
`runProductionD2bDataV1Case`，独立出口是 `ProductionIngressRunsD2bSelectionIntoSvsDataV1`
及其同组的 tamper/drop/duplicate/reorder selectors；源码 closure 是当前
`ServiceProvider.cpp/.hpp` 与 `integration-tests` target。旧 session 拒绝、同名旧序列
重放拒绝和 `svs_mutex` 所有权仍由同一 gate 验收。跨进程、真实 namespace、旧调用方零使用
和最终资格不是本批验收依赖，继续由 T013/T016 保持开放。

## First failure boundary

R6-B7 的 trace 已确认 User 发布两条合法 Selection（provider1 sequence 3、provider0
sequence 4），但 provider1 在 `handleServiceSelectionMessage` 前没有 callback。原实现用
producer/session 的单一最高 sequence frontier，因此先看到 sequence 4 后把尚未见过的
另一 publication 的 sequence 3 当作旧数据丢弃。该边界发生在 Selection publication 完成
之后、Provider callback 之前；不是 ACK、解密、执行或 DATA_V1 fetch 边界。

## Implementation result

`ServiceProvider::isFresh` 现在在 `svs_mutex` 下维护 producer session 和每个 publication
name 的最高已接受 sequence：

- 更旧 producer session 继续拒绝；更高 session 清空该 producer 的 publication frontiers；
- 同一 session 对不同 publication name 允许合法乱序；
- 同一 publication name 的相同或更低 sequence 继续拒绝；
- session/session-map 与 per-name map 在同一锁下更新，避免 callback 线程间竞态。

没有修改 User publication、Selection wire、解密、执行或 T016 设施路径。

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` | `tests/integration-tests/ndnsf-di-core-flow.t.cpp:runProductionD2bDataV1Case`, `ServiceUser::PublishServiceSelectionMessageV2`, `ServiceProvider::onServiceSelectionMessage`/`handleServiceSelectionMessage` | `codegraph explore "ServiceProvider::isFresh ... ProductionIngressRunsD2bSelectionIntoSvsDataV1"`; `rg -n "ProductionIngressRunsD2b|handleServiceSelectionMessage"` | R6-B7 receipt/newness boundary reproduced; repaired path reaches both Provider callbacks |
| `implementation and wire` | `covered` | `ServiceProvider::isFresh`, `ServiceProvider.hpp` freshness maps, `svs_mutex`; Selection subscription registration | `sed` source trace; `rg -n "m_sessionIDMap|m_publicationSeqMap|svs_mutex"`; review of `onServiceSelectionMessage` and pre-existing `OnRequest`/collaboration registrations | No new actionable finding after checking session reset, duplicate fencing, lock scope and unchanged wire/decrypt path |
| `test/harness/oracle` | `covered` | D2b positive plus tamper/drop/duplicate/reorder selectors; independent provider callback and DATA_V1 assertions | `rg -n "BOOST_AUTO_TEST_CASE\(ProductionIngress.*D2b"`; six named selectors run individually against current binary | All five D2b selectors and D2h212 production selector passed; broad unfiltered suite exposed a separate later D2h crash boundary |
| `build/source closure` | `covered` | Waf `integration-tests` target, `tests/wscript` source list, current `ServiceProvider.cpp` | `rg -n "integration-tests|ServiceProvider.cpp|ndnsf-di-core-flow.t.cpp" tests/wscript`; system-first Waf build | `integration-tests` rebuilt from current source; 118/118 compile/link steps completed |
| `migration/evidence` | `partial` | R6-B7 diagnostic, this record, T013-B compatibility manifest and T016 preflight | `git diff --check`; preserved `.codex-tmp/spec182-r6-b9/` logs; task/evidence cross-links | Local legacy boundary closed for validation; cross-process zero-use and qualification remain open |

## Review trace

- Skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`
- SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`
- Baseline: `82f8e006`
- Diff scope: `ServiceProvider.cpp`, `ServiceProvider.hpp`, this evidence record, `tasks.md`, and the synchronized Spec Kit review references
- Review methods: call-path tracing, state/decision table for session/name/sequence ordering, ownership/concurrency review of `svs_mutex`, contract/wire comparison, Waf source closure and selector registration checks
- Finding: the prior static gate did not model independent publication names sharing one producer/session. The runtime trace supplied the missing counterexample. The bounded per-name repair was re-reviewed; no further actionable finding was found in this diff.

## Validation

```text
./waf build --targets=integration-tests -j4
# exit 0; Waf 1m37.712s, real 1m37.819s; 118/118; log: .codex-tmp/spec182-r6-b9/build-repair.log
# vmstat samples after the first line showed no sustained swap-out; the host already had swap pages resident.

./.codex-tmp/spec182-r4-b2/build/integration-tests \
  --run_test='Spec170NdnsfDiCoreFlow/ProductionIngressRunsD2bSelectionIntoSvsDataV1' \
  --log_level=message
# exit 0; .codex-tmp/spec182-r6-b9/individual-0.log
```

The following named selectors also exited 0 individually: `ProductionIngressRejectsTamperedD2bSvsDataV1`,
`ProductionIngressBoundsDroppedD2bSvsDataV1`, `ProductionIngressDeduplicatesD2bSvsDataV1`,
`ProductionIngressReordersD2bSvsDataV1`, and
`ProductionNativeHandlersRunD2h212ToCompleteOracleResponse`. Their logs are
`.codex-tmp/spec182-r6-b9/individual-1.log` through `individual-5.log`.

The first unfiltered `Spec170NdnsfDiCoreFlow/*` attempt reported missing response, one missing
role and then `double free or corruption` in a later
`ProductionNativeHandlersRunD2h212ToCompleteOracleResponse` instance; it was interrupted after
that boundary. Two subsequent fresh unfiltered runs exited 0, and the individual D2h selector also
exited 0. The intermittent first attempt is retained as a runtime/test observation outside this
bounded D2b repair; it is not treated as a deterministic regression without a durable reproducer.

## Batch Retrospective

- `static`: the previous static review covered the global freshness rule but missed that independent
  publication names can legitimately share a producer/session and arrive out of order. The new review
  rule now requires this interleaving table explicitly.
- `compile/link`: no compiler or linker defect was found. Adding the header map caused the expected
  transitive rebuild; the Waf target registered the current source correctly.
- `runtime/test`: only the named D2b runtime selector exposed the lost provider1 callback. After the
  repair, all five D2b selectors and the named D2h212 selector passed individually; two subsequent
  unfiltered Spec170 suite runs also passed. The first unfiltered attempt exposed an intermittent
  D2h callback/lifetime crash, retained as an open reproducibility boundary rather than attributed to
  this D2b repair.
- `unobserved`: cross-process SVS delivery, long-lived publication-map growth, legacy caller zero-use,
  and T016 namespace/NFD execution were not observed here.

本批在取得稳定 D2b 出口后停止扩张，没有继续加入新的生产职责。构建耗时只适用于本次
`integration-tests`、当前 source closure、system-first toolchain 和 `-j4` 工作树条件，不能
推导总体提速。

## Result and closure

`STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS` for the six named selectors;
`PARTIAL`; not `QUALIFICATION_PASS`.

Closure decision: `CLOSED_FOR_VALIDATION` for the local legacy D2b freshness boundary.
`OPEN_FOR_NEXT_BATCH` for T013-B/T013-C and T016 until maintained callers, cross-process behavior,
and the external MiniNDN/NFD qualification context are available. The broad-suite D2h crash remains
an intermittent independent runtime boundary and must not be erased by this local D2b result.
