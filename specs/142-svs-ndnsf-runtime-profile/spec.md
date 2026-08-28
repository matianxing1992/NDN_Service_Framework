# Feature Specification: NDN-SVS Worker Validation Under the NDNSF Runtime Profile

**Feature Branch**: `142-svs-ndnsf-runtime-profile`

**Created**: 2026-07-23

**Status**: Closed with a negative qualification result — 600/800 not authorized

**Input**: "Use the same effective configuration as the NDNSF experiment:
protocol V3; the bidirectional publish/subscribe workload may remain; Fetch
retry must not affect a zero-loss experiment."

## Evidence Correction And Boundary

- Specs 140 and 141 and their result directories are frozen. They MUST NOT be
  modified, rerun, or selectively replaced.
- Their forced V2, one-byte piggyback-limit configuration is retained only as a
  forced-Fetch stress diagnostic. It MUST NOT be cited as evidence of
  production-worker behavior under the effective NDNSF runtime profile.
- Spec 142 creates a new binary manifest, runner namespace, raw result root, and
  report. No Spec 140/141 measurement can satisfy a Spec 142 cell.
- This is an NDN-SVS microbenchmark under the effective NDNSF SVS transport
  profile. It intentionally retains two symmetric publishing/subscribing peers;
  it does not claim to reproduce the asymmetric
  REQUEST/ACK/SELECTION/RESPONSE message mix.

## User Scenarios & Testing

### User Story 1 - Freeze the Effective NDNSF SVS Profile (Priority: P1)

As an NDN-SVS maintainer, I need the benchmark to resolve and record the
configuration that ServiceUser and ServiceProvider actually pass to NDN-SVS so
the experiment represents the current NDNSF data path rather than an artificial
forced-Fetch profile.

**Why this priority**: A worker comparison is not meaningful until the
mode-independent protocol, piggyback, Fetch, Sync, security, and CPU settings
match.

**Independent Test**: Start both peers in configuration-only mode and compare
their machine-readable resolved profiles against the experiment contract.

**Acceptance Scenarios**:

1. **Given** the current NDNSF source and NDN-SVS defaults, **when** each peer
   resolves its profile, **then** it reports protocol V3, a resolved
   `maxPiggyDataSize` of 800 bytes, 1 ms suppression, and the NDNSF adaptive
   publication-Fetch window for the offered rate.
2. **Given** the NDNSF MiniNDN runner exports
   `NDNSF_SVS_MAX_PIGGYDATA_BYTES=4096`, **when** the profile is resolved,
   **then** the manifest records that this variable is not consumed by the
   current ServiceUser/ServiceProvider code and does not misreport 4096 as the
   effective limit.
3. **Given** inline and worker modes, **when** their profiles are compared,
   **then** the only NDN-SVS behavior difference is
   `publicationPreparationWorkers=0` versus `1`.

---

### User Story 2 - Qualify the Corrected 400 pps Pair (Priority: P2)

As an experiment reviewer, I need one fresh matched pair at 400 pps per peer to
prove that the corrected V3/NDNSF-profile harness produces the requested load
without retry-driven recovery before more expensive rates are attempted.

**Why this priority**: It prevents four more formal cells from being spent on a
misconfigured or incorrectly linked benchmark.

**Independent Test**: Run exactly one 10/60/10 cell in each mode at 400 pps per
peer and validate the profile, binary identity, attempted rate, security,
accounting, and Fetch-health gates.

**Acceptance Scenarios**:

1. **Given** two MiniNDN nodes, **when** the 400 pps qualification pair runs,
   **then** both nodes continuously publish and subscribe using RSA-2048 and
   each attempted rate is within +/-2% of 400 pps during measurement.
2. **Given** zero configured link loss, **when** a cell finishes, **then** no
   publication or Mapping retry, timeout, or Nack was observed.
3. **Given** a retry, timeout, Nack, linkage mismatch, configuration mismatch,
   process failure, or accounting mismatch, **when** the cell is classified,
   **then** it is retained as `PROFILE_INVALID` diagnostic evidence and is not
   used to compare worker latency or capacity.
4. **Given** both qualification cells have terminal receipts, **when** the gate
   is evaluated, **then** the 600/800 stage is enabled only if both receipts
   satisfy every profile and harness invariant; delivery sustainability itself
   is an outcome, not a prerequisite.

---

### User Story 3 - Measure the Higher-Rate Worker Boundary (Priority: P3)

As an NDN-SVS maintainer, I need matched 600 and 800 pps results under the same
qualified profile to determine whether one FIFO publication-preparation worker
reduces Face blocking or extends sustainable throughput without changing the
signing algorithm.

**Why this priority**: These rates answer the performance question only after
the corrected profile is proven valid.

**Independent Test**: After the 400 pps gate passes, run exactly one inline and
one worker cell at 600 and 800 pps, preserve all outcomes, and recompute
throughput and latency from raw samples.

**Acceptance Scenarios**:

1. **Given** a passed 400 pps qualification gate, **when** the higher-rate stage
   runs, **then** it executes exactly
   `[600-inline, 600-worker, 800-inline, 800-worker]` with no automatic rerun.
2. **Given** a profile-valid cell, **when** raw data is analyzed, **then** the
   report includes attempted and delivered pps per peer, delivery ratio, and
   delivery-latency mean, p50, p95, and p99.
3. **Given** an overload outcome without Fetch recovery activation, **when** it
   is analyzed, **then** it is retained as a capacity result; percentiles from
   partial delivery are explicitly labeled survivor-distribution statistics.
4. **Given** Fetch recovery activates, **when** the cell is analyzed, **then**
   the report preserves the event but does not attribute that cell's latency or
   capacity difference solely to publication worker placement.

### Edge Cases

- A 256-byte application payload may encode to more than 800 bytes after names,
  TLV framing, and RSA signature. The harness MUST record the actual signed
  publication wire size and observed piggyback eligibility instead of assuming
  eligibility from payload size.
- Zero configured loss does not preclude overload-induced queue drops. Such
  Fetch timeout/retry activity is preserved as a failure-boundary observation,
  but it invalidates the clean worker-only causal comparison.
- A passed attempted-rate check with zero delivery is not sufficient evidence
  of a valid worker comparison; process, security, profile, Fetch-health, and
  accounting gates must also pass.
- The machine has four logical CPUs. The manifest MUST record affinity and all
  active worker pools; no conclusion may imply unconstrained CPU capacity.
- Installed and workspace NDN-SVS libraries can differ. A peer whose runtime
  library hash differs from the frozen manifest is invalid.

## Requirements

### Functional Requirements

- **FR-001**: Preserve Specs 140/141 and classify them only as forced-Fetch
  stress evidence.
- **FR-002**: Use one newly built C++17 benchmark binary for every Spec 142 cell
  and freeze its source commit/diff, SHA-256, compiler, link map, runtime
  library paths, and runtime library SHA-256 values before qualification.
- **FR-003**: Use two MiniNDN nodes on a 100 Mbps, 10 ms one-way, zero
  configured loss link. Each node MUST continuously publish and subscribe.
- **FR-004**: Use NDN-SVS protocol V3, 1 s Sync Interest lifetime, 1 ms Sync
  suppression, 30 s periodic Sync, `useTimestamp=false`, a 256-byte application
  payload, and RSA-2048 publication signing and validation.
- **FR-005**: Use the effective `SVSPubSubOptions.maxPiggyDataSize=800` unless
  current NDNSF source is changed in a separately reviewed feature to consume a
  different value. The runner's currently unconsumed 4096-byte environment
  value MUST NOT override this contract.
- **FR-006**: Use the NDNSF adaptive publication-Fetch window:
  `clamp(ceil(expectedRps * 0.64), 32, 128)`, unless an explicit window is
  already part of the referenced canonical NDNSF command. The resolved value
  MUST be recorded per peer and rate.
- **FR-007**: Freeze the current NDN-SVS Fetch constants equally in both modes:
  Mapping window 10, retries 0, failure backoff 200 ms; publication retries 2,
  inner retries 2, Interest lifetime 500 ms, adaptive lifetime bounds
  250--2000 ms, failure backoff 50 ms, and maximum backoff 2000 ms. These are
  controlled constants, not experimental variables.
- **FR-008**: Treat any observed publication/Mapping retry activation, Fetch
  timeout, or Nack as `PROFILE_INVALID` for the worker-only comparison. Preserve
  the cell and counts for diagnosis.
- **FR-009**: Enable the same NDNSF-default parallel Sync processing and
  parallel Sync production settings in both modes: four workers and queue 256
  for each pool, Sync signing on the Face thread, extra-block preparation in
  workers, and Sync batching disabled. Record every resolved value.
- **FR-010**: The sole treatment variable MUST be
  `publicationPreparationWorkers`: zero for `face-inline-rsa`, one for
  `worker-rsa`. The worker queue and ordered Face commit behavior MUST be fixed.
- **FR-011**: Use CPU affinity 0--3 for both modes and record system load,
  process CPU time, and active thread counts so the four-CPU resource boundary
  is visible.
- **FR-012**: Use independent high-resolution APP pacers and verify at the
  maximum requested rate that pacer-only attempted load is within +/-2% on
  both peers before formal MiniNDN cells.
- **FR-013**: Use 10 seconds warmup, 60 seconds measurement, and 10 seconds
  drain per formal cell.
- **FR-014**: Run the fresh 400 pps inline/worker pair first. Start the fresh
  600/800 pps cells only after the qualification gate passes.
- **FR-015**: Execute each authorized mode/rate cell exactly once. Do not
  automatically retry, replace, tune, or selectively omit a started cell.
- **FR-016**: Emit a terminal receipt for every started cell with profile,
  identity, workload, security, Fetch-health, accounting, and outcome fields.
- **FR-017**: Report, per peer and aggregate, attempted pps, delivered pps,
  delivery ratio, delivery-latency mean/p50/p95/p99, actual publication wire
  size, piggyback eligibility/hit counts, Mapping/Publication Fetch counts,
  retry counts, timeout counts, and Nack counts.
- **FR-018**: Recompute all summary statistics from raw delivery samples and
  reject any mismatch between raw counts/statistics and peer summaries.
- **FR-019**: Limit conclusions to the measured two-node zero-loss topology and
  distinguish moving RSA publication preparation off the Face thread from
  accelerating RSA itself.

### Key Entities

- **RuntimeProfileManifest**: Frozen source, binary, library, topology,
  security, protocol, Sync, piggyback, Fetch, pacing, CPU, and timing controls.
- **CellReceipt**: One started cell's resolved profiles, counters, raw-data
  references, validity gates, terminal state, and reason.
- **ModeComparison**: A rate-matched inline/worker comparison constructed only
  from two `PROFILE_VALID` receipts.

## Success Criteria

### Measurable Outcomes

- **SC-001**: The profile verifier demonstrates V3 and an effective 800-byte
  piggyback limit on both peers and identifies zero mode-independent
  configuration differences.
- **SC-002**: Both 400 pps qualification cells reach attempted pps within +/-2%
  per peer and have zero publication/Mapping retry, timeout, and Nack events.
- **SC-003**: Every started formal cell has exactly one terminal receipt and no
  replacement run.
- **SC-004**: Every profile-valid cell's raw samples reproduce its delivery
  count and mean/p50/p95/p99 values.
- **SC-005**: The final report clearly separates valid worker comparisons,
  invalid/confounded diagnostics, and survivor-distribution latency.

## Assumptions

- The objective is to validate the benefit of one publication-preparation
  worker under NDNSF-effective NDN-SVS settings, not to reproduce the full
  NDNSF application protocol.
- One run per mode/rate is descriptive and does not estimate run-to-run
  variance.
- No NDN-SVS periodic timing, suppression, retry, or piggyback tuning is
  authorized while the formal campaign is active.
