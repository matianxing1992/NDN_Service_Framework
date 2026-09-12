---

description: "Task list template for feature implementation"
---

# Tasks: [FEATURE NAME]

<!--
  DOCUMENT LANGUAGE POLICY (constitution 1.4.0, 2026-09-05):
  中文写叙述性内容：Phase 的 Purpose/Goal/Checkpoint、任务边界说明的正文。
  保持英文：任务 ID（T001）、[US1]/[P] 标签、`- [ ] T001 ...` 整行格式、
  文件路径、状态词。旧 spec 不回译。
-->

**Input**: Design documents from `/specs/[###-feature-name]/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: The examples below include test-first work inside behavioral tasks.
Tests may be omitted only when the feature has no code-backed acceptance. For
native NDNSF-DI runtime, protocol, state, concurrency, crypto, or model behavior,
the task MUST provide C++ fixture/driver/oracle code that names and runs a production
C++ target/selector. Python may orchestrate an external facility or launch that C++
executable; Python-only checks remain binding/facade, oracle, or external-facility evidence.
If the native path is asynchronous or detached, the fixture MUST own external Face,
io_context, scheduler/timer, and callback dependencies until worker release, or provide
an explicit join/drain barrier; the task's static gate MUST inspect destruction order and
the runtime gate MUST repeat the named selector. Do not change production close/callback
semantics to accommodate a fixture lifetime race.
Each code-backed task MUST also record a `Risk class`, a `Dynamic profile` (`asan-ubsan`,
`tsan`, `parser-fuzz`, or `none`), and the `Dynamic invariants` it will check. The profile is
risk-based and runs at batch validation time; it is not a per-task full rebuild. Before that
run, the batch evidence MUST provide one `Dynamic gate card` with parameter boundaries,
production C++ selectors, repeat/budget, toolchain/source identity, output path, and failure
classification. The card MUST also include a bounded `Dynamic Parameter Matrix` that maps
equivalence-class inputs (normal, boundary, invalid, and relevant cancel/replacement cases) to
expected C++ business results. Execute the card as one batch-level Freeze → Sample → Run → Classify
loop; do not create per-parameter or per-inherited-row tasks. A pure documentation task records `N/A` and a reason. Sanitizer suppression
does not qualify as `DYNAMIC_PASS` unless a clean, ABI-consistent run follows.

**Organization**: Tasks are grouped by user story and expressed as cohesive,
reviewable behavioral outcomes. Do not optimize for a high task count.

## Logical Batches

生成时应用 `skills/speckit-code-design/references/pre-test-static-review.md` 与
`skills/speckit-code-design/references/batch-quality-gates.md` 及其中的只读
review-agent profile。在本节登记每批 ID、成员、连贯行为边界、稳定出口、
implementation / acceptance dependencies、共享构建/测试选择器及负责人。逐任务静态通过后继续同批，
整批流程审查后统一构建/测试；测试待运行保持 PARTIAL，并在 Evidence / Remaining 写
`STATIC_PASS / TESTS_DEFERRED / Batch ID`。硬验收依赖不自动降级；不能把整个 Spec 默认作为一批。
具体批次表取代本段提示。

## Execution Progress

本表是所有模型和执行者共用的当前执行进度入口，必须列全所有可执行单元。
生成时用真实任务替换示例行；每行包含详情链接、状态、依赖、证据/剩余项和更新时间。
未拆分任务直接使用 T001 等 ID；需要拆分的父任务使用 T001-A 等稳定子任务 ID，
所有子任务逐行展示，父任务保留下方阶段验收，不强迫每个任务增加一层子任务。
详细说明至少明确成果、阅读入口、修改范围、步骤与验收，可放本文件或链接契约。

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [UNIT-ID and title](DETAIL-LINK) | NOT_STARTED | — | 待执行及验收 | YYYY-MM-DD |

状态：NOT_STARTED、READY、IN_PROGRESS、PARTIAL、BLOCKED、DONE。
READY 必须满足依赖和阶段门禁；DONE 必须有完整单元验收证据。
每个单元开始时标 IN_PROGRESS，成功、失败或阻塞后更新结果，提交及回复前核对。
新增、拆分、修复和验证工作先登记再执行；重新生成任务时保留稳定 ID、既有进度和证据。
父任务全部子任务 DONE 且自身验收满足后才勾选；正式资格验收独立，不能由局部测试替代。
生成后检查执行单元与进度行一一对应、链接/依赖有效、无重复 ID、无提前完成标记。
完整规则见 [task progress registry](../../skills/speckit-code-design/references/task-progress.md)。

## Current Checkpoint

记录最近工作单元的实际结果、持久证据与下一步；当前状态以 Execution Progress 为准。
生成时用实际已知情况替换说明，不将生成任务本身计为产品完成，也不重置已有 checkpoint。

## Batch Quality Record

每批只维护一份结果记录，引用 [batch-quality-gates](../../skills/speckit-code-design/references/batch-quality-gates.md)。
批末填写以下字段；没有发现时写 `none` 或 `not observed`，不留空。耗时只能用于相同
target/source closure、toolchain、配置和工作树条件下的对照，不能由单次运行推导总体提速。

| Batch ID | Coverage matrix | Static findings | Compile/build misses | Runtime/test misses | Dynamic validation | Build scope / target / `-j` / elapsed / exit | Review trace / closure decision | Behavior result | Evidence / remaining |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| B-01 | [five lanes: status, file/symbol, query/check] | [found/fixed/gaps] | [compiler/linker/wiring misses] | [runtime/test misses and first boundary] | [risk/profile/invariants/build-output/selector/repeat/exit: NOT_RUN or DYNAMIC_PASS/FAIL] | [command, closure, toolchain, timing, log] | [review-agent path/SHA, baseline/diff; CLOSED_FOR_VALIDATION or OPEN_FOR_NEXT_BATCH and trigger] | [STATIC_PASS / BUILD_PASS / FOCUSED_BEHAVIOR_PASS / QUALIFICATION_PASS] | [links and next step] |

每条批次结果还必须附一段 `Batch Retrospective`：分别记录 static、compile/link、runtime/test
和 unobserved 漏检（无观察写 `none`/`not observed`），说明是否在稳定出口后继续吸收职责，
并注明耗时是否可比。缺少该复盘时保持 `PARTIAL`；它不能由任务数、静态通过数或单次构建
时长替代。
批次还要写 `Batch growth decision`，说明稳定出口前的成员分配依据，以及出口出现后是否
立即停止扩张；若新增成员改变入口、状态机、selector、source closure 或硬验收依赖，先
关闭当前批次并登记新的 Batch ID。
`--help`、usage/schema rejection、可执行文件存在或 harness-start smoke 只能证明命令、
target/link 或外部设施接线；没有真实生产请求/结果时，不得把它们写成 native behavior、
Python/C++ parity 或 qualification PASS。
若本批是失败重试或同类漏检后的下一批，`Evidence / remaining` 还必须链接首个失败边界，
写明本次改变的静态检查及其覆盖 lane；重复同类漏检时先修订共享 skill、模板或 checklist，
或记录明确的替代门禁。只重跑原命令不构成漏检修复。
`Review trace` 必须链接 `references/review-agent.md` 的 Minimum Review Record，并在
`test/harness/oracle` lane 中列出测试注册，在 `build/source closure` lane 中列出实际
target/source closure；对于新增入口、跨库调用或链接重试，还要列出 project-symbol
definition map（符号→定义 translation unit→target/library）及 `rg`/CodeGraph、
`nm -C`/`readelf` 查询；只有 `No findings` 而没有这些行时仍属于 coverage gap。

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Task Cohesion Rule

A task is the smallest independently reviewable outcome, not the smallest
possible action. Keep test-first work, implementation, focused validation, and
evidence together when they close one behavior under one acceptance gate.

Split work only when the resulting tasks are independently assignable and
mergeable, establish a real prerequisite, carry materially different risk, or
produce independently meaningful acceptance evidence. Do not create separate
tasks merely to:

- edit different files for the same behavior;
- write a test, implement its behavior, run that test, and record its result;
- run multiple commands that form one validation gate;
- update a contract and its directly corresponding implementation.

Every code-backed task description must identify the applicable production
entry/caller, test/harness/oracle, and build/source registration. If one lane is
not applicable, record `N/A` with a reason in the batch result record; if it is
unknown, keep the task `PARTIAL` until the gap is closed.
When a fixture or oracle reconstructs canonical bytes, digests, or identities,
the batch record must name the production serializer and check its field order,
normalization, and source identity; an unverified reconstruction is a coverage gap.

Before implementation, record why each member belongs to its batch: shared
production entry/caller, interface/state/ownership contract, independent
oracle/test selector, source/build closure, and acceptance exit. Split when one
of these differs; never keep a stable batch open only to avoid another build.

Example:

```text
# Avoid mechanical fragmentation
- [ ] T010 [US1] Add failing contract test for token expiry in tests/contract/test_tokens.py
- [ ] T011 [US1] Implement token expiry in src/services/token_service.py
- [ ] T012 [US1] Run the token test and record evidence in evidence/token-expiry.md

# Prefer one cohesive behavioral task
- [ ] T010 [US1] Enforce token expiry by adding the failing contract case, implementing the service behavior, and recording the passing focused gate in tests/contract/test_tokens.py, src/services/token_service.py, and evidence/token-expiry.md
```

## Design binding

每项实现任务引用具体Class/FN/FIELD/FLOW/PO及Design status；见
`skills/speckit-code-design/references/work-unit-contract.md#design-binding-before-coding`。
只有“修改某文件/接入某类”或公开方法清单不足以编码。先补关键签名、owner和失败收尾；
缺口只阻止受影响范围，LOCAL_DETAIL不需单独批准。非代码任务说明N/A及实际产物/验收。

## Design-Code Convergence Gate

Every durable feature MUST include one post-implementation, pre-qualification
convergence task. It MUST freeze the accepted design authority, inspect the real
production entry points/callers/wiring/effective configuration with CodeGraph
and exact source verification, map every requirement to its implementation and
focused regression, and write a severity-classified gap report. Checked boxes,
helpers that are not called by production, isolated unit tests, and historical
evidence are not proof of production conformance.

Any controlling discrepancy reopens or creates a cohesive repair task with its
focused failing test, implementation, and closing regression. The convergence
task closes only after re-audit reports `PASS`. Every complete unit/integration
suite, MiniNDN/system test, SIF/container qualification, cluster job, benchmark,
and experiment task MUST depend on that PASS. Focused red/green and diagnostic
tests remain allowed during implementation and repair.

## Expensive Execution Closure Rule

When a plan includes large artifacts, containers, remote staging, GPU/cluster
allocation, or long campaigns, tasks MUST implement and mutation-test the
candidate-closure gate before any task performs that expensive action. Keep the
gate implementation and its focused mutation coverage in one cohesive task;
keep the later immutable-candidate execution in a dependent task.

The closure task MUST bind every candidate plane, compute the invalidation and
restart gate, reject stale or cross-candidate evidence, and prove zero upload,
remote mutation, staging, scheduler, or campaign calls on failure. The execution
task MUST accept only that closed candidate and must require protocol, fresh
result, child-exit, and cleanup agreement before recording PASS.

## Path Conventions

- **Single project**: `src/`, `tests/` at repository root
- **Web app**: `backend/src/`, `frontend/src/`
- **Mobile**: `api/src/`, `ios/src/` or `android/src/`
- Paths shown below assume single project - adjust based on plan.md structure

<!--
  ============================================================================
  IMPORTANT: The tasks below are SAMPLE TASKS for illustration purposes only.

  The /speckit-tasks command MUST replace these with actual tasks based on:
  - User stories from spec.md (with their priorities P1, P2, P3...)
  - Feature requirements from plan.md
  - Entities from data-model.md
  - Endpoints from contracts/

  Tasks MUST be organized by user story so each story can be:
  - Implemented independently
  - Tested independently
  - Delivered as an MVP increment

  Before writing the final list, coalesce items that share the same behavior,
  owner, dependency position, and acceptance gate. Do not generate one task per
  file, test case, shell command, or evidence artifact.

  DO NOT keep these sample tasks in the generated tasks.md file.
  ============================================================================
-->

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization and basic structure

- [ ] T001 Create project structure per implementation plan
- [ ] T002 Initialize [language] project with [framework] dependencies
- [ ] T003 [P] Configure linting and formatting tools

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

Examples of foundational tasks (adjust based on your project):

- [ ] T004 Setup database schema and migrations framework
- [ ] T005 [P] Implement authentication/authorization framework
- [ ] T006 [P] Setup API routing and middleware structure
- [ ] T007 Create base models/entities that all stories depend on
- [ ] T008 Configure error handling and logging infrastructure
- [ ] T009 Setup environment configuration management

**Checkpoint**: Foundation ready - user story implementation can now begin in parallel

---

## Phase 3: User Story 1 - [Title] (Priority: P1) 🎯 MVP

**Goal**: [Brief description of what this story delivers]

**Independent Test**: [How to verify this story works on its own]

### Behavioral Tasks for User Story 1

> **NOTE**: When tests are requested, each behavioral task states the required
> failing test first, then implementation and the focused passing gate.

- [ ] T010 [P] [US1] Deliver [independent behavior A] with a failing contract case, implementation, and focused validation in tests/contract/test_[name].py and src/[location]/[file].py
- [ ] T011 [US1] Deliver [independent behavior B], including its model/service changes, error handling, diagnostics, and integration gate in src/models/[entity].py, src/services/[service].py, and tests/integration/test_[name].py

**Checkpoint**: At this point, User Story 1 should be fully functional and testable independently

---

## Phase 4: User Story 2 - [Title] (Priority: P2)

**Goal**: [Brief description of what this story delivers]

**Independent Test**: [How to verify this story works on its own]

### Behavioral Tasks for User Story 2

- [ ] T012 [P] [US2] Deliver [independent behavior] with its test-first contract, model/service implementation, and focused evidence in tests/[name].py, src/models/[entity].py, and src/services/[service].py
- [ ] T013 [US2] Integrate [behavior] at the established US1 boundary and close the independent story gate in src/[location]/[file].py and tests/integration/test_[name].py

**Checkpoint**: At this point, User Stories 1 AND 2 should both work independently

---

## Phase 5: User Story 3 - [Title] (Priority: P3)

**Goal**: [Brief description of what this story delivers]

**Independent Test**: [How to verify this story works on its own]

### Behavioral Tasks for User Story 3

- [ ] T014 [US3] Deliver [independent behavior] across its model, service, endpoint, test-first coverage, and focused acceptance gate in src/ and tests/

**Checkpoint**: All user stories should now be independently functional

---

[Add more user story phases as needed, following the same pattern]

---

## Phase N: Polish & Cross-Cutting Concerns

**Purpose**: Improvements that affect multiple user stories

- [ ] TXXX [P] Documentation updates in docs/
- [ ] TXXX Code cleanup and refactoring
- [ ] TXXX Performance optimization across all stories
- [ ] TXXX [P] Additional unit tests (if requested) in tests/unit/
- [ ] TXXX Security hardening
- [ ] TXXX Run quickstart.md validation

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3+)**: All depend on Foundational phase completion
  - User stories can then proceed in parallel (if staffed)
  - Or sequentially in priority order (P1 → P2 → P3)
- **Polish (Final Phase)**: Depends on all desired user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational (Phase 2) - No dependencies on other stories
- **User Story 2 (P2)**: Can start after Foundational (Phase 2) - May integrate with US1 but should be independently testable
- **User Story 3 (P3)**: Can start after Foundational (Phase 2) - May integrate with US1/US2 but should be independently testable

### Within Each User Story

- 每个小任务编码及测试编写后使用只读 review-agent profile 静态门；同批成员静态通过后继续编码，整批逻辑/流程审查通过后统一构建和相关测试。仅用户或 Spec 明确要求 TDD 时执行具名 RED。
- Respect model, service, endpoint, and integration dependencies inside the task
- Split those steps into separate tasks only when they meet the Task Cohesion Rule
- Story complete before moving to next priority

### Parallel Opportunities

- All Setup tasks marked [P] can run in parallel
- All Foundational tasks marked [P] can run in parallel (within Phase 2)
- Once Foundational phase completes, all user stories can start in parallel (if team capacity allows)
- All tests for a user story marked [P] can run in parallel
- Models within a story marked [P] can run in parallel
- Different user stories can be worked on in parallel by different team members

---

## Parallel Example: User Story 1

```bash
# Launch independent behavioral outcomes together:
Task: "Deliver [behavior A] with test-first coverage, implementation, and focused gate"
Task: "Deliver [behavior B] with test-first coverage, implementation, and focused gate"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL - blocks all stories)
3. Complete Phase 3: User Story 1
4. **STOP and VALIDATE**: Test User Story 1 independently
5. Deploy/demo if ready

### Incremental Delivery

1. Complete Setup + Foundational → Foundation ready
2. Add User Story 1 → Test independently → Deploy/Demo (MVP!)
3. Add User Story 2 → Test independently → Deploy/Demo
4. Add User Story 3 → Test independently → Deploy/Demo
5. Each story adds value without breaking previous stories

### Parallel Team Strategy

With multiple developers:

1. Team completes Setup + Foundational together
2. Once Foundational is done:
   - Developer A: User Story 1
   - Developer B: User Story 2
   - Developer C: User Story 3
3. Stories complete and integrate independently

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable
- 明确要求 TDD 时按具名 RED 规则执行；默认逐任务静态门、批末构建测试
- 逻辑批次验收后按仓库规则 checkpoint；静态通过但测试待执行不提前标 DONE
- Stop at any checkpoint to validate story independently
- Avoid: vague tasks, same file conflicts, cross-story dependencies that break independence, and mechanical test/implementation/evidence fragmentation
