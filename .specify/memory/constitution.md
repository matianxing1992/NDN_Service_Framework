<!--
Sync Impact Report
- Version: 1.3.0 -> 1.4.0
- Added constraint: Document Language Policy (Chinese narrative, English
  structural markers) under Project Constraints.
- Updated templates:
  - ✅ .specify/templates/spec-template.md
  - ✅ .specify/templates/plan-template.md
  - ✅ .specify/templates/tasks-template.md
- Updated runtime guidance:
  - ✅ AGENTS.md
  - ✅ CLAUDE.md
- Updated scripts:
  - ✅ scripts/spec180_contract_gate.py (owner-map marker error message states
    the English-marker convention)
- Removed sections: none
- Deferred items: none
- Version: 1.2.0 -> 1.3.0
- Added principle: VIII. Design-Code Convergence Before Formal Validation
- Modified principle: V. Verify With The Right Scope
- Modified guidance: Spec-driven work and development workflow now require a
  post-implementation production-path audit and repair gate before formal
  validation, in addition to candidate closure before expensive work.
- Updated templates:
  - ✅ .specify/templates/plan-template.md
  - ✅ .specify/templates/spec-template.md
  - ✅ .specify/templates/tasks-template.md
- Updated runtime guidance:
  - ✅ AGENTS.md
  - ✅ added .specify/memory/design-code-convergence.md
  - ✅ .agents/skills/speckit-audit/SKILL.md
- Command templates: reviewed; `.specify/templates/commands/` is not present
- Removed sections: none
- Deferred items: none
-->
# NDNSF Constitution

## Core Principles

### I. Canonical Dynamic Runtime

NDNSF development centers on the generic dynamic C++ runtime API. New work must
use unified `serviceName` paths, V2 request/ACK/selection/response naming, and
the current Targeted terminology for known-provider low-latency calls. Do not
reintroduce generated service/stub classes, split `ServiceName + FunctionName`
APIs, Direct terminology, or framework-specific HELLO wire types.

### II. Security Is Part Of The Data Path

Authorization and execution must preserve the current NAC-ABE, permission,
token, replay-protection, and provider-permission checks. Permission discovery
Data is controller-signed and encrypted to the target identity certificate.
REQUEST and SELECTION map to `/SERVICE/<service>`, while ACK and RESPONSE map to
`/PERMISSION/<service>`. Do not add debug bypasses such as forced
`isAuthorized = true`.

### III. CodeGraph First, Source Verified

For code questions, impact analysis, bug tracing, and source edits, start with
CodeGraph in this indexed repository. Use it to find symbols, callers, callees,
and affected files, then verify final claims against the actual source and
targeted tests. Use `rg` after CodeGraph for exact strings, scripts, logs,
configs, and docs.

### IV. Spec-Driven Changes For Durable Work

Use Spec Kit for new features, protocol/API changes, architecture changes,
evaluation-plan changes, and multi-file work that needs durable requirements.
The normal path is specify, clarify when needed, plan, tasks, analyze when
useful, implement, and converge. Small one-line fixes, direct command-output
requests, and narrow slide/text edits may use the direct development workflow.

### V. Verify With The Right Scope

Testing must match risk. NDNSF security and runtime changes require the focused
regression scripts plus relevant unit tests. Shared protocol or distributed
inference changes require broader unit tests. Network, security, and
performance regressions should use MiniNDN by default; host NFD is only a
temporary diagnostic path unless the user asks otherwise. Performance short
tests should keep a 60-second measured window unless explicitly changed.
Focused red/green and diagnostic tests are part of implementation and repair;
they MUST NOT be presented as formal qualification evidence before the
design-code convergence gate passes.

### VI. Cohesive, Outcome-Based Tasks

Spec Kit tasks MUST represent reviewable behavioral outcomes, not mechanical
file operations or workflow bookkeeping. When one behavior has one owner and
one acceptance gate, its test-first work, implementation, focused validation,
and evidence update MUST remain one task unless a real dependency boundary
requires separation. A task MAY be split only when a part is independently
assignable and mergeable, blocks later work, carries materially different risk,
or has an independently meaningful acceptance result. Task count is not a
quality metric. Reviews MUST flag repeated chains such as “write test”,
“implement”, “run test”, and “record evidence” as over-fragmentation when those
steps only close the same behavior.

### VII. Immutable Promotion For Expensive Execution

A feature that uses large build artifacts, containers, remote staging, GPU or
cluster allocation, or a long benchmark campaign MUST define one immutable
promotion candidate across every behavior-affecting plane. At minimum, the
feature specification and plan MUST identify the source, built runtime,
test/replay harness, submission bundle, effective configuration, external
artifacts, and validation contract that form that candidate. They MUST define a
change-plane invalidation matrix that states which evidence becomes stale and
the earliest gate that must be rerun.

Before any expensive build, upload, remote mutation, model staging, scheduler
submission, or long campaign, a repository-owned executable closure gate MUST
validate all transitive inputs and use mutation tests to prove that rejected
inputs cause zero external side effects. Readiness MUST exercise the real path
when local markers cannot prove reachability. Success requires agreement among
the protocol oracle, fresh result artifacts, all child exit statuses, and
bounded cleanup; a marker, response, or benchmark line alone is insufficient.
Only one active subject may exist per candidate and gate, and evidence from
different candidate identities MUST NOT be combined. This principle prevents
expensive infrastructure from becoming an iterative configuration debugger and
makes every promotion decision reproducible.

### VIII. Design-Code Convergence Before Formal Validation

After implementation changes and before any complete unit/integration suite,
MiniNDN campaign, SIF qualification, TigerCluster job, benchmark, or experiment
is run as acceptance evidence, the feature MUST pass a code-aware
design-to-code convergence audit. The audit MUST establish the accepted
specification, plan, contracts, and invariants, then inspect the real production
entry points, callers, runtime wiring, effective configuration, security
boundaries, and evidence-producing paths with CodeGraph plus exact source
verification. It MUST distinguish what is specified, implemented, wired,
executed, and measured; the existence of a helper or isolated test does not
prove that the production path uses it.

Every discrepancy MUST be recorded with severity, source evidence, controlling
requirement, correction, owner task, and closing regression. Any unresolved
semantic, architecture, security, production-wiring, or evidence-validity gap
produces `BLOCK`. The specification and tasks MUST be corrected before code is
repaired when the written plan is incomplete or wrong; otherwise code MUST be
repaired to match the accepted design. Focused failing tests and focused repair
regressions remain allowed and required during convergence. Formal validation
or experiment execution begins only after a fresh audit reports `PASS`, and any
later behavior-affecting source, design, dependency, or configuration change
invalidates that verdict and requires re-audit.

This principle prevents expensive or broad tests from repeatedly measuring the
wrong implementation and prevents passing component tests from hiding a broken
production call path.
The canonical executable checklist is
`.specify/memory/design-code-convergence.md`; feature-specific rules may be
stricter but MUST NOT weaken its PASS/BLOCK boundary.

## Project Constraints

Use resumable workflow tooling for long-running, multi-phase, unclear, or
stateful work: VM setup, benchmark campaigns, major protocol changes,
distributed-inference work, and proposal-wide slide revisions. Preserve enough
state to keep work scoped and recoverable across interrupted sessions.

Keep `results/` as local experiment output, not source of truth. Preserve only
canonical reproduction runs or the latest result for a distinct diagnostic
scenario once the finding is documented.

When README documentation is updated, keep Chinese and English versions in sync
when both exist.

### Document Language Policy

Spec Kit documents for NEW features use a layered bilingual convention:
narrative content (user-story descriptions, revision history, audit findings,
failure diagnoses, design rationale, summaries) is written in Chinese;
structural content that machine gates, templates, and cross-document checks
depend on stays in English: section titles, FR/SC/task item titles and IDs,
status words (`existing`, `planned`, `current`, `PASS`, `BLOCK`,
`WAITING_EXTERNAL_INPUT`), contract JSON, field names, file paths, hashes,
and commands. Commit messages and code comments remain English.

Existing spec directories are NOT retro-translated; migrating them would
disturb frozen evidence and worsen cross-document comparison (audit principle
4). New specs may start under this policy from the first feature created after
version 1.4.0. When a document section must be machine-parsed by a repository
gate script, its English markers must remain verbatim; gate scripts MAY add a
Chinese explanation in their error messages but MUST NOT silently accept
translated markers.

## Development Workflow

1. Check the working tree before edits and never revert unrelated user changes.
2. For code work, start with CodeGraph unless the task is a literal text/doc
   lookup.
3. Use Spec Kit before implementing durable feature or architecture work, and
   verify that its tasks are cohesive behavioral units rather than mechanically
   split file or process steps. For expensive execution, also verify the
   immutable candidate, invalidation matrix, executable closure gate, and
   no-side-effect mutation coverage before implementation is promoted.
4. After implementation and before formal validation, run the post-
   implementation design-code convergence audit, repair every controlling gap,
   add its focused regression, and require a fresh `PASS`. Focused development
   tests are allowed during repair; broad qualification and experiments are not.
5. Use GSD for multi-phase work that needs explicit state, verification, or
   recovery.
6. Prefer MiniNDN for final NDNSF network/security/performance validation.
7. After completion, summarize changed files, verification, residual risk, and
   the next best step.
8. Play the 1-second completion bell after success or failure.

## Governance

This constitution is the tracked project authority for development workflow,
security invariants, and required validation gates. Local development
instructions must not override NDNSF runtime/security rules. Amend this
constitution when those project rules change.

**Version**: 1.4.0 | **Ratified**: 2026-06-24 | **Last Amended**: 2026-09-05
