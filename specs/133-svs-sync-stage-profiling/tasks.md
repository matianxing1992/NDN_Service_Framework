# Tasks: Synchronous NDN-SVS Stage Profiling

**Input**: Design documents from `specs/133-svs-sync-stage-profiling/`

**Prerequisites**: `spec.md`, `plan.md`, `research.md`, `data-model.md`,
`contracts/stage-measurement-contract.md`, `traceability.md`, and `quickstart.md`

**Execution boundary**: These tasks implement and validate only the exact
synchronous pre-feature subject. They do not execute Spec 132, do not profile
the async/latest subject, and do not modify frozen Spec 131/132 evidence.

## Phase 1: Freeze The Profiling Contract

- [x] T001 Audit the Spec 133 intent, historical call graph, stage registry, instrumentation necessity, perturbation gate, patch ownership, rollback, and evidence boundary; resolve every BLOCK before source work and record the reviewed verdict in `specs/133-svs-sync-stage-profiling/checklists/pre-implementation-audit.md`, `specs/133-svs-sync-stage-profiling/spec.md`, `specs/133-svs-sync-stage-profiling/plan.md`, and `specs/133-svs-sync-stage-profiling/contracts/stage-measurement-contract.md`.

**Checkpoint**: The exact subject, five-cell limit, stage semantics, CPU/wait
separation, and admissibility rules are frozen before instrumentation begins.

---

## Phase 2: Establish The Immutable Subject Foundation

- [x] T002 Create the two-mode Spec 133 subject builder and fail-first contract tests: `prepare` must create a dedicated clean worktree from exact base `a9944019f76791773604999f00128057b9534ace`, apply only the canonical Boost 1.71 patch, build the compression-disabled clean library with `-j2`, create the profiling worktree at that clean head, reject active/frozen-worktree movement or undeclared hunks, and emit `build/spec133/subject-foundation.json`; `finalize` must be defined to reject a missing/unreviewed profiling patch or shared driver and is executed only by T008 after T003--T006 in `Experiments/build_svs_sync_stage_profile.py` and `tests/python/test_spec133_svs_sync_stage_profile.py`.

**Checkpoint**: The subject can be rebuilt without touching the active
NDN-SVS checkout or Specs 131/132, and its only behavioral delta is the
reviewed profiling patch.

---

## Phase 3: User Story 1 - Observe Every Expensive PubSub Stage (Priority: P1) MVP

**Goal**: Produce low-overhead, structured, attributable spans and exact
counters for the complete synchronous PubSub path.

**Independent Test**: A local two-peer smoke trace passes schema, sampling,
parent/child, call-count, and residual checks and exercises the piggyback path;
fixtures exercise Mapping and Payload fallback paths.

- [x] T003 [US1] Implement the diagnostics-only profiler with `CLOCK_MONOTONIC_RAW`, fixed stage registry, non-throwing RAII timers, exact per-stage aggregate counters, leaf/aggregate/lock/queue/external semantics, exact/ambiguous/censored correlation, deterministic trace-key sampling that keeps sampled children together, dedicated `ndn_svs.Profile` structured `NDN_LOG` records, process-end summary flush, and focused fail-first unit/contract coverage in `build/spec133/worktrees/sync-stage-profile/ndn-svs/profile.hpp`, `build/spec133/worktrees/sync-stage-profile/ndn-svs/profile.cpp`, `build/spec133/worktrees/sync-stage-profile/wscript`, and `tests/python/test_spec133_svs_sync_stage_profile.py`.
- [x] T004 [US1] Instrument the synchronous publisher and Sync-production critical paths without changing protocol decisions: inner/outer build, sign, encode, store, Face put, timestamp/Mapping insert, local state update, explicit mutex wait versus protected work, timer arm/cancel/lateness/coalescing, repeated Mapping candidate encode, piggyback packing, version-vector/ApplicationParameters encode, Interest build/sign/express, and aggregate residuals; add focused historical unit/smoke gates in `build/spec133/worktrees/sync-stage-profile/ndn-svs/svspubsub.cpp`, `build/spec133/worktrees/sync-stage-profile/ndn-svs/svsync-base.cpp`, `build/spec133/worktrees/sync-stage-profile/ndn-svs/core.cpp`, `build/spec133/worktrees/sync-stage-profile/ndn-svs/version-vector.cpp`, `build/spec133/worktrees/sync-stage-profile/ndn-svs/store-memory.hpp`, and `tests/python/test_spec133_svs_sync_stage_profile.py`.
- [x] T005 [US1] Instrument Sync receive, Mapping resolution, Mapping fallback, Payload fallback, validation/decode/cache, and subscription delivery with distinct CPU, lock-wait, queue-wait, external-wait, aggregate, and milestone records; preserve historical unsynchronized-container behavior, count piggyback/fallback/Nack/timeout/validation outcomes exactly, reject ambiguous cross-peer waits from critical-path claims, and close focused path fixtures in `build/spec133/worktrees/sync-stage-profile/ndn-svs/core.cpp`, `build/spec133/worktrees/sync-stage-profile/ndn-svs/svspubsub.cpp`, `build/spec133/worktrees/sync-stage-profile/ndn-svs/mapping-provider.cpp`, `build/spec133/worktrees/sync-stage-profile/ndn-svs/fetcher.hpp`, `build/spec133/worktrees/sync-stage-profile/ndn-svs/fetcher.cpp`, `build/spec133/worktrees/sync-stage-profile/ndn-svs/store-memory.hpp`, and `tests/python/test_spec133_svs_sync_stage_profile.py`.
- [x] T006 [US1] Replace the invalid cross-thread driver with a two-node/two-process driver in which each process uses one Face/io_context thread for the absolute-deadline application timer, synchronous `publish()`, Sync/fetch processing, and subscription callbacks; record skipped releases, lateness, state, and delivery events; prohibit catch-up bursts and every second-thread NDN-SVS call; then prove complete piggyback plus synthetic fallback trace accounting in `Experiments/ndn-svs-pubsub-benchmark/svs-sync-stage-profile.cpp` and `tests/python/test_spec133_svs_sync_stage_profile.py`.

**Checkpoint**: User Story 1 is independently demonstrable before any formal
campaign: every registered stage is measurable or explicitly zero/skipped, and
nested spans reconcile without double counting.

---

## Phase 4: User Story 2 - Measure The Five Synchronous Rate Boundaries (Priority: P1)

**Goal**: Admit the profiler only when perturbation is bounded, then preserve
exactly five once-only bidirectional 60-second cells.

**Independent Test**: A temporary manifest fixture contains exactly five
ascending rates and rejects duplicate attempts, changed logging, failed
overhead admission, stale hashes, async/worker settings, or reused paths.

- [x] T007 [P] [US2] Update the MiniNDN runner and source-contract tests to require two distinct MiniNDN nodes and processes, one Face/io_context execution thread per peer, the fixed clean-control/profiled-disabled/profiled-enabled 1000 pps short admission triplet, FR-009 A-vs-B/B-vs-C/A-vs-C receipt, exact five-cell manifest/seal gate, 10/60/10 timing, fixed topology/payload/security/compression/CPU/log settings, process/resource/log capture, immutable subject-versus-infrastructure terminal receipts, and no formal retry in `Experiments/NDN_SVS_Sync_Stage_Profile_Minindn.py` and `tests/python/test_spec133_svs_sync_stage_profile.py`.
- [x] T008 [US2] Freeze the corrected T006 driver identity, rebuild/self-test it against clean and profiled libraries, create a new immutable subject manifest without overwriting the earlier one, run one fresh three-arm single-I/O-thread overhead preflight, validate hashes/schema/performance bounds, and either seal one fresh five-cell manifest or stop without consuming a formal cell; preserve the earlier cross-thread preflight as ineligible harness evidence and write the new gate evidence under a new path. The original derived receipt remains `REJECTED`; `overhead-receipt-corrected.json` reuses the same raw run, fixes the per-peer CPU-sample endpoint bug, binds the original hash, records `networkRerun=false`, and is `ADMITTED`.
- [x] T009 [US2] Execute the sealed 200, 400, 600, 800, and 1000 publications/s-per-peer MiniNDN cells once in ascending order, monitor without modifying the command, retain every successful/negative/invalid outcome, and close with exactly five terminal receipts plus raw event/profile/resource logs in `results/spec133-svs-sync-stage-profiling/spec133-confirm-io01-20260723T040005Z/`. All five cells are `COMPLETE`; no retry or replacement was performed.

**Checkpoint**: Exactly five formal outcomes exist. No missing, failed, or slow
cell has been selectively replaced.

---

## Phase 5: User Story 3 - Rank Bottlenecks Without Conflating CPU And Waiting (Priority: P2)

**Goal**: Convert raw spans/counters/events into reviewer-facing attribution
and an evidence-bounded bottleneck conclusion.

**Independent Test**: Synthetic fixtures with nested spans, residuals, invalid
records, piggy/fallback branches, and known rate boundaries produce exact
counts/distributions/demand shares and the expected supported/candidate/
inconclusive verdicts.

- [x] T010 [P] [US3] Implement the strict analyzer and fail-first fixtures for subject/manifest/receipt/log validation, exact-summary reconciliation, nearest-rank distributions, interval-union residuals, leaf-only thread demand, wait separation, per-direction rate/delivery metrics, piggyback/fallback ratios, two-signal bottleneck ranking, and all required JSON/CSV/Markdown outputs in `Experiments/analyze_svs_sync_stage_profile.py` and `tests/python/test_spec133_svs_sync_stage_profile.py`.
- [x] T011 [US3] Analyze all five immutable receipts without omission, generate the rate-by-stage, grouped critical-path, path-frequency, and ranked bottleneck tables, document logging overhead and one-run-per-rate limitations, trace every claim to raw evidence, run the post-implementation audit, and close Spec 133 in `results/spec133-svs-sync-stage-profiling/spec133-confirm-io01-20260723T040005Z/`, `specs/133-svs-sync-stage-profiling/evidence/bottleneck-report.md`, and `specs/133-svs-sync-stage-profiling/checklists/post-implementation-audit.md`.

**Checkpoint**: The teacher can see which application-publication stage,
remaining Face/io_context stage, timer delay, queue, or external wait dominates
at each rate, or an explicit inconclusive verdict when the evidence does not
isolate one.

---

## Dependencies & Execution Order

```text
T001 -> T002 -> T003 -> T004 -> T005 -> T006
T006 -> T007 -> T008 -> T009
T006 -> T010
T009 + T010 -> T011
```

- T001 blocks all code because it freezes subject and attribution semantics.
- T002 blocks instrumentation because all profiling edits belong only in the
  dedicated historical worktree.
- T003--T006 are sequential because they share the profiler and historical
  PubSub call graph.
- T007 and T010 are parallelizable after US1 because runner and analyzer own
  different files and can use frozen fixtures.
- T008 blocks formal sealing; T009 is the only task authorized to consume the
  five formal cells.
- T011 requires both the real receipts and the already validated analyzer.

## Parallel Example

After T006 closes, two independent implementation tracks may proceed:

```text
Track A: T007 runner and admission/manifest contracts
Track B: T010 analyzer, accounting, and bottleneck fixtures
```

Formal execution remains serialized: `T008 -> T009 -> T011`.

## Implementation Strategy

### MVP First

Complete T001--T006 and stop. This produces an independently reviewable
two-peer stage trace and proves that every expensive historical path has a
defined measurement without consuming formal evidence.

### Controlled Formal Closure

1. Close runner and analyzer contracts in parallel.
2. Build once and pass the fixed overhead gate.
3. Seal exactly five cells.
4. Execute once in ascending rate order.
5. Analyze all five and close the audit without result-driven reruns.

## Cohesion Review

- Profiler primitives, their test-first contract, and summary flush are one
  behavior (T003), not separate file/edit/test tasks.
- Each historical path task owns its instrumentation plus focused evidence
  (T004/T005), rather than one task per span.
- Runner implementation and real campaign execution are separate because the
  latter irreversibly consumes formal cells (T007 versus T008/T009).
- Analyzer implementation is independent of campaign execution through fixed
  fixtures (T010); final evidence synthesis requires real receipts (T011).
- Total tasks: 11. No task was added merely to increase detail.
