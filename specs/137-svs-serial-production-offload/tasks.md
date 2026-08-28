# Tasks: Serial Sync-Production Offload Proof

**Input**: Design documents in
`specs/137-svs-serial-production-offload/`

**Execution boundary**: This task list implements and measures only Spec 137.
Spec 133, Spec 135, and Spec 136 remain immutable and must not be invoked as
campaigns.

## Phase 1: Freeze Design And Implementation Gate

- [x] T001 Audit intent fidelity, exact NDN-SVS source reality, same-binary
  treatment isolation, Sync-production versus publication-signing ownership,
  one-signer/fallback/conservation invariants, MiniNDN authority, pilot
  selection, six-cell irreversibility, Boost-1.71 provenance, protected prior
  Specs, rollback, and task cohesion across `spec.md`, `plan.md`, `research.md`,
  `data-model.md`, `contracts/experiment-contract.md`, `quickstart.md`, and
  `traceability.md`; record a pre-implementation PASS with no unresolved
  HIGH/CRITICAL finding before T002 starts.

**Checkpoint**: The causal claim and evidence contract are frozen. A BLOCK
verdict stops implementation.

---

## Phase 2: Prove The Same-Binary Runtime Treatment

**Goal**: One common source patch and one binary implement both modes while
directly proving stage ownership, serial signing, and complete work accounting.

**Independent Test**: Run fail-first treatment/lifecycle tests, then start the
same test binary in both modes and verify the expanded runtime diff contains
only the registered treatment fields; worker mode reports one signer and zero
fallback under bounded load.

- [x] T002 [US1] In the isolated Spec 137 NDN-SVS worktree, first add
  failing treatment, thread-owner, active-signer, counter-conservation,
  queue-full visibility, stale/failure, and sanitizer-backed shutdown/drain
  cases; then implement the common runtime/profiler patch that measures
  snapshot, queue, extension, encode, Sync sign, Face finalization, thread CPU,
  heartbeat, and terminal outcomes without changing default behavior. Add the
  standalone two-mode benchmark and close focused NDN-SVS/unit/compatibility
  gates in
  `Experiments/ndn-svs-pubsub-benchmark/spec137-measurement.patch`,
  `Experiments/ndn-svs-pubsub-benchmark/svs-serial-production-offload.cpp`,
  and `tests/python/test_spec137_svs_serial_production_offload.py`.

**Checkpoint**: The only treatment delta is runtime execution location;
`max_active_sync_signers=1` is observable; all fallbacks and missing work are
fail-closed.

---

## Phase 3: Build And Admit One Immutable Subject

**Goal**: Build once, execute two real MiniNDN processes symmetrically, and
derive all metrics through one fail-closed evidence pipeline.

**Independent Test**: Create one local build and run the no-op, instrumentation,
configuration-diff, route, two-process, conservation, one-signer, fallback, and
shutdown preflights without creating a formal receipt.

- [x] T003 [US2] Implement one hash-bound builder, symmetric
  two-peer MiniNDN runner, structured event/resource schemas, deterministic
  rate selector, immutable receipt ledger, and offline analyzer. The builder
  must create one exact-commit Boost-1.71 worktree/binary, reject additional
  hunks or Boost 1.74 linkage, and preserve the active NDN-SVS checkout. The
  runner must enforce one Face thread/no receive workers, CPU placement,
  10/60/10 formal timing, distinct lifecycle counts, single-writer ownership,
  and once-only ordinals. Close parser/admission/tamper/error-path tests and the
  two-process MiniNDN smoke in
  `Experiments/build_svs_serial_production_offload.py`,
  `Experiments/NDN_SVS_Serial_Production_Offload_Minindn.py`,
  `Experiments/analyze_svs_serial_production_offload.py`,
  `tests/python/test_spec137_svs_serial_production_offload.py`, and
  `build/spec137-four-core/`.

  **Current execution status**: the earlier eight-exclusive-CPU admission rule
  was rejected as an experiment-design error. The corrected four-core layout
  uses one active publisher, one fixed receiver, and exactly one publisher
  worker. A fresh T003 preflight is required; old failed preflights remain
  immutable and are not evidence.

**Checkpoint**: A single frozen binary is admitted, the preflights pass, and no
formal cell has started.

---

## Phase 4: Select And Seal The Stress Rate

**Goal**: Validate the pre-registered 60 pps four-core workload once, then
freeze the exact six-cell campaign before seeing formal outcomes.

**Independent Test**: Execute the fixed 60 pps face/worker diagnostic pair
once and verify the sealed manifest cannot accept an operator rate, changed
binary, or seventh cell.

- [x] T004 [US3] Run the declared `60` pps non-formal face/worker pair with
  5/15/5 timing and no retry; preserve both diagnostics. Admit the fixed rate
  without requiring a favorable result, or close as
  `FIXED_RATE_INADMISSIBLE`. If admissible, seal the exact AB/BA/AB
  six-cell matrix, source/binary/configuration hashes, CPU map, topology,
  thresholds, and analysis rules under
  `results/spec137-svs-serial-production-offload/<campaign-id>/pilot/` and
  `campaign-manifest.json`.

**Checkpoint**: Either the campaign has a cryptographically sealed \(R^\*\) and
six ordinals, or it has a valid terminal no-rate conclusion. Nothing formal has
been selectively sampled.

---

## Phase 5: Execute All Formal Cells Once

**Goal**: Obtain three fresh, paired run-level contrasts without replacement.

**Independent Test**: Verify receipt ordinals 01-06 against the sealed manifest
and confirm each has one attempt, two peer records, raw hashes, and an explicit
terminal/admission status.

- [x] T005 [US3] If T004 sealed a rate, display the full manifest and run
  all six formal MiniNDN cells in ordinal order exactly once. Monitor bounded
  liveness without changing parameters; write one immutable receipt for
  completion, crash, timeout, or inadmissibility; never retry or replace an
  unfavorable cell. Preserve per-peer events/resources, NFD/topology records,
  commands, logs, counters, and hashes under
  `results/spec137-svs-serial-production-offload/<campaign-id>/formal/` and
  `receipts/`.

**Checkpoint**: Exactly six terminal receipts exist, or formal execution ended
at a recorded infrastructure stop condition without rewriting prior receipts.

---

## Phase 6: Analyze, Audit, And Freeze

**Goal**: Decide the narrow proposition from all recorded evidence and preserve
both positive and negative outcomes.

**Independent Test**: A clean offline analyzer invocation reproduces every
run-level metric, paired contrast, admission result, outcome-table branch, and
artifact hash from the closed campaign.

- [ ] T006 [US4] Verify all inputs, emit run metrics, three paired
  contrasts, non-overlapping production stages, Face/worker CPU and heartbeat,
  delivery, queue movement, and Sync/Mapping/Payload/retry/timeout/Nack/fallback
  traffic. Apply the registered taxonomy without override; explicitly separate
  `SUPPORTED`, `FACE_RELIEF_ONLY`, trade-off, negative, and inadmissible
  outcomes; retain all failed cells; state every excluded claim. Write
  `analysis/*.csv`, `analysis/conclusion.json`, and
  `specs/137-svs-serial-production-offload/evidence/offload-proof-report.md`,
  run the post-implementation Spec Kit audit, verify Spec 133/135/136 and the
  active NDN-SVS checkout are unchanged, and freeze Spec 137.

**Checkpoint**: The report is reproducible, scoped to the frozen rate and
HMAC/SHA-256 subject, and Spec 137 is immutable.

## Dependencies And Execution Order

```text
T001 -> T002 -> T003 -> T004 -> T005 -> T006
                         \            /
                          no-rate ----
                              -> T006
```

- T001 blocks all implementation.
- T002 establishes mechanism validity before the harness can claim it.
- T003 must close all preflights before pilot execution.
- T004 may terminate the campaign without T005 when no rate is jointly
  admissible.
- T005 is irreversible and strictly serial.
- T006 consumes all terminal evidence and never repairs it.

## Parallel Opportunities

Formal execution has no parallel tasks. Within T003, offline analyzer unit
fixtures may be developed independently of MiniNDN orchestration after the
event/receipt schemas from T002 are frozen, but they merge under the same
admission checkpoint.

## Cohesion Review

Six tasks correspond to six independently reviewable outcomes: design gate,
mechanism proof, immutable harness, adaptive-but-pre-registered sealing,
irreversible execution, and interpretation/freeze. Each task keeps its
test-first work, implementation, focused validation, and direct evidence
together; no task exists merely to edit one file or run one command.
