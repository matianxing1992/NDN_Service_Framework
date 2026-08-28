# Feature Specification: SVS Latency Rate Extension

**Feature Branch**: `141-svs-latency-rate-extension`

**Created**: 2026-07-23

**Status**: Frozen

**Input**: "Use the same script configuration to test 600 and 800 pps."

## Evidence Boundary

- Spec 140 is frozen and MUST NOT be modified or rerun.
- Reuse the exact Spec 140 binary and all mode-independent controls.
- Change only the offered rate from 400 to 600 and 800 pps per peer.
- Run exactly one cell per mode and rate; retain failures without retry.

## User Scenarios & Testing

### User Story 1 - Compare Inline And Worker At Higher Rates (Priority: P1)

As an NDN-SVS maintainer, I need the same two-node bidirectional RSA experiment
at 600 and 800 pps to see whether the one-worker advantage persists and where
either mode stops sustaining load.

**Acceptance Scenarios**:

1. **Given** the frozen Spec 140 configuration, **when** Spec 141 starts, **then**
   the only experimental variable added is rate.
2. **Given** 600 and 800 pps, **when** the campaign runs, **then** it executes
   exactly inline then worker at each rate with 10/60/10 timing.
3. **Given** an unsustained or failed cell, **when** it terminates, **then** its
   evidence is retained and no replacement cell is launched.
4. **Given** valid raw samples, **when** analysis runs, **then** it reports
   delivered throughput, delivery ratio, mean, p50, p95, and p99.

## Requirements

- **FR-001**: Preserve Spec 140 results and its binary manifest.
- **FR-002**: Use binary SHA-256
  `a5789d075ec0fbd702add6cc4084bddd0e91cc996b12ca6166e665fd6ec9204a`.
- **FR-003**: Preserve the two-node 100 Mbps, 10 ms one-way, zero configured
  loss topology and bidirectional publish/subscribe traffic.
- **FR-004**: Preserve RSA-2048, 256-byte payload, Fetch window 64, Sync batch
  window 5 ms, CPU affinity 0--3, and independent APP pacer.
- **FR-005**: Preserve `face-inline-rsa` and exactly one `worker-rsa`
  publication worker with the same ordered Face commit path.
- **FR-006**: Use 10 s warmup, 60 s measurement, and 10 s drain for every cell.
- **FR-007**: Execute exactly
  `[600-inline, 600-worker, 800-inline, 800-worker]`.
- **FR-008**: Each peer's attempted rate MUST be within +/-2% of its target for
  the cell to be workload-admissible.
- **FR-009**: Retain raw delivery latency samples and verify sample count and
  mean/p50/p95/p99 against each peer summary.
- **FR-010**: Treat `LOAD_UNSUSTAINED` as a measured outcome, not a harness
  failure, provided offered-load, security, ownership, and evidence checks pass.
- **FR-011**: Do not automatically retry, replace, tune, or selectively omit
  any cell.
- **FR-012**: Produce one table across 400, 600, and 800 pps while labeling the
  400 row as frozen Spec 140 evidence and the new rows as Spec 141 evidence.

## Success Criteria

- **SC-001**: The manifest contains one frozen binary hash and exactly four new
  cells.
- **SC-002**: Every started cell has one terminal receipt and zero retries.
- **SC-003**: Every nonempty delivery population has reproducible mean, p50,
  p95, and p99 from raw samples.
- **SC-004**: The final report preserves negative capacity outcomes and clearly
  separates throughput sustainability from latency among delivered samples.

## Limitations

One cell per mode/rate is descriptive and does not estimate run-to-run
variance. Latency percentiles under partial delivery describe delivered
survivors and MUST NOT be presented as equivalent to full-delivery latency.
