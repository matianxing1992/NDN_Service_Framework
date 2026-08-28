# Feature Specification: NDN-SVS Zero-Loss Fetch Timeout Causality

**Feature Branch**: `143-svs-zero-loss-fetch-causality`

**Created**: 2026-07-24

**Status**: Complete — `DIAGNOSED` (2026-07-24)

**Input**: Continue from the frozen Spec 142 negative qualification by
identifying why publication and Mapping Fetch timeout/retry activity occurs on
a MiniNDN link configured with zero packet loss. Do not rerun Spec 142 or start
the blocked 600/800 cells.

## Evidence Boundary

- Spec 142 and
  `results/spec142-svs-ndnsf-runtime-profile/campaign-20260724T012559Z`
  are immutable baseline evidence. No Spec 143 run may be written there,
  substituted for either 400 pps cell, or used to reopen the 600/800 gate.
- The Spec 142 r4 result proves timeout/retry activation, but its
  `NDN_LOG='*=WARN'` logs cannot identify the causal branch.
- Spec 143 is diagnosis-only. It may add bounded observability, a runner, and an
  analyzer, but it MUST NOT tune Fetch windows, lifetimes, retries, protocol
  timing, piggyback size, workload, or worker counts and MUST NOT implement a
  production recovery fix.

## User Scenarios & Testing

### User Story 1 - Correlate Each Fetch Across Both Peers (Priority: P1)

As an NDN-SVS maintainer, I need a per-Interest causal timeline that connects
the consumer's fallback dispatch with the producer's DataStore lookup and
response so a timeout is attributed to an observed branch rather than guessed
from aggregate counters.

**Why this priority**: The current counters do not distinguish a producer store
miss from a late response, a missing response, or a consumer-side callback
delay.

**Independent Test**: Feed synthetic interleaved logs for two peers into the
analyzer and verify deterministic classification for producer miss, late Data,
no producer observation, producer hit without put, and unclassified sequences.

**Acceptance Scenarios**:

1. **Given** a dispatched Interest that reaches the producer and misses the
   DataStore, **when** the consumer times out, **then** the analyzer classifies
   it as `PRODUCER_STORE_MISS`.
2. **Given** a producer store hit and `Face::put`, **when** the consumer times
   out without a Data callback for that attempt, **then** it is classified as
   `PRODUCER_PUT_WITHOUT_CONSUMER_DATA`; any later same-name Data is retained
   as a secondary observation.
3. **Given** a consumer timeout without a matching producer observation,
   **when** the trace is analyzed, **then** it is classified as
   `NO_PRODUCER_OBSERVATION`, not silently called packet loss.
4. **Given** repeated attempts for the same name, **when** timelines are
   reconstructed, **then** consumer attempt identifiers and the Interest Nonce
   observed on both peers prevent retry events from being merged into the wrong
   attempt.

---

### User Story 2 - Reproduce the Boundary Once Under the Frozen Profile (Priority: P2)

As an experiment reviewer, I need one fresh, minimally instrumented 400 pps
worker cell under the same effective NDNSF V3 profile to determine which causal
branch accounts for the Spec 142 failure without spending another matrix.

**Why this priority**: The worker cell already exhibited the common timeout
boundary while maintaining the cleanest delivery behavior; one diagnostic cell
is the smallest useful reproduction.

**Independent Test**: Run one two-node, bidirectional, 10/60/10 MiniNDN worker
cell and verify its immutable receipt, trace coverage, CPU accounting, and
timeout classification.

**Acceptance Scenarios**:

1. **Given** the frozen topology and profile, **when** the diagnostic cell
   starts, **then** both peers publish and subscribe at 400 pps using RSA-2048,
   protocol V3, the 800-byte piggyback limit, the adaptive 128 Fetch window,
   and one publication-preparation worker.
2. **Given** a 60-second measurement window, **when** the cell finishes,
   **then** publication inner retries, publication outer retry activations,
   publication timeouts, Mapping timeouts, and Nacks are reported separately.
3. **Given** measurement-window Fetch timeouts, **when** the trace is analyzed,
   **then** at least 95% are assigned to a causal class backed by events from
   both peer logs, or the Spec closes as inconclusive with the missing seam
   named.
4. **Given** the diagnostic cell does not reproduce any measurement-window
   timeout, **when** its receipt closes, **then** no replacement run is started;
   one conditional inline cell may be authorized only by an explicit recorded
   decision explaining why it is necessary.

---

### User Story 3 - Distinguish CPU Saturation from Fetch Semantics (Priority: P3)

As an NDN-SVS maintainer, I need process CPU consumption during the exact
measurement window so the report can tell whether a semantic store/response
failure occurred under spare CPU or alongside resource saturation.

**Why this priority**: Spec 142 required CPU evidence but did not emit it.
Retrofitting a number is impossible; it must be measured in the new run.

**Independent Test**: Unit-test process-resource snapshots and recompute CPU
deltas and utilization from the peer summaries.

**Acceptance Scenarios**:

1. **Given** start and end `getrusage` snapshots, **when** the summary is
   written, **then** it reports user CPU, system CPU, total CPU, one-core
   utilization, four-core-normalized utilization, maximum RSS, and process
   thread count for the measurement window.
2. **Given** wall and CPU deltas, **when** the analyzer validates them, **then**
   inconsistent, negative, or impossible values invalidate the resource
   evidence without altering the raw Fetch diagnosis.

### Edge Cases

- A later retry for the same name may receive Data after an earlier attempt
  timed out. The timeline retains both attempts instead of merging their
  outcomes.
- One name may have multiple inner and outer retry attempts. Correlation uses
  peer, full Interest name, per-process attempt identifier, Interest Nonce, and
  event order.
- Producer and consumer clocks are not assumed synchronized for one-way delay.
  Cross-peer classification uses event order and bounded clock-offset evidence;
  same-process queue, lifetime, and callback durations use monotonic time.
- An Interest may be satisfied from piggyback cache before remote dispatch.
  Such a record is not counted as a network Fetch attempt.
- NFD may aggregate repeated Interests. The report distinguishes application
  dispatches from producer observations and does not infer link loss from their
  difference alone.
- TRACE logging can perturb timing. Trace volume and file size are reported,
  and the conclusion is limited to causal classification rather than a new
  worker-performance comparison.

## Requirements

### Functional Requirements

- **FR-001**: Preserve Spec 142 documents, raw results, terminal receipts,
  qualification verdict, and negative conclusion without modification or
  rerun.
- **FR-002**: Add structured, bounded `NDN_LOG` events at the NDN-SVS consumer
  Fetcher queue/dispatch/Data/Nack/timeout/validation boundaries, publication
  producer DataStore lookup/Face-put boundary, and Mapping producer
  query/empty/Face-put boundary, always including a full Interest name and
  Interest Nonce, plus a per-process attempt identifier where available.
- **FR-003**: Add SVSPubSub context events that distinguish publication
  fallback queue/dispatch/success/outer-retry/expiry, piggyback cache
  satisfaction, and Mapping Fetch dispatch/outcome.
- **FR-004**: Observability MUST NOT change wire names, packets, retry policy,
  scheduling, queue capacity, security behavior, or public NDN-SVS APIs.
- **FR-005**: Use exactly two MiniNDN nodes on the Spec 142 100 Mbps, 10 ms
  one-way, zero configured loss link; both peers continuously publish and
  subscribe.
- **FR-006**: Use the unchanged Spec 142 effective NDNSF V3 configuration,
  RSA-2048 sign/validate path, 256-byte payload, four-CPU affinity, one
  publication-preparation worker, and 400 pps per peer.
- **FR-007**: Use 10 seconds warmup, 60 seconds measurement, and 10 seconds
  drain. Start the worker diagnostic cell exactly once with no automatic retry,
  tuning, or replacement.
- **FR-008**: Set `NDN_LOG` only for the four diagnostic NDN-SVS components;
  retain WARN for other components and capture each peer's stdout/stderr
  separately.
- **FR-009**: Emit a content-addressed build/runtime manifest, exact command
  record, topology, per-peer summary, raw trace, analyzer output, and one
  terminal receipt under a new Spec 143 result root.
- **FR-010**: Report Mapping and publication Interest/Data/Nack/timeout counts;
  separate Fetcher inner retry count from SVSPubSub outer retry activation
  count.
- **FR-011**: Classify every measurement-window timeout as one of
  `PRODUCER_STORE_MISS`, `PRODUCER_PUT_WITHOUT_CONSUMER_DATA`,
  `NO_PRODUCER_OBSERVATION`, `PRODUCER_STORE_HIT_WITHOUT_PUT`, or
  `UNCLASSIFIED`, with linked raw event references. Later same-name Data and
  validation success/failure are reported separately and are never labeled the
  cause of a prior timeout.
- **FR-012**: Report queue-to-dispatch, dispatch-to-timeout/Data, producer
  lookup, producer lookup-to-put, and Data-to-validation durations wherever
  their endpoints exist; never subtract unsynchronized peer monotonic clocks.
- **FR-013**: Record measurement-window process user/system CPU deltas,
  one-core and four-core-normalized CPU utilization, max RSS, thread count, and
  trace file volume per peer.
- **FR-014**: Require at least 95% classification coverage for observed
  measurement-window timeouts. If the run has zero such timeouts or coverage is
  below 95%, close as `INCONCLUSIVE` and name the missing evidence.
- **FR-015**: The final report MUST distinguish measured facts, source-derived
  mechanism statements, and hypotheses; it MUST NOT claim that zero configured
  loss means zero queue loss or zero network failure.
- **FR-016**: Do not run an inline diagnostic cell unless the worker cell has
  zero timeouts or the root-cause classifier proves a mode comparison is
  necessary. The authorization and reason must be recorded before it starts.
- **FR-017**: Do not modify production recovery behavior in Spec 143. Any
  proposed fix and its validation matrix belong to a new Spec after the causal
  report closes.

### Key Entities

- **FetchTraceEvent**: Peer, component, monotonic and log timestamp, event,
  Interest name, Nonce, attempt identifier, retry state, queue/pending sizes,
  and optional duration/result.
- **FetchAttemptTimeline**: Consumer and producer events correlated for one
  dispatched attempt, its causal class, evidence references, and derived
  same-clock durations.
- **ResourceWindow**: Measurement start/end wall time, process CPU snapshots,
  normalized utilization, RSS, thread counts, and trace volume.
- **DiagnosticReceipt**: Frozen identity/configuration, commands, artifacts,
  counters, classification coverage, terminal state, and conditional-cell
  authorization.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Focused tests prove all five timeout classifications, retry-attempt
  separation, unsynchronized-clock protection, and malformed/incomplete trace
  rejection.
- **SC-002**: The exactly-once worker cell retains the Spec 142 runtime profile,
  reaches attempted rate within +/-2% per peer, and produces complete traces
  and resource windows for both peers.
- **SC-003**: At least 95% of observed measurement-window timeouts have one
  evidence-backed causal class; otherwise the report is explicitly
  `INCONCLUSIVE`.
- **SC-004**: Inner Fetcher retries, outer publication retries, timeouts,
  Nacks, producer store hits/misses, late Data, CPU, RSS, threads, and trace
  volume are independently reported and reproducible from raw artifacts.
- **SC-005**: No Spec 142 artifact changes and no Spec 143 production recovery
  or configuration-tuning change exists.

## Assumptions

- The first diagnostic cell uses worker mode because Spec 142 observed the
  boundary there while delivery remained complete; this minimizes unrelated
  inline backlog.
- One diagnostic cell provides mechanism evidence, not run-to-run variance or
  a performance effect estimate.
- NDN logging timestamps and process monotonic durations are sufficient for
  event ordering, but cross-host one-way latency is not inferred without an
  explicit clock-offset measurement.
