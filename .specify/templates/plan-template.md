# Implementation Plan: [FEATURE]

<!-- 工作流采用 skills/speckit-code-design/references/pre-test-static-review.md 与
     batch-quality-gates.md：设计先行；声明逻辑批次及实现/验收依赖；每小任务只读静态审查后继续同批；
     整批逻辑/流程审查后统一构建和相关测试。批次表在 plan 或 tasks 只定义一次，另一处引用；未测试不标 DONE。
     每批还要冻结稳定行为出口、生产入口/调用方/测试与构建接线范围，并记录静态、编译/链接、运行漏检分类和耗时。 -->

<!--
  DOCUMENT LANGUAGE POLICY (constitution 1.4.0, 2026-09-05):
  中文写叙述性内容：Summary、Architecture Decisions 的动机与取舍、修订历史、
  诊断记录、迁移说明的正文。
  保持英文：节标题、门名（G0/S0/T001 等）、状态词、契约 JSON、字段名、
  文件路径、哈希、命令。旧 spec 不回译。
-->

**Branch**: `[###-feature-name]` | **Date**: [DATE] | **Spec**: [link]

**Input**: Feature specification from `/specs/[###-feature-name]/spec.md`

**Note**: This template is filled in by the `/speckit-plan` command. See `.specify/templates/plan-template.md` for the execution workflow.

## Summary

[Extract from feature spec: primary requirement + technical approach from research]

## Technical Context

<!--
  ACTION REQUIRED: Replace the content in this section with the technical details
  for the project. The structure here is presented in advisory capacity to guide
  the iteration process.
-->

**Language/Version**: [e.g., Python 3.11, Swift 5.9, Rust 1.75 or NEEDS CLARIFICATION]

**Primary Dependencies**: [e.g., FastAPI, UIKit, LLVM or NEEDS CLARIFICATION]

**Storage**: [if applicable, e.g., PostgreSQL, CoreData, files or N/A]

**Testing**: [e.g., pytest, XCTest, cargo test or NEEDS CLARIFICATION]

**Target Platform**: [e.g., Linux server, iOS 15+, WASM or NEEDS CLARIFICATION]

**Project Type**: [e.g., library/cli/web-service/mobile-app/compiler/desktop-app or NEEDS CLARIFICATION]

**Performance Goals**: [domain-specific, e.g., 1000 req/s, 10k lines/sec, 60 fps or NEEDS CLARIFICATION]

**Constraints**: [domain-specific, e.g., <200ms p95, <100MB memory, offline-capable or NEEDS CLARIFICATION]

**Scale/Scope**: [domain-specific, e.g., 10k users, 1M LOC, 50 screens or NEEDS CLARIFICATION]

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

[Gates determined based on constitution file]

The plan MUST identify cohesive behavioral slices and their acceptance gates.
It MUST NOT prescribe a future task for every file, function, test command, or
evidence document when those actions jointly deliver one behavior.

The plan MUST place one code-aware design-to-code convergence gate after
implementation/focused repair tests and before any complete unit/integration
suite, system/network qualification, benchmark, or experiment is accepted as
evidence. It MUST identify the production entry points and effective
configuration to inspect, the specification/contracts that control them, the
required traceability report, the severity-to-verdict rule, and the changes
that trigger re-audit. An unresolved semantic, architecture, security,
production-wiring, or evidence-validity discrepancy MUST block formal
validation.

When the feature uses large artifacts, containers, remote staging, GPU/cluster
allocation, or long campaigns, the plan MUST also define:

- one immutable candidate tuple across source, runtime artifact, replay/test
  harness, submit bundle, effective configuration, external artifacts, and the
  validation contract;
- a change-plane invalidation matrix with the earliest restart gate;
- a repository-owned pre-dispatch closure gate whose rejected mutations make
  zero upload, remote mutation, staging, scheduler, or campaign calls;
- real-path readiness evidence where local markers are insufficient;
- terminal acceptance requiring protocol/result/child-exit/cleanup agreement;
- one active subject per candidate and gate, with no cross-candidate PASS reuse.

## Pre-Qualification Design-Code Convergence

**Design authority**: [exact spec/plan/contracts/invariants that implementation must match]

**Production paths to inspect**: [public entry points, runtime callers, wiring,
effective configuration, security owners, and evidence-producing paths]

**Required audit artifact**: [path to code-aware gap report and traceability map]

**Closure rule**: [all controlling gaps repaired with focused regressions;
post-implementation audit verdict PASS]

**Formal validation boundary**: [complete unit/integration, MiniNDN, SIF,
cluster, benchmark, or experiment tasks that depend on the PASS verdict]

**Re-audit triggers**: [behavior-affecting source, design, dependency,
configuration, harness, or evidence-contract changes]

Follow `.specify/memory/design-code-convergence.md`; feature-specific rules may
be stricter but must not weaken its PASS/BLOCK boundary.

## Logical Batch Quality Plan

<!--
  Define each independently verifiable behavior batch once. Do not grow a batch
  after its stable exit merely to avoid another build. Use the shared reference
  for required coverage and result fields. For documentation-only work, record
  N/A with the reason instead of inventing a code batch.
-->

| Batch ID | Behavior boundary / stable exit | Members | Implementation dependencies | Acceptance dependencies | Shared build/test selector and owner |
| --- | --- | --- | --- | --- | --- |
| B-01 | [observable behavior and exit] | [task IDs] | [IDs or —] | [hard gates or —] | [selector / owner] |

## Project Structure

### Documentation (this feature)

```text
specs/[###-feature]/
├── plan.md              # This file (/speckit-plan command output)
├── research.md          # Phase 0 output (/speckit-plan command)
├── data-model.md        # Phase 1 output (/speckit-plan command)
├── quickstart.md        # Phase 1 output (/speckit-plan command)
├── contracts/           # Phase 1 output (/speckit-plan command)
└── tasks.md             # Phase 2 output (/speckit-tasks command - NOT created by /speckit-plan)
```

### Source Code (repository root)
<!--
  ACTION REQUIRED: Replace the placeholder tree below with the concrete layout
  for this feature. Delete unused options and expand the chosen structure with
  real paths (e.g., apps/admin, packages/something). The delivered plan must
  not include Option labels.
-->

```text
# [REMOVE IF UNUSED] Option 1: Single project (DEFAULT)
src/
├── models/
├── services/
├── cli/
└── lib/

tests/
├── contract/
├── integration/
└── unit/

# [REMOVE IF UNUSED] Option 2: Web application (when "frontend" + "backend" detected)
backend/
├── src/
│   ├── models/
│   ├── services/
│   └── api/
└── tests/

frontend/
├── src/
│   ├── components/
│   ├── pages/
│   └── services/
└── tests/

# [REMOVE IF UNUSED] Option 3: Mobile + API (when "iOS/Android" detected)
api/
└── [same as backend above]

ios/ or android/
└── [platform-specific structure: feature modules, UI flows, platform tests]
```

**Structure Decision**: [Document the selected structure and reference the real
directories captured above]

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| [e.g., 4th project] | [current need] | [why 3 projects insufficient] |
| [e.g., Repository pattern] | [specific problem] | [why direct DB access insufficient] |
