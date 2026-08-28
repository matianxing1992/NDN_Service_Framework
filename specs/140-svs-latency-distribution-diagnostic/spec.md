# Feature Specification: SVS Latency Distribution Diagnostic

**Feature Branch**: `140-svs-latency-distribution-diagnostic`

**Created**: 2026-07-23

**Status**: Frozen

**Input**: "In a new diagnostic experiment record mean, p50, p95, and p99
simultaneously; do not reconstruct p50 in frozen results."

## Evidence Boundary

- The Spec 136 formal campaign and all reports derived from it remain frozen.
- Missing percentiles MUST NOT be inferred, interpolated, or reconstructed from
  the frozen Spec 136 summaries.
- This Spec creates a new binary identity, result root, schema, and diagnostic
  report.
- It compares only the existing `face-inline-rsa` and `worker-rsa` paths at
  400 publications/s per peer.
- This is a descriptive diagnostic with two fresh cells, not a replacement
  formal matrix and not a repeated-trial significance claim.

## User Scenarios & Testing

### User Story 1 - Preserve The Full Latency Distribution (Priority: P1)

As a reviewer, I need the measured delivery-latency population retained so that
mean and percentiles are reproducible rather than reconstructed after the run.

**Independent Test**: Feed known latency samples through the benchmark summary
and offline analyzer and verify sample count, mean, p50, p95, and p99.

**Acceptance Scenarios**:

1. **Given** a peer completes a measured window, **when** it writes evidence,
   **then** it records every measured delivery latency and reports count, mean,
   p50, p95, and p99 from that same population.
2. **Given** a legacy summary containing only p99, **when** the new analyzer
   reads it, **then** it rejects the input instead of fabricating p50.
3. **Given** two peer sample files for one mode, **when** aggregate statistics
   are computed, **then** samples are concatenated before calculating
   percentiles; peer percentiles are never averaged.

### User Story 2 - Run A Fresh Two-Cell Diagnostic (Priority: P2)

As an NDN-SVS maintainer, I need one new 400 pps Face-inline cell and one new
400 pps one-worker cell under identical MiniNDN conditions.

**Independent Test**: Run exactly two two-node MiniNDN cells with 10/60/10
timing and analyze the newly retained samples.

**Acceptance Scenarios**:

1. **Given** either cell, **when** the experiment runs, **then** both independent
   nodes publish and subscribe at 400 pps with RSA-2048 signing and validation.
2. **Given** the cell completes, **when** it is admitted, **then** each peer's
   attempted rate is within 2% of target and all security, seriality, and
   accounting checks pass.
3. **Given** both cells terminate, **when** analysis runs, **then** it reports
   mean, p50, p95, and p99 for each peer and for the combined mode population.

### Edge Cases

- A cell fails or misses offered load: retain it as negative diagnostic
  evidence; do not replace it automatically.
- The number of samples differs from `deliveredMeasured`: reject the cell.
- No measured delivery samples exist: report the cell as inadmissible, not as
  zero latency.
- A sample file is truncated or malformed: reject it before comparison.

## Requirements

### Functional Requirements

- **FR-001**: Preserve the frozen Spec 136 result tree byte-for-byte and use
  `results/spec140-svs-latency-distribution/` for all new evidence.
- **FR-002**: Add a distinct Spec 140 experiment mode and schemas without
  changing the existing Spec 136 formal command or matrix.
- **FR-003**: Retain one integer nanosecond delivery-latency sample for every
  counted measured delivery.
- **FR-004**: Each peer summary MUST include `deliverySamples`,
  `deliveryMeanNs`, `deliveryP50Ns`, `deliveryP95Ns`, and `deliveryP99Ns`.
- **FR-005**: Summary statistics and the sample file MUST describe the same
  population, and `deliverySamples` MUST equal `deliveredMeasured`.
- **FR-006**: The analyzer MUST recompute all four statistics from raw samples
  and reject any mismatch with the peer summary.
- **FR-007**: Combined mode statistics MUST be calculated from concatenated raw
  peer samples, never from averaged or maximized peer percentiles.
- **FR-008**: Legacy evidence lacking the required samples or fields MUST be
  rejected and MUST NOT be backfilled.
- **FR-009**: Run exactly two fresh 400 pps-per-peer cells:
  `face-inline-rsa` followed by `worker-rsa`.
- **FR-010**: Each cell MUST use two MiniNDN nodes, and both processes MUST
  publish and subscribe for 10 s warmup, 60 s measurement, and 10 s drain.
- **FR-011**: Both cells MUST use the same newly built binary, NDN-SVS library,
  topology, 10 ms one-way link delay, payload, fetch window, Sync batching,
  RSA identities, CPU affinity, and independent APP pacer.
- **FR-012**: Each peer MUST achieve 392--408 attempted pps during measurement;
  failures are retained and classified, never silently retried.
- **FR-013**: The report MUST include per-peer and combined count, mean, p50,
  p95, and p99 in milliseconds, plus delivery and admission ratios.
- **FR-014**: The conclusion MUST be limited to the observed 400 pps cells and
  MUST distinguish central tendency from tail latency and delivery capacity.

### Key Entities

- **LatencySampleSet**: Raw nanosecond delivery latencies from one peer.
- **PeerDistribution**: Count, mean, p50, p95, and p99 verified against samples.
- **ModeDistribution**: Correct statistics over both peers' concatenated
  samples.
- **DiagnosticReceipt**: Immutable terminal record for one fresh cell.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Tests prove exact mean/p50/p95/p99 calculation on known samples.
- **SC-002**: Tests prove missing raw samples and legacy p99-only summaries are
  rejected.
- **SC-003**: Exactly two new MiniNDN cells are retained with no modification to
  the frozen Spec 136 campaign.
- **SC-004**: The final report presents reproducible mean, p50, p95, and p99
  from the newly recorded delivery samples.

## Assumptions

- The host provides four logical CPUs and the experiment remains pinned to
  CPUs 0--3.
- The current NDN-SVS library and RSA benchmark mechanism remain unchanged;
  only evidence capture and independent diagnostic orchestration are added.
