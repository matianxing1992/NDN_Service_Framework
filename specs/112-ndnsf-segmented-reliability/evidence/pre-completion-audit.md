# Spec 112 Pre-Completion Audit

**Date**: 2026-07-15  
**Mode**: post-implementation, code-aware  
**Verdict**: `BLOCK`

The executed MiniNDN, unit, Python, and lifecycle evidence satisfies the stated
acceptance counts, but one late-callback lifetime gap remains in the synchronous
Python Targeted adapter. Completion is blocked until that adapter owns its wait
state independently of the Python call stack and a new integrated candidate is
sealed and validated.

## Deterministic Gates

| Gate | Result | Evidence |
|---|---|---|
| Strict structure | PASS | 20 FR, 9 SC, 4 user stories, 47 unique tasks, 44 complete before this audit, all 20 FR traced |
| Prerequisites | PASS | `check-prerequisites.sh --json --require-tasks --include-tasks` resolved this feature and all required artifacts |
| Diff hygiene | PASS | `git diff --check` passed in the root, `../ndn-svs`, and `../NAC-ABE` repositories |
| CodeGraph | PASS | index synchronized over 11 changed files; current Targeted request, timer, cleanup, and wrapper call paths inspected |
| Scope search | PASS | no DI, Docker, iTiger, UAV, 5% loss, checked-publish API, tokens-off API, or Direct-path mechanism was added by the Spec 112 source/test set |
| Evidence schema | PASS | six unique final cells, four boundary cells 24/24, same-epoch burst 102/102, timeout 1060.256/1500 ms with one terminal callback, lifecycle 300/300 |
| GSD health | CONDITIONAL | no errors; one unrelated stale Spec 111 worktree warning was preserved and not deleted |

The first ad-hoc aggregate validator failed because it treated the lifecycle
schema's integer `roleExitCount` as a per-role mapping. Inspection of the
immutable artifact showed `roleExitCount=300`; the corrected read-only validator
counted the 300 records and confirmed 100 Controller, 100 Provider, and 100 User
exits. No experiment cell or result file was rerun or modified.

## Findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| PC-001 | HIGH | Code reality / terminal lifetime | `pythonWrapper/src/ndnsf/_ndnsf.cpp:2514-2579` | `requestServiceTargeted()` posts a lambda and registers native callbacks that capture stack-owned `output`, `mutex`, `cv`, and `done` by reference. The local `timeout_ms + 500 ms` fallback can return while the event loop remains alive; a later posted operation or native timeout/response can then access destroyed storage. Existing degraded-Provider evidence proves the normal bounded case, but not an event-loop stall beyond the local fallback. | Replace the stack-reference captures with shared terminal state and copied submission inputs, arbitrate the local fallback and native callbacks once, rebuild the binding, and seal/validate a new candidate because the current final candidate predates this correction. |

## Traceability Gaps

| Source | Missing link | Impact |
|---|---|---|
| FR-013, T030, `contracts/request-terminal-outcome.md` | No proof that a callback arriving after the synchronous adapter's local fallback cannot touch returned stack state | Potential use-after-return in the exact degraded/stalled condition the timeout correction must bound |

No other FR, SC, or task lacked an implementation/test/evidence link. The
reported old aarch64/NixOS/Wi-Fi environment remains an explicit external
evidence limitation rather than a local completion claim.

## Readiness Scorecard

| Dimension | Ready? | Notes |
|---|---|---|
| Intent and five-defect scope | Yes | no new mechanism outside the email report |
| Architecture and ownership | Yes | segmentation remains in ndn-svs; deadline in ServiceUser; wrapper adaptation in pybind; OpenABE ownership in NAC-ABE |
| Security/correctness | Conditional | tokens, permissions, replay, and routing pass; PC-001 blocks terminal lifetime safety |
| Task executability | Yes | all existing tasks are concrete and ordered |
| Validation/evidence | Conditional | immutable evidence passes, but a post-candidate wrapper correction requires a new candidate |
| Migration/rollback | Yes | internal changes preserve public APIs and are independently revertible |
| Code reality | No | PC-001 remains in current source |

## Metrics

- User stories: 4
- Functional requirements: 20, all mapped
- Success criteria: 9, all mapped
- Tasks before convergence: 47, 44 complete
- Unmapped tasks: 0
- Placeholders: 0
- Findings: 0 Critical / 1 High / 0 Medium / 0 Low

## Evidence Limits

- Local x86-64 Ubuntu/MiniNDN only; the reporter's old aarch64/NixOS/Wi-Fi
  combination was not rebuilt.
- The 300 lifecycle exits were not ASan/UBSan-instrumented.
- The root worktree contains unrelated Spec 110/111/user changes; candidate
  manifests bind them, while this audit attributes only the task-listed paths
  to Spec 112.

## Required Next Actions

1. Run Spec Kit convergence and append the PC-001 correction as traceable work.
2. Implement and rebuild the shared synchronous Targeted terminal state.
3. Seal a new candidate and repeat the declared final cells exactly once under
   that new identity; do not overwrite the prior successful candidate.
4. Update closeout evidence and then run the final audit.
