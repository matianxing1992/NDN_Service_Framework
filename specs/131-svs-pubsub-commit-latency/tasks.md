# Tasks: NDN-SVS PubSub Commit-Latency Comparison

**Input**: `spec.md`, `plan.md`, `research.md`, `data-model.md`,
`contracts/measurement-contract.md`, and `quickstart.md`

**Scope**: Pure NDN-SVS PubSub in MiniNDN. The formal campaign contains exactly
five baseline cells followed by five latest cells. Formal cells have one
attempt, no retry, no selective replacement, and no NDNSF runtime dependency.

## Phase 1: Correct Harness and Freeze Subjects

- [x] T001 [US1] Rebuild the two independent pinned NDN-SVS subjects from
  `a9944019f76791773604999f00128057b9534ace` and
  `6bb34545b4f89f1f6c265a68c18f1a40ade413eb` with the identical, sole Boost
  1.71 `wscript` patch. Record source/tree/patch/library/binary hashes, clean
  worktrees, Boost 1.71 linkage, and NDNSF-free process/link manifests; run both
  C++ self-tests. (FR-001-FR-006, FR-022; SC-001, SC-009)

- [x] T002 [US1] Implement and test the corrected publisher: an independent
  Face event-loop thread and independent high-resolution pacer thread; direct
  calls to the common thread-safe publication adapter; Face-side execution of
  `publish()`/`publishAsync()`; absolute pacing with a two-period skip bound and
  no unbounded debt-repayment loop;
  no Face scheduler offered-load callbacks; separate scheduled, attempted,
  API-completed, delivered, attempted/scheduled, and delivered/attempted fields. Preserve
  logical-ID joins, latency, payload, worker, CPU/RSS, and network evidence.
  (FR-008-FR-016; SC-003-SC-005)

- [x] T003 [US1] Implement and test the immutable planner/analyzer for exactly
  10 unique cells: baseline ordinals 1-5 at 200/400/600/800/1000, then latest
  ordinals 6-10 at the same rates, repetition fixed to one, one receipt per
  cell, direct descriptive differences only, and no bootstrap/p-value claim.
  Reject cardinality, order, rate, identity, retry, and accounting violations.
  (FR-007, FR-017-FR-020; SC-002, SC-004-SC-009)

## Phase 2: Preflight and Formal Admission

- [x] T004 [US1] Run the complete focused Python suite, both C++ self-tests,
  dependency/clock/CPU/storage/single-writer checks, then one non-formal 1000
  pps MiniNDN smoke for each subject. Require attempted rate within ±2% for
  both. Only after both pass, create and seal a fresh exactly-10-cell manifest,
  record all identities and admission evidence in `evidence/preflight.md`.
  (FR-002-FR-008, FR-017-FR-018, FR-021-FR-022; SC-001-SC-002, SC-008-SC-009)

## Phase 3: Formal Baseline

- [x] T005 [US2] Execute sealed ordinals 1-5 exactly once with
  `baseline-sync-serial`. Preserve all outcomes, require one receipt at every
  rate, exact scheduled/attempted/delivered accounting, unchanged identities,
  zero treatment starts, and terminal `BASELINE_COMPLETE` or honest
  `INCOMPLETE`; record `evidence/baseline-block.md`. (FR-004, FR-007-FR-019;
  SC-002-SC-005, SC-008)

## Phase 4: Formal Latest

- [x] T006 [US3] Only after T005 closes, execute sealed ordinals 6-10 exactly
  once with `latest-async-parallel`. Preserve all outcomes and worker counters,
  require one matched receipt at every rate, no retry, unchanged identities,
  exact accounting, and terminal `COMPLETE` or honest `INCOMPLETE`; record
  `evidence/treatment-block.md`. (FR-004-FR-020; SC-002-SC-005, SC-008-SC-009)

## Phase 5: Comparison and Closure

- [x] T007 [US3] Generate the per-cell and per-rate comparison table showing
  scheduled/attempted/delivered, attempted/scheduled, delivered/attempted,
  attempted rate, delivered and deadline-capped latency, state-update/API
  timing, resources/workers, direct p95 delta, and highest sustained rate. Run
  strict Spec Kit and code/evidence audits, retain negative or inconclusive
  results, and close only the pinned version-bundle claim in
  `evidence/final-comparison.md`, `evidence/final-audit.md`, and
  `completion-summary.md`. (FR-012-FR-022; SC-003-SC-009)

## Dependencies

```text
T001 -> T002 -> T003 -> T004 -> T005 -> T006 -> T007
```

The formal path is sequential. The old five cells finish before any latest
cell begins. Any formal failure remains evidence; continuing after a repair
requires a new campaign ID and a complete fresh 10-cell run.

## Completion Definition

- T001-T007 are `[x]`.
- The formal campaign has 10/10 unique once-only receipts, or closure is
  explicitly `INCOMPLETE` without a comparison claim.
- Both 1000 pps admission smokes meet attempted-rate ±2% before sealing.
- Exact commits, hashes, Boost 1.71 linkage, and NDNSF exclusion pass.
- All five rates have one direct old/latest comparison.
- Negative delivery, rate, queue, and latency results remain visible.
- The report says version bundle and direct observation, not component-only
  causality or replication-based statistical confidence.
