# Feature Specification: Single-Worker Necessity Confirmation

**Feature Branch**: `138-svs-worker-necessity`

**Created**: 2026-07-23

**Status**: Draft

**Input**: User description: "Use the simplest proof: on the same latest
NDN-SVS source and the same binary, compare serial production inside the Face
thread with serial production on one worker, and prove why worker/asynchronous
offload is needed."

## Evidence Boundary

- Spec 137 is frozen negative/inadmissible baseline evidence. Its six receipts,
  binaries, reports, and result directories MUST NOT be modified, rerun, or
  replaced.
- The source subject is the current clean NDN-SVS commit
  `6bb34545b4f89f1f6c265a68c18f1a40ade413eb`.
- Both arms MUST execute the exact same binary. The only behavioral treatment
  is whether serial Sync Interest production executes inline on the Face
  thread or is asynchronously queued to exactly one serial worker.
- `publishAsync()` remains common to both arms. This feature therefore evaluates
  the combined **asynchronous single-worker Sync-production offload** mechanism;
  it does not claim a separately identified publication-API asynchronous effect.
- No multi-worker, old/new commit, RSA, UAV, codec, NDNSF, or service-framework
  comparison is in scope.

## User Scenarios & Testing

### User Story 1 - Establish A Clean Two-Mode Contrast (Priority: P1)

As an NDN-SVS maintainer, I need a reviewer-verifiable experiment in which one
runtime switch changes only the location of serial Sync production.

**Why this priority**: Without the same-source, same-binary, single-variable
contrast, a performance difference cannot be attributed to worker offload.

**Independent Test**: Verify the subject hash and expanded configurations for
both modes, then confirm that only the registered production-location fields
differ.

**Acceptance Scenarios**:

1. **Given** `face-serial`, **when** Sync production runs, **then** snapshot,
   extension construction, encoding, signing, and transmission occur serially
   from the Face path.
2. **Given** `worker-serial`, **when** Sync production runs, **then** the Face
   admits work, exactly one worker performs the serial build/encode/sign chain,
   and the Face finalizes transmission.
3. **Given** either mode, **when** more than one signer is active, fallback
   occurs, work is unaccounted, or another controlled setting differs, **then**
   the cell is inadmissible and retained.

---

### User Story 2 - Select A Real Pressure Point Without Outcome Shopping (Priority: P1)

As a researcher, I need a rate that stresses Face-inline production while the
common publisher and network path remain valid, without selecting the rate
because the worker happened to look better.

**Why this priority**: Spec 137 used 60 pps, where the Face had little sustained
pressure, and its first pair was contaminated by host starvation.

**Independent Test**: Execute the preregistered control-only rate ladder in
descending order after a host-quiescence gate. Select the first rate that
achieves offered-load and delivery validity and crosses the registered Face
pressure threshold; then run one worker qualification cell at that same rate.

**Acceptance Scenarios**:

1. **Given** the fixed ladder `1000, 800, 600, 400, 200`, **when** calibration
   begins, **then** rates are considered only in that order and no unregistered
   rate is introduced.
2. **Given** a control cell, **when** offered load, delivery, accounting, or
   host-quiescence fails, **then** it is preserved as calibration evidence and
   the next lower registered rate may be considered.
3. **Given** a selected control rate, **when** the single worker qualification
   cannot sustain the same rate and invariants, **then** formal execution is
   blocked rather than tuning the worker.

---

### User Story 3 - Decide Whether Offload Is Necessary At The Tested Boundary (Priority: P2)

As an NDN-SVS maintainer, I need run-level evidence showing whether the worker
protects the Face event loop or user-visible delivery at a load where inline
production is actually under pressure.

**Why this priority**: Moving CPU work is useful, but “necessary” requires a
repeatable correctness, responsiveness, or capacity benefit under a defined
boundary.

**Independent Test**: Run three paired repetitions in the sealed AB/BA/AB
order, with 10 seconds warmup, 60 seconds measurement, and 10 seconds drain,
then apply the preregistered decision rule to all six receipts.

**Acceptance Scenarios**:

1. **Given** six admissible cells, **when** paired results are calculated,
   **then** each run—not each packet—is one experimental replicate.
2. **Given** one-worker mode, **when** results close, **then** Face production
   CPU, Face heartbeat p99, offered rate, delivery ratio, delivery p99, queue
   wait, fallback, and traffic are all reported.
3. **Given** the worker does not produce the registered repeatable benefit,
   **when** the campaign closes, **then** the conclusion states that necessity
   was not demonstrated at the tested boundary.

### Edge Cases

- A quiescence gate passes but external CPU contention begins during a cell.
  Offered-load and completeness gates still fail closed; the cell is retained
  and is not replaced. The report does not invent a causal host diagnosis when
  the recorded data cannot distinguish it from subject overload.
- The control never reaches a registered pressure point. The result is
  `NO_TESTABLE_PRESSURE_POINT`, not evidence for or against worker necessity.
- The worker queue grows, falls back to Face, drops stale work, or is not
  drained. The result is a trade-off or inadmissible, not a win.
- Both modes sustain the boundary with indistinguishable Face responsiveness
  and delivery. The result is `NOT_NECESSARY_AT_TESTED_BOUNDARY`.
- The worker improves Face responsiveness but harms delivery by more than the
  registered bound. The result is `TRADE_OFF`.

## Requirements

### Functional Requirements

- **FR-001**: The confirmation MUST preserve all Spec 137 source and evidence
  bytes and use a new Spec 138 result directory.
- **FR-002**: Both modes MUST use source commit `6bb34545...`, one measurement
  patch, one Boost-1.71 build, and one identical binary hash.
- **FR-003**: The only modes MUST be `face-serial` and `worker-serial`;
  worker mode MUST have exactly one production worker and both modes MUST have
  one Face thread, zero receive workers, and serial signing.
- **FR-004**: Both modes MUST use identical `publishAsync()` publication path,
  topology, payload, protocol, security, timing, logging, CPU placement, and
  receiver behavior.
- **FR-005**: The experiment MUST use one active publisher and one fixed
  receiver in separate processes on two MiniNDN nodes.
- **FR-006**: Before every calibration, qualification, or formal cell starts,
  the runner MUST record a bounded per-core host-quiescence observation. A cell
  MUST NOT start until the registered gate passes; a gate timeout is terminal
  infrastructure evidence and is not a subject retry.
- **FR-007**: Rate calibration MUST consider only `1000, 800, 600, 400, 200`
  pps in descending order and MUST use `face-serial` only.
- **FR-008**: A calibration rate is eligible only when attempted rate is within
  +/-2%, remote delivery ratio is at least 99%, fallback and unexplained
  accounting are zero, maximum active signers is one, and the Face pressure
  gate is met.
- **FR-009**: The Face pressure gate is met when aggregate Face production CPU
  consumes at least 10% of one CPU over the active warmup-plus-measure
  publication interval or Face heartbeat p99 is at least 2 ms.
- **FR-010**: After selecting the highest eligible control rate, one non-formal
  `worker-serial` qualification MUST pass the same offered-load, delivery,
  seriality, fallback, accounting, shutdown, and host-contamination gates.
- **FR-011**: Formal execution MUST seal exactly six cells in AB/BA/AB order,
  three per mode, with 10/60/10 timing and no automatic or selective retry.
- **FR-012**: The analyzer MUST verify raw artifact hashes and emit run,
  paired, production-stage, traffic, host-load, and conclusion artifacts.
- **FR-013**: Formal analysis MUST use runs as replicates and report all
  outcomes, including inadmissible and unfavorable cells.
- **FR-014**: `NECESSARY_AT_TESTED_BOUNDARY` requires all six cells admissible,
  at least 50% Face production CPU relief in at least two pairs, at least 20%
  Face-heartbeat-p99 improvement in at least two pairs, no delivery-ratio harm
  above one percentage point in any pair, no delivery-p99 harm above 10% in
  two or more pairs, one signer, zero fallback, and drained queues.
- **FR-015**: When FR-014 does not hold, the analyzer MUST select one of
  `FACE_RELIEF_ONLY`, `NOT_NECESSARY_AT_TESTED_BOUNDARY`, `TRADE_OFF`,
  `WORKER_WORSE`, `NO_TESTABLE_PRESSURE_POINT`, or `INADMISSIBLE`.
- **FR-016**: The report MUST state that the result applies to asynchronous
  single-worker Sync-production offload at the selected workload and does not
  separately identify `publishAsync()`, RSA, multi-worker, or cross-version
  effects.

### Key Entities

- **FrozenSubject**: Source, patch, library, binary, linkage, and hash identity
  shared by both modes.
- **CalibrationCell**: A control-only load-validity and Face-pressure
  observation at one registered rate.
- **QualificationCell**: The one-worker validation at the selected rate.
- **FormalCell**: One immutable treatment execution and receipt.
- **HostQuiescenceRecord**: Per-core background utilization and gate decision
  captured before cell start.
- **PairedContrast**: One Face/worker run-level comparison.
- **NecessityDecision**: The deterministic terminal classification.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Every admitted comparison references one source commit and one
  identical binary hash, with only the registered runtime treatment fields
  differing.
- **SC-002**: All admitted worker cells prove exactly one active signer, one
  production worker, zero fallback, zero unexplained work, and drained
  shutdown.
- **SC-003**: Calibration either selects one preregistered pressure rate or
  terminates with `NO_TESTABLE_PRESSURE_POINT`; no outcome-driven rate is used.
- **SC-004**: The formal campaign contains exactly six once-only terminal
  receipts and a reproducible offline analysis.
- **SC-005**: The conclusion reports whether the registered necessity rule is
  met and includes every run-level Face CPU, heartbeat, delivery, queue, and
  traffic result.

## Assumptions

- Four logical CPUs remain available; they are capacity for the whole
  experiment, not four CPUs assigned to the single worker.
- The frozen Spec 137 binary remains bit-identical and its linked library and
  Boost 1.71 dependencies remain available.
- Host quiescence may delay a cell start but may not cause a started cell to be
  silently retried.
