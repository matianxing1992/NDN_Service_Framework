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
Tests are OPTIONAL - include them only when the feature specification requests
them.

**Organization**: Tasks are grouped by user story and expressed as cohesive,
reviewable behavioral outcomes. Do not optimize for a high task count.

## Detailed Execution Progress

维护日期与证据基线（commit / candidate / run，适用时填写）：[UPDATE AT CHECKPOINT]。
本表记录父任务内部可核实的执行步骤；父任务 checkbox 仍是验收完成状态。
不要为了增加细节把同一行为拆成更多顶层任务，也不要用步骤数量推算完成百分比。

| Step | Parent | Concrete outcome / path | State | Evidence / verification scope | Blocker / next action | Reuse / rerun trigger |
| --- | --- | --- | --- | --- | --- | --- |
| T001.a | T001 | [具体结果及文件路径] | NOT_STARTED | NOT_RUN | [依赖或下一步] | [可复用证据及失效条件；无则 N/A] |

生成真实任务时替换示例行，覆盖每个父任务；复杂或部分完成任务使用稳定的
`Tnnn.a`、`Tnnn.b` 子步骤 ID，简单任务可只有一行。状态使用
`NOT_STARTED / IN_PROGRESS / IMPLEMENTED / VERIFIED / BLOCKED / FAILED`。
`IMPLEMENTED` 仅表示代码存在；`VERIFIED` 仅限该行明确声明的验收范围。
源码、组件测试、真实集成和运行测量必须区分；缺证据写 `NOT_RUN` 或
`UNVERIFIED`，不得把 fixture 或旧 candidate 的 PASS 提升为当前运行资格。

每次有效 checkpoint、失败、阻塞变化及 handoff 同步更新本表、父任务 checkbox
和顶部汇总；只有父任务的全部必需验收条件通过才勾选。证据引用具体文件与
命令/结果或 run ID，长日志保留在证据文件中。发生变化先按失效矩阵判断影响，
复用仍匹配的证据；只有行为变化、失败或未决风险才触发相应重测，不因更新
本表或增加子步骤重跑整套测试。审计只读模式报告不一致，不自动改表。

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

Example:

```text
# Avoid mechanical fragmentation
- [ ] T010 [US1] Add failing contract test for token expiry in tests/contract/test_tokens.py
- [ ] T011 [US1] Implement token expiry in src/services/token_service.py
- [ ] T012 [US1] Run the token test and record evidence in evidence/token-expiry.md

# Prefer one cohesive behavioral task
- [ ] T010 [US1] Enforce token expiry by adding the failing contract case, implementing the service behavior, and recording the passing focused gate in tests/contract/test_tokens.py, src/services/token_service.py, and evidence/token-expiry.md
```

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

- Within a behavioral task, tests (if included) MUST be written and fail before implementation
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
- Verify tests fail before implementing
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- Avoid: vague tasks, same file conflicts, cross-story dependencies that break independence, and mechanical test/implementation/evidence fragmentation
