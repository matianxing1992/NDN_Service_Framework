# Tasks: Placement Observation Collection and Planning

**Input**: Design documents from `specs/194-placement-observation-planning/`
**Prerequisites**: [spec.md](spec.md), [plan.md](plan.md), [research.md](research.md), [data-model.md](data-model.md), [contracts/placement-observation-v1.md](contracts/placement-observation-v1.md)
**Execution mode**: STRICT_SERIAL

## Prior-work disposition

旧 Experimental 工作集（包括 Spec193）均为 `STOPPED / NOT COMPLETE`，不恢复任务；详细边界见 [current source audit](evidence/current-source-audit.md#reset-and-scope)。

## Execution Progress

| Task | Status | Depends on | Evidence | Updated | Remaining acceptance |
|---|---|---|---|---|---|
| [T001](#phase-1-core-telemetry) | NOT_STARTED | — | None | 2026-09-28 23:48 -05:00 | 在同一代码任务中核对 Core 真实 producer/consumer 与字段来源，实现 typed directed samples/access，增加 C++ behavior assertions；selectors 由 T005 统一运行。 |
| [T002](#phase-2-signed-provider-observation-offer) | NOT_STARTED | T001 | None | 2026-09-28 23:48 -05:00 | 同一任务内关闭并实现 authenticated V4 capability、canonical/signature/key/epoch 契约与 offer/admission C++ regressions；保持 V3 bytes 不变。 |
| [T003](#phase-3-immutable-request-snapshot) | NOT_STARTED | T002 | None | 2026-09-28 23:48 -05:00 | 实现 whole-roster profile、不可变 snapshot/sealer binding，并用 C++ oracle 检查 stale/tamper/replay。 |
| [T004](#phase-4-observation-aware-placement) | NOT_STARTED | T003 | None | 2026-09-28 23:48 -05:00 | 实现有界确定性的 role-DAG policy，保留 hard/preference/fallback 语义，并运行独立 C++ cost/determinism oracles。 |
| [T005](#phase-5-native-batch-and-full-path-convergence) | NOT_STARTED | T004 | None | 2026-09-28 23:48 -05:00 | 单次组合审查与共享 native build/selectors；记录五 lanes、review trace、稳定出口和四类漏检。 |
| [T006](#phase-6-real-minindn-qualification) | NOT_STARTED | T005 | None | 2026-09-28 23:48 -05:00 | 已安装系统上的真实 MiniNDN 双 Provider A/B 完整生命周期、SC-004 latency 与 cleanup/identity gates 全部通过。 |

## Phase 1: Core Telemetry

**Purpose**: Expose accurate, bounded, typed directed samples from real events while retaining Core ownership and avoiding synthetic RTTs.

- [ ] T001 [US1] 核对 `NetworkTelemetryStore`、`ServiceUser` 和 `ServiceProvider` 的真实生产调用方、数据来源、TTL 与构建/test registration；实现 User ACK/data-transfer 和 Provider dependency-fetch 的 typed snapshot/access。区分 ACK RTT 与 data-fetch first-byte/elapsed/goodput；无来源、未知或过期数据保持 unavailable，不导出 Data names/key scopes。增加具名 production C++ behavior assertions。若该任务改变 API、可观察行为或安全契约，只在本任务同步对应契约/设计文档。

**Design binding**: `CD=NetworkTelemetrySample/Snapshot/Store + User/Provider access`; `FN=按源码核对并在本任务冻结、实现 typed sample update/query signatures`; `FIELD=Core owns sample windows, kind, counters, TTL and endpoint keys`; `FLOW=real ACK/fetch completion -> bounded snapshot -> DI consumer`; `PO=Core production C++ selectors for success, failure, stale and direction-swapped samples`; `Design status=DRAFT until the in-task source review`; `Risk class=async transport state`; dynamic profile frozen by T005.

## Phase 2: Signed Provider Observation Offer

**Purpose**: Advertise current, request-scoped Provider capacity/residency and P→P rows before Selection without fetching model material.

- [ ] T002 [US1] [US2] 从真实 discovery/ACK 路径定位并实现 authenticated `DI_PLACEMENT_V4` capability 和独立版本的 production offer/decoder；在同一任务内冻结 canonical field order、signature/key policy、source/target epoch 和资源字段。绑定 request/attempt/model/graph/provider boot/expiry；区分 observed/known-empty/unknown，只发布当前 candidate digests，拒绝 malformed、duplicate、wrong-epoch、stale、non-finite 数据；逐字节保持 V3 offer 兼容。增加 C++ capability/offer/admission regressions，并将必要安全/行为契约更新并入本任务。

**Design binding**: `CD=placement-observation-v1.md V4 envelope`; `FN=capability advertisement and V4 issuer/decoder/admission signatures closed and implemented here`; `FIELD=Provider owns capability, offer/resource/cache rows; Core owns generic sample facts; key policy owns signer trust`; `FLOW=authenticated capability discovery + ACK decision + current snapshots -> canonical signed V4 -> Core ACK authentication -> DI admission`; `PO=named production C++ capability/V4 signer/decoder/admission selectors`; `Design status=DRAFT until the in-task source/security review`; `Risk class=canonical parser/crypto and cache side channel`; dynamic profile frozen by T005.

## Phase 3: Immutable Request Snapshot

**Purpose**: Join verified Provider facts with requester-local observations and the current graph into one freshness-bound strategy input.

- [ ] T003 [US1] [US2] 在固定完整 eligible candidate roster 后选择单一 request-level profile：仅当所有候选均 authenticated V4 时使用 V4，否则整次 request/attempt 保持现有 V3/default；拒绝混合 offer。实现 native snapshot builder，合并 User→Provider samples、Provider edges/resources/residency 和精确 prepared role graph。拒绝 replay、stale/unknown required path、endpoint boot/topology 变化、digest mismatch 或 graph/offer mismatch；将 snapshot digest 绑定 proposal/sealed plan。增加独立 C++ negative oracles；仅当该行为改变 API/可观察行为/安全契约时在本任务更新相应文档。

**Design binding**: `CD=request-level profile decision + PlacementObservationSnapshot + snapshot/selection binding`; `FN=whole-roster profile selector, builder, validator and proposal binding closed and implemented here`; `FIELD=User planner owns immutable snapshot/profile decision; Core ACK and DI offer validators own independent authentication`; `FLOW=freeze eligible roster -> select one profile -> ACK closure -> authenticate offers -> snapshot -> propose -> seal`; `PO=named production C++ profile/snapshot/sealer selectors`; `Design status=DRAFT until the in-task source review`; `Risk class=replay/TOCTOU and async cancellation`; dynamic profile frozen by T005.

## Phase 4: Observation-Aware Placement

**Purpose**: Rank complete feasible assignments with real directed costs while preserving existing admission, caller preference, spread and deterministic fallback behavior.

- [ ] T004 [US3] 实现独立 V4 strategy。在 stable topological order 上评估完整可行 role/provider/device assignment，保留 hard checks 与既有 preferred/distinct group；只计入可证实的 queue/preparation 和有已知字节数的 directed `DATA_FETCH` transfer。将 `ACK_RTT` 仅用于 control path，不作为 bulk throughput；未知 tensor bytes 不按零处理。于本任务冻结并实现 deterministic beam width、`maxAssignmentStates` 与 request deadline/budget 行为；超预算回退现有 baseline，不返回 partial assignment。保留 V3 strategy/bytes；绑定 policy version、snapshot digest、cost breakdown 和 fallback reason。增加独立 C++ hand-calculated/determinism oracles；若策略改变 API/可观察行为/安全契约，在本任务同步相应文档。

**Design binding**: `CD=PlacementCostBreakdown + V4 strategy order/bounds`; `FN=proposeRolesV4 signature and cost comparator closed and implemented here`; `FIELD=DI owns graph/bytes/cache/strategy; Core does not score model roles`; `FLOW=validated V4 snapshot -> hard feasibility -> bounded deterministic cost ranking -> validated proposal/seal`; `PO=named C++ production strategy and proposal validator`; `Design status=DRAFT until the in-task source/algorithm review`; `Risk class=algorithm bounds and cancellation`; dynamic profile frozen by T005.

## Phase 5: Native Batch and Full Path Convergence

**Purpose**: Close the native batch once, including the full-path static convergence review, before system/network qualification.

- [ ] T005 在 T001–T004 分别通过 read-only task review 后，对 `source→identity/catalog→prepare→material/Repo→ACK/Selection→placement fetch→assembly→runner/execute→terminal→cleanup` 做一次组合审查。核实实际 target/source closure、registered C++ selectors 和 `Dynamic gate card`；记录五 lanes、severity/owner findings、review-agent baseline/path/SHA、stable observable exit 与 `static`/`compile-link`/`runtime-test`/`unobserved` retrospective 到唯一的 `evidence/native-batch-closure.md`。只有审查无阻塞时才构建共享 `integration-tests` target 并运行 T001–T004 的 C++ selectors 一次。T005 为 verification-only；发现问题时先登记依赖明确的修复工作单元并完成受影响审查，再恢复共享验证。Python 可编排 target，但不能断言 native behavior。

**Design binding for T005**: `CD=all accepted Spec194 contracts and batch exit`; `FN=registered C++ selectors and actual target/source closure verified here`; `FIELD=owner map in data-model.md`; `FLOW=production telemetry-to-proposal path plus read-only full-path owner trace`; `PO=native batch closure artifact with five-lane review, shared C++ oracle result and four-category retrospective`; `Design status=PASS required before T006`; `Risk class=parser/crypto/async/cache`; sanitizer/fuzz only under the frozen batch card.

## Phase 6: Real MiniNDN Qualification

**Purpose**: Verify complete production behavior and measured user value without relaxing host/install constraints.

- [ ] T006 在 T005 `PASS` 后运行冻结的 installed-system MiniNDN A/B candidate pair：相同 model、graph、binary（仅 strategy mode 不同）、Provider identities、input、topology、concurrency 和 cleanup；使用 asymmetric topology 验证目标收益，并加入 symmetric control。记录完整 Prepare/Repo/ACK/Selection/placement-bound fetch/assembly/execute/terminal/cleanup、baseline/V4 p50/p95、snapshot freshness、strategy decision time、installed binary/library hashes、首个失败边界及 run-scoped artifacts。任何 startup/resource/protocol/cleanup 失败均不计 PASS，并遵循规定的 static-review retry loop。

## Current Checkpoint

2026-09-28 23:48 -05:00 — 将任务从 7 项收敛为 6 项：删除独立的源码/契约/API/PDF 预备任务，把 Core telemetry 源码核查和契约同步并入首个代码任务；V4 wire/security、snapshot、策略决策分别并入所属实现；共享批次与 MiniNDN 验收保留。所有 feature 任务仍 `NOT_STARTED`；本轮只改共享规则和 Spec 计划/任务结构，没有修改产品源码、运行产品测试/构建或启动实验。细节及检查记录见 [code-first task review](evidence/current-source-audit.md#code-first-task-restructure)。下一步是 T001 的源码绑定、C++ 实现与定向静态审查。
