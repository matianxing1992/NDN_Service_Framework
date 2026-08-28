# Feature Specification: NDN-SVS Fetcher Queue Causality Diagnostic

**Feature Branch**: `135-svs-fetcher-queue-causality`

**Created**: 2026-07-22

**Status**: Complete / Frozen

**Input**: Re-measure the synchronous NDN-SVS rate boundary with the RSA-2048
Data signing used by the deployment, then experimentally test whether that
boundary is caused jointly by the fixed 10-Interest Fetcher window, fallback
pressure after bounded piggyback, bursty missing-range expansion, and one
shared Face/io_context thread. After measurement, analyze solution directions
without implementing a production fix.

## Frozen Evidence Boundary

- Spec 133 and its five formal cells are immutable and MUST NOT be rerun,
  relabeled, overwritten, or used as treatment cells.
- Spec 135 is a diagnostic intervention, not a new NDN-SVS performance claim.
- The historical profiled head
  `e9913c9a957a214d699ab5eb0bc99684e06573c5` is the parent of one isolated
  diagnostic-only patch. The active NDN-SVS checkout is not modified.
- Spec 133 remains a `DigestSha256` baseline and MUST NOT be relabeled as RSA.
- The sole admissible Spec 135 campaign is
  `results/spec135-svs-fetcher-queue-causality/spec135-rsa-confirm02-20260723T051420Z`.
  It is closed and MUST NOT be rerun or selectively supplemented.
- Spec 135 deliberately changes Data signing to a freshly generated RSA-2048
  identity. HMAC Sync Interests, validators disabled, compression disabled,
  payload size, topology, CPU assignment, and the single-I/O-thread model
  remain matched to Spec 133.

## User Scenarios & Testing

### User Story 1 - Establish The RSA Boundary (Priority: P1)

As an evaluator, I can see a fresh 200/400/600/800/1000-pps sweep using verified
RSA-2048 Data signatures so that the deployment security cost is included
before any bottleneck claim is made.

**Independent Test**: A frozen stage-A manifest contains exactly five once-only
cells with the original Fetcher window 10 and 4096-byte ApplicationParameters
limit. Each peer proves that a probe Data packet signed by the same signer has
signature type `SignatureSha256WithRsa`.

**Acceptance Scenarios**:

1. **Given** a peer process, **when** it starts, **then** it creates one
   RSA-2048 identity before warmup, configures the NDN-SVS Data signer from that
   identity, signs a probe, and refuses to measure unless the wire signature
   type is RSA.
2. **Given** the five rates, **when** stage A closes, **then** the first valid
   rate crossing the preregistered instability rule is selected without
   rerunning or selectively replacing any sweep cell.

### User Story 2 - Isolate Window And Piggyback Effects (Priority: P1)

As an evaluator, I can compare a preregistered 2x2 intervention at the first
RSA boundary so that the effects of Fetcher admission width and piggyback
capacity are not conflated. The matching `W10-P4096` stage-A cell is the
baseline and is not rerun.

**Independent Test**: After stage A seals the chosen rate, a stage-B manifest
contains exactly three once-only treatment cells: `W40-P4096`, `W10-P7168`,
and `W40-P7168`. Combined with the one stage-A baseline at that rate, these
form the preregistered 2x2.

**Acceptance Scenarios**:

1. **Given** the four factor combinations, **when** both manifests are
   inspected, **then** only those two factors differ within the selected rate.
2. **Given** a cell failure or unfavorable result, **when** execution closes,
   **then** one terminal receipt remains and no automatic retry occurs.

### User Story 3 - Observe The Proposed Queueing Chain (Priority: P1)

As a developer, I can relate missing-range bursts and fallback admission to
Fetcher queue residence, Sync receive callback residence, publication timer
misses, and delivery.

**Independent Test**: Each peer result includes missing-range batch width,
Mapping/Payload fallback counts, queue-wait calls and duration, Sync receive
calls and duration, scheduled/attempted/missed releases, and delivery.

**Acceptance Scenarios**:

1. **Given** an incoming missing range, **when** it is expanded, **then** its
   batch width is recoverable from one timestamp-bound event group.
2. **Given** a Fetcher intervention, **when** the cell completes, **then** its
   effective window and piggyback limit are recorded in commands, environment,
   logs, and the terminal receipt.
3. **Given** queue growth on the shared I/O thread, **when** analysis runs,
   **then** waiting time is not added to leaf CPU time.

### User Story 4 - Bound The Engineering Recommendation (Priority: P2)

As a researcher, I receive a falsifiable conclusion about each proposed cause
and a ranked set of possible fixes without silently modifying NDN-SVS.

**Independent Test**: The final report labels each hypothesis `SUPPORTED`,
`PARTIAL`, `NOT SUPPORTED`, or `INCONCLUSIVE`, cites both factor contrasts, and
separates measured evidence from proposed remediation.

## Edge Cases

- A larger ApplicationParameters block may approach transport MTU and introduce
  fragmentation; 7168 bytes leaves headroom under the configured 8800-byte face
  MTU and the actual wire size is retained.
- A larger Fetcher window may move the bottleneck to NFD/network queues rather
  than improve delivery.
- Missing ranges arrive in bursts, so average arrival rate alone may remain
  below the ideal window/RTT capacity while queue tails still grow.
- Overloaded waits may be censored at the drain boundary. Completed-wait means
  are survivor metrics and must be paired with queue call counts and delivery.
- One observation per factor combination supports descriptive intervention
  evidence, not confidence intervals or population inference.
- If no rate crosses the instability rule, stage B uses 1000 pps only as the
  highest tested stress point and the report MUST say that no RSA boundary was
  observed through 1000 pps.

## Requirements

### Functional Requirements

- **FR-001**: Spec 133 source, manifests, results, reports, receipts, and frozen
  worktrees MUST remain unchanged except for clarification of the already
  measured `DigestSha256` security profile.
- **FR-002**: The diagnostic subject MUST start from exact profiled head
  `e9913c9a957a214d699ab5eb0bc99684e06573c5` in a new worktree and contain only
  a hash-recorded diagnostic patch.
- **FR-003**: The patch MAY expose a bounded diagnostic Fetcher window setting
  and diagnostic counters/events; it MUST NOT be merged, installed globally,
  or presented as a production solution.
- **FR-004**: Each peer MUST create an RSA-2048 identity before warmup, bind
  the NDN-SVS Data signer to that identity, and verify a same-signer probe has
  TLV signature type `SignatureSha256WithRsa`; key generation MUST be excluded
  from measured intervals.
- **FR-005**: The driver MUST expose the maximum Sync ApplicationParameters
  size and record missing-range batches while retaining synchronous `publish()`
  on the only Face/io_context thread.
- **FR-006**: Stage A MUST contain exactly five fresh cells in fixed order
  `200, 400, 600, 800, 1000` pps/peer using `W10-P4096`. Its boundary is the
  lowest infrastructure-valid rate where any peer attempts less than 98% of
  scheduled releases, or aggregate delivered/attempted is below 98%. If none
  crosses, 1000 pps becomes a labeled stress point, not a measured boundary.
- **FR-007**: After all five stage-A receipts close, the runner MUST seal the
  chosen rate and a stage-B manifest with exactly three cells in fixed order:
  `W40-P4096`, `W10-P7168`, `W40-P7168`. The matching stage-A `W10-P4096`
  receipt is reused as the baseline and MUST NOT be rerun.
- **FR-008**: Every cell MUST use two MiniNDN nodes/processes launched with
  MiniNDN's per-node `HOME`/working-directory environment, 10 ms link delay,
  100 Mbps, zero configured loss, 256-byte payload, 10-second warmup, 60-second
  measurement, 10-second drain, and identical CPU/security/profile settings.
- **FR-009**: Every cell MUST have one attempt, a unique path, bounded timeout,
  verified dual-prefix routes, raw logs, resource samples, and one immutable
  terminal receipt; no automatic retry or replacement is permitted.
- **FR-010**: Per-peer analysis MUST report scheduled, attempted, missed, and
  delivered publications; delivery ratio; missing-batch count/p50/p95/max;
  Mapping/Payload fallback calls; queue wait calls/mean/max; Sync receive
  calls/total/mean; publication CPU stages including RSA sign; and CPU
  utilization.
- **FR-011**: Factor contrasts MUST compare window 10 versus 40 at both
  piggyback levels and 4096 versus 7168 bytes at both window levels.
- **FR-012**: Queue/external waits MUST remain separate from CPU durations, and
  aggregate callback residence MUST not be added to child CPU totals.
- **FR-013**: The report MUST state that shared-I/O causality is supported only
  by temporal/co-variation evidence because an unsafe cross-thread treatment is
  prohibited for the historical subject.
- **FR-014**: The report MUST include an ARS Material Passport, commands,
  subject/patch/binary hashes, anomalies, a full 11-type fallacy scan, and
  reproducibility limits.
- **FR-015**: Proposed fixes MUST be ranked by the measured mechanism they
  address and MUST remain analysis only; production NDN-SVS and NDNSF code are
  out of scope.

### Key Entities

- **DiagnosticSubject**: Parent commit, diagnostic patch, build, binary/library,
  driver, security profile, and hashes.
- **SweepCell**: Offered rate, `W10-P4096` controls, RSA proof, result path, and
  terminal receipt.
- **FactorCell**: Selected rate, window, ApplicationParameters limit, fixed
  controls, result path, and terminal receipt.
- **PeerDiagnostic**: Missing batches, Fetcher waits, Sync callback residence,
  release misses, delivery, and resource use for one peer.
- **CausalFinding**: Hypothesis, two matched contrasts, alternative
  explanations, verdict, and solution direction.

## Success Criteria

- **SC-001**: Structural and source-contract tests prove exactly five stage-A
  cells, three stage-B cells, RSA-2048 Data signing, and no change to Spec 133
  authorities.
- **SC-002**: Eight once-only terminal receipts exist, including any negative
  or infrastructure-invalid outcome.
- **SC-003**: Every complete peer supplies all FR-010 metrics with no invalid
  payload, an RSA signature proof, and a recorded effective factor
  configuration.
- **SC-004**: The window hypothesis is called supported only if both matched
  window contrasts reduce queue pressure and release misses without reducing
  delivery; otherwise it is partial, unsupported, or inconclusive.
- **SC-005**: The piggyback hypothesis is called supported only if both matched
  capacity contrasts reduce fallback pressure and downstream queue/release
  pressure; otherwise it is partial, unsupported, or inconclusive.
- **SC-006**: The shared-I/O hypothesis reports whether peer-level Sync receive
  residence and Fetcher pressure co-vary with missed releases and whether either
  intervention changes those outcomes.
- **SC-007**: The final report identifies what the experiment falsifies, what it
  cannot prove, and which next implementation experiment would distinguish the
  remaining alternatives.

## Assumptions

- MiniNDN namespaces share the host monotonic clock.
- The factor interventions are diagnostic and are never runtime defaults.
- A single run per cell is sufficient for the requested diagnostic round but
  insufficient for statistical generalization.
