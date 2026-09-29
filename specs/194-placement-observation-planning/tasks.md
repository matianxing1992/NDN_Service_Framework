# Tasks: Placement Observation Collection and Planning

**Input**: Design documents from `specs/194-placement-observation-planning/`
**Prerequisites**: [spec.md](spec.md), [plan.md](plan.md), [research.md](research.md), [data-model.md](data-model.md), [contracts/placement-observation-v1.md](contracts/placement-observation-v1.md)
**Execution mode**: STRICT_SERIAL

## Prior-work disposition

All active local Spec work on the discarded pre-reset `Experimental` workset, including Spec193, is `STOPPED / NOT COMPLETE`. No old task is resumed or carried into this task list. The old ignored Spec directories are absent after the user-authorized reset/clean; their removed checkboxes are not completion evidence. Historical documents already present in `origin/AutoInstall` keep their own recorded status and are not rewritten by this disposition.

## Execution Progress

| Task | Status | Depends on | Evidence | Updated | Remaining acceptance |
|---|---|---|---|---|---|
| [T001](#phase-1-source-and-design-closure) | READY | — | [current source audit](evidence/current-source-audit.md) | 2026-09-28 22:50 -05:00 | Close telemetry producer/consumer and graph mapping; locate authenticated V4 capability advertisement; freeze exact V4 canonical/signature/policy/epoch, score/budget, native batch composition, test registration/source closure and named C++ target/selector; update accepted target/API/design docs and PDFs; pass read-only semantic review. |
| [T002](#phase-2-core-telemetry-producer-and-access) | NOT_STARTED | T001 | None | 2026-09-28 23:15 -05:00 | Core typed directed telemetry and User/Provider C++ assertions are added with design/API evidence; execute their selectors only in the shared T006 gate. |
| [T003](#phase-3-signed-provider-observation-offer) | NOT_STARTED | T002 | None | 2026-09-28 23:15 -05:00 | Authenticated V4 capability and signed candidate-scoped offer assertions are added; preserve V3 bytes and execute selectors only in T006. |
| [T004](#phase-4-immutable-request-snapshot) | NOT_STARTED | T003 | None | 2026-09-28 23:15 -05:00 | Whole-roster profile and immutable snapshot/sealer assertions are added; execute selectors only in T006 and prove stale/tamper/replay rejection. |
| [T005](#phase-5-observation-aware-placement) | NOT_STARTED | T004 | None | 2026-09-28 23:15 -05:00 | Deterministic observation-aware role-DAG policy preserves hard/preference/fallback semantics; assertions are added and executed once in T006. |
| [T006](#phase-6-native-batch-and-full-path-convergence) | NOT_STARTED | T005 | None | 2026-09-28 23:15 -05:00 | One combined read-only full-path/composition review; if clean, run the shared native target and selectors once, then record all five lanes and four miss categories. Findings block this gate and require a separately registered repair before it resumes. |
| [T007](#phase-7-real-minindn-qualification) | NOT_STARTED | T006 | None | 2026-09-28 23:15 -05:00 | Real installed-system MiniNDN two-Provider A/B, complete production lifecycle, paired latency results meet SC-004, and cleanup/identity gates pass. |

## Phase 1: Source and Design Closure

**Purpose**: Freeze the measured source, authenticated V4 capability-discovery point, endpoint identity, canonical V4 wire, strategy cost order/budget, native batch membership, exact build/source closure and named C++ oracle before production edits.

- [ ] T001 Freeze the Core telemetry-to-DI V4 contract and Design binding using the real `NetworkTelemetryStore` call sites, User large-data path, authenticated capability-discovery point, Provider offer caller, Native role dependency mapping and ACK/signature policy. Freeze the batch-growth decision, actual test registration/source closure, build target(s), caller signatures and C++ oracle selectors. Produce exact before/after signatures and failure flows in the contract; update only accepted target/API/design artifacts and verify their PDF provenance. Required outputs: closed questions in `contracts/placement-observation-v1.md`, `Design/spec-design-changes.md`, target references/PDFs, T001 source-gate evidence. No production code changes.

## Phase 2: Core Telemetry Producer and Access

**Purpose**: Expose accurate, bounded, typed directed samples from real events, retaining Core ownership and not adding synthetic RTTs.

- [ ] T002 [US1] Add focused C++ coverage and production access for User-side ACK/data transfer and Provider-to-Provider dependency fetch snapshots through `NetworkTelemetryStore`, `ServiceUser`, and `ServiceProvider`. Keep ACK RTT distinct from data-fetch first-byte/elapsed/goodput; bind observer and source expiry; don't export Data names/key scopes; record no-sample/stale states. Update Core API/Chinese behavior contract and evidence in the same work unit.

**Design binding**: `CD=NetworkTelemetrySample/Snapshot/Store + User/Provider access`; `FN=typed sample update/query signatures frozen by T001`; `FIELD=Core owns sample windows, kind, counters, TTL and endpoint keys`; `FLOW=real ACK/fetch completion -> bounded snapshot -> DI consumer`; `PO=Core production C++ selector with successful, failed, stale and direction-swapped samples`; `Design status=DRAFT until T001`; `Risk class=async transport state`; dynamic profile frozen by T006.

## Phase 3: Signed Provider Observation Offer

**Purpose**: Make current, request-scoped Provider capacity/residency and P→P rows available before Selection without fetching model material.

- [ ] T003 [US1] [US2] Add an authenticated Provider capability advertisement for `DI_PLACEMENT_V4` at the source path frozen by T001, then add the separately versioned production offer/decoder wired from the real executable ACK callback and telemetry/cache owners. Sign exact canonical content and bind request/attempt/model/graph/provider boot and expiry. Encode observed/known-empty/unknown distinctly; keep residency limited to current candidate digests; reject malformed, duplicate, wrong-epoch, stale and non-finite fields. Preserve V3 canonical offer bytes byte-for-byte. Include C++ capability, V4 offer/admission regressions, Design/API reference update and evidence.

**Design binding**: `CD=placement-observation-v1.md V4 envelope`; `FN=capability advertisement and V4 issuer/decoder/admission signatures frozen in T001`; `FIELD=Provider owns capability, offer/resource/cache rows; Core owns generic sample facts; key policy owns signer trust`; `FLOW=authenticated capability discovery + ACK decision + current snapshots -> canonical signed V4 -> Core ACK authenticated -> DI admission`; `PO=named production C++ capability/V4 signer/decoder/admission selectors`; `Design status=READY only after T001`; `Risk class=canonical parser/crypto and cache side channel`; dynamic profile frozen by T006.

## Phase 4: Immutable Request Snapshot

**Purpose**: Join verified provider facts with requester-local observations and the current graph into one freshness-bound strategy input.

- [ ] T004 [US1] [US2] After fixing the complete eligible candidate roster, select one request-level profile: use V4 only when every candidate advertises authenticated V4 support; otherwise keep the entire request/attempt on the existing V3/default path. Reject mixed V3/V4 offers. For V4, add the native snapshot builder to combine locally observed User→Provider samples, Provider edges/resources/residency and the exact prepared role graph. Reject replay, stale/unknown required path, changed endpoint boot/topology, digest mismatch or graph/offer mismatch. Bind snapshot digest into proposal/sealed plan and lifecycle evidence; add independent C++ negative oracles, API/design updates and evidence.

**Design binding**: `CD=request-level profile decision + PlacementObservationSnapshot + snapshot/selection binding`; `FN=whole-roster profile selector, builder, validator and proposal binding frozen in T001`; `FIELD=User planner owns immutable snapshot/profile decision; Core ACK and DI offer validators own independent authentication`; `FLOW=freeze eligible roster -> select one profile -> ACK closure -> authenticate offers -> snapshot -> propose -> seal`; `PO=named production C++ profile/snapshot/sealer selectors`; `Design status=READY only after T001/T003`; `Risk class=replay/TOCTOU and async cancellation`; dynamic profile frozen by T006.

## Phase 5: Observation-Aware Placement

**Purpose**: Rank complete feasible assignments with real directed costs while preserving existing admission, caller preference, spread and deterministic fallback behavior.

- [ ] T005 [US3] Add a separate V4 observation-aware strategy over the immutable graph/snapshot. In stable topological order, accumulate measured queue/preparation and directed edge contributions for feasible role/provider choices; use only present tensor bytes and valid `DATA_FETCH` data. Count User ingress/egress only when the plan declares the transfer and its byte size is known; unknown size is never zero. Preserve V3 strategy and bytes unchanged. If observations are incomplete/stale after V4 was authenticated, keep the V4 profile and apply current baseline ranking semantics to authenticated non-network V4 fields; V3/default is used only when selected before request due incomplete capability coverage. Bind policy version, snapshot digest, cost breakdown and fallback reason. Add C++ independent hand-calculated and determinism oracles; update Design/API contract and evidence.

**Design binding**: `CD=PlacementCostBreakdown + V4 strategy order`; `FN=proposeRolesV4 signature and cost comparator frozen by T001`; `FIELD=DI owns graph/bytes/cache/strategy; Core does not score model roles`; `FLOW=validated V4 snapshot -> hard feasibility -> bounded deterministic cost ranking -> validated proposal/seal`; `PO=named C++ production strategy and proposal validator`; `Design status=READY only after T001/T004`; `Risk class=algorithm bounds and cancellation`; dynamic profile frozen by T006.

## Phase 6: Native Batch and Full Path Convergence

**Purpose**: Close the native batch once, including the full-path static convergence review, before system/network qualification.

- [ ] T006 After T002–T005 each passes its own read-only static gate, run one combined batch composition and full-path convergence review over `source→identity/catalog→prepare→material/Repo→ACK/Selection→placement fetch→assembly→runner/execute→terminal→cleanup`. Freeze source identity, actual target/source closure and `Dynamic gate card`; record all five lanes, severity/owner findings, the review-agent trace, stable observable exit, and `static`/`compile-link`/`runtime-test`/`unobserved` retrospective in `evidence/native-batch-closure.md`. Only if this review has no blocking gap, build the shared `integration-tests` target and run the named C++ selectors for T002–T005 once as a batch. T002–T005 add their C++ assertions but defer native build/selector execution to this shared gate. T006 is verification-only: do not repair production code inside it. A blocking finding leaves T006 `BLOCKED`; register a dependency-ordered repair unit before resuming T006, then repeat the affected static review and shared batch gate. Python may orchestrate the target but cannot assert native behavior.

**Design binding for T006**: `CD=all accepted Spec194 contracts and batch exit`; `FN=registered C++ selectors and actual target/source closure frozen in T001`; `FIELD=owner map in data-model.md`; `FLOW=production telemetry-to-proposal path plus read-only full-path owner trace`; `PO=native batch closure artifact with five-lane review, shared C++ oracle result and four-category retrospective`; `Design status=PASS required before T007`; `Risk class=parser/crypto/async/cache`; sanitizer/fuzz only under the frozen batch card.

## Phase 7: Real MiniNDN Qualification

**Purpose**: Verify complete production behavior and measured user value without relaxing host/install constraints.

- [ ] T007 After T006 `PASS`, run a frozen installed-system MiniNDN A/B candidate pair: same model, graph, binaries except the strategy mode, provider identities, input, topology, concurrency and cleanup; asymmetric topology for the target benefit and symmetric control. Record the complete Prepare/Repo/ACK/Selection/placement-bound fetch/assembly/execute/terminal/cleanup evidence, baseline and V4 p50/p95, snapshot freshness, strategy decision time, actual installed binary/library hashes, first boundary, and run-scoped artifacts. Any startup/resource/protocol/cleanup failure remains non-PASS and follows the required static-review retry loop.

## Current Checkpoint

2026-09-28 23:15 -05:00 — 合并原生批次验证与完整生产路径静态收敛为一个 T006；T002–T005 逐项静态审查并编写 C++ assertions，原生目标和 selectors 只在共享批次门运行一次。T007 单独负责真实 MiniNDN 资格验收；追踪表逐项列出 FR-001–FR-012、SC-001–SC-006，包含此前未映射的 FR-007、FR-010 和 FR-011。没有修改产品源码，也没有运行产品测试、构建或实验。检查结果和限制记录在 [current-source-audit.md](evidence/current-source-audit.md)。旧 Spec 工作仍为 `STOPPED / NOT COMPLETE`。T001 仍为 `READY`；V4 canonical/schema/source epoch、批次源码闭包及目标 API/Design 同步仍待关闭。
