# Feature Specification: Fixed-Rate Single-Worker Proof

**Feature Branch**: `139-svs-fixed-worker-proof`

**Created**: 2026-07-23

**Status**: Draft

**Input**: "Prove the simplest same-latest-source, same-binary comparison:
serial production inside Face versus serial production on one worker."

## Evidence Boundary

- Specs 137 and 138 are frozen and MUST NOT be rerun or modified.
- Both arms use the current NDN-SVS commit `6bb34545...` and exact binary
  `c4f3b296...35ac`.
- The fixed rate is 600 pps. It is the next unobserved registered rate below
  Spec 138's 800 pps worker offered-load failure; it was not chosen from a
  favorable worker effect.
- Only `face-serial` and `worker-serial` exist. Worker mode uses one worker.
- `publishAsync()` is common to both modes. The claim is about the combined
  asynchronous single-worker Sync-production offload mechanism.

## User Scenarios & Testing

### User Story 1 - Qualify One Fixed Boundary (Priority: P1)

As a reviewer, I need both modes to sustain the same 600 pps workload before
any causal result is considered.

**Independent Test**: Run one 5/15/5 qualification cell per mode using the same
binary and frozen CPU map.

**Acceptance Scenarios**:

1. **Given** either mode, **when** attempted rate differs by more than 2%,
   delivery is below 99%, seriality/fallback/accounting fails, or Face control
   lacks registered pressure, **then** no formal campaign is sealed.
2. **Given** both modes pass, **when** the manifest is produced, **then** it
   contains one binary hash, one 600 pps rate, and exactly six cells.

### User Story 2 - Test The Two Modes Repeatedly (Priority: P2)

As an NDN-SVS maintainer, I need three paired comparisons to decide whether one
worker materially protects Face responsiveness without delivery harm.

**Independent Test**: Execute AB/BA/AB once each with 10/60/10 timing and apply
the frozen necessity predicate.

**Acceptance Scenarios**:

1. **Given** a started cell, **when** it completes or fails, **then** exactly
   one immutable receipt is retained and no retry occurs.
2. **Given** all six receipts, **when** analysis runs, **then** every run and
   pair appears in the report.
3. **Given** the necessity conjunction fails, **when** closure occurs, **then**
   the report states that worker necessity was not demonstrated.

### Edge Cases

- Qualification fails by a small margin: retain it and stop.
- A formal cell is externally disturbed: offered-load/completeness gates reject
  it without replacement.
- Worker queueing or stale sends rise: report them; do not hide moved work.

## Requirements

### Functional Requirements

- **FR-001**: Preserve Specs 137 and 138 byte-for-byte and use a new result root.
- **FR-002**: Use one verified source commit, library, and binary for both modes.
- **FR-003**: Compare only Face-inline serial production with exactly one
  serial production worker; receive workers remain zero.
- **FR-004**: Keep publication API, topology, payload, protocol, security,
  receiver, timing, logging, and CPU map identical.
- **FR-005**: Qualify both modes once at fixed 600 pps with 5/15/5 timing.
- **FR-006**: Qualification requires attempted-rate error <=2%, delivery >=99%,
  one signer, zero fallback/remainder/owner violations, and drained shutdown.
- **FR-007**: Face qualification additionally requires production CPU >=10% of
  one CPU over warmup-plus-measure or heartbeat p99 >=2 ms.
- **FR-008**: Seal exactly six AB/BA/AB 10/60/10 cells with retry count zero.
- **FR-009**: Record bounded pre-cell host-quiescence evidence.
- **FR-010**: Emit run, pair, stage, traffic, host-load, conclusion, and report
  artifacts from verified raw hashes.
- **FR-011**: `NECESSARY_AT_TESTED_BOUNDARY` requires all six admissible, Face
  CPU relief >=50% and heartbeat-p99 improvement >=20% in at least two pairs,
  delivery-ratio harm <=1 percentage point in every pair, delivery-p99 harm
  >10% in fewer than two pairs, and clean queues/seriality.
- **FR-012**: Otherwise classify `FACE_RELIEF_ONLY`,
  `NOT_NECESSARY_AT_TESTED_BOUNDARY`, `TRADE_OFF`, `WORKER_WORSE`, or
  `INADMISSIBLE`.
- **FR-013**: Exclude separate `publishAsync()`, RSA, multi-worker,
  cross-version, and universal-necessity claims.

### Key Entities

- **FixedQualification**: The two once-only 600 pps qualification cells.
- **FormalReceipt**: One terminal AB/BA/AB cell authority.
- **PairedContrast**: One Face/worker run-level difference.
- **Decision**: The deterministic local necessity classification.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Both qualification cells pass the exact fixed gates or formal
  execution is blocked.
- **SC-002**: Every worker cell proves one worker, one signer, zero fallback,
  zero unexplained work, and drained shutdown.
- **SC-003**: A sealed campaign produces exactly six once-only receipts.
- **SC-004**: Offline analysis reproduces three paired contrasts and one
  deterministic decision.

## Assumptions

- Four logical CPUs remain available for the whole experiment.
- The 600 pps rate remains unobserved before this Spec and retains predicted
  Face pressure from the registered 800 pps control measurement.
