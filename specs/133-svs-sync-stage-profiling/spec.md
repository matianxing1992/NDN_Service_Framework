# Feature Specification: Synchronous NDN-SVS Stage Profiling

**Feature Branch**: `133-svs-sync-stage-profiling`

**Created**: 2026-07-22

**Status**: Complete. The corrected single-I/O-thread driver and verified
dual-prefix routing passed the fresh three-arm overhead gate; the sealed
200/400/600/800/1000 pps matrix completed exactly once with five valid
receipts. Canonical campaign:
`spec133-confirm-io01-20260723T040005Z`.

**Input**: Profile the NDN-SVS version before asynchronous publication and
parallel processing were introduced. At 200, 400, 600, 800, and 1000 PubSub
publications per second per peer, break synchronization into every potentially
expensive stage, including Mapping encoding/decoding and Mapping Data signing,
so the dominant bottleneck can be identified from structured diagnostic logs.

## Frozen Subject And Evidence Boundary

| Item | Contract |
|---|---|
| Base NDN-SVS commit | `a9944019f76791773604999f00128057b9534ace` |
| Capability | Synchronous `publish()`; no `publishAsync()`; no internal receive or production worker pools |
| Build compatibility | The canonical Boost 1.71 build-only patch used by Specs 131/132 |
| Formal build mode | `NDN_SVS_COMPRESSION` disabled; non-segmented 256-byte publication path |
| Profiling change | One frozen diagnostics-only patch whose source delta is hash-bound and reviewed before formal execution |
| Perturbation controls | Clean historical binary, profiled binary with profiling disabled, and the same profiled binary with formal logging enabled |
| Formal rates | Exactly 200, 400, 600, 800, and 1000 publications/s per peer |
| Formal cells | Exactly five once-only bidirectional cells, in ascending rate order |
| Prior evidence | Specs 131 and 132 remain unchanged and are not inputs to the Spec 133 aggregate |

The measured subject is the exact base commit plus the canonical Boost 1.71
build patch and the frozen profiling-only patch. The report MUST use that full
identity and MUST NOT describe the result as an unmodified upstream binary.

## User Scenarios & Testing

### User Story 1 - Observe Every Expensive PubSub Stage (Priority: P1)

As an NDN-SVS developer, I can see structured timing for the synchronous
publisher, Sync production/consumption, Mapping, publication retrieval, and
subscription-delivery stages, so a large end-to-end delay is attributable to a
specific CPU stage, queue, or network wait.

**Why this priority**: End-to-end delay alone cannot show whether the bottleneck
is Mapping encoding, signing, storage, state-vector processing, fetch, or the
application callback.

**Independent Test**: A local two-peer smoke publication produces valid stage
records for the publisher and subscriber, identifies the piggyback or fallback
path, and reconciles every top-level operation with its measured children and
explicit residual time.

**Acceptance Scenarios**:

1. **Given** a synchronous publication, **when** it returns, **then** the trace
   distinguishes inner Data construction/signing/encoding, outer Data
   construction/signing/storage, Mapping preparation/storage, local state
   update, Face output, and total API duration.
2. **Given** a Sync Interest is produced and received, **when** its trace is
   analyzed, **then** Mapping selection/encoding, piggyback processing,
   state-vector encoding/decoding, Interest signing/verification, merge,
   scheduling, and callback dispatch are independently visible.
3. **Given** Mapping or publication data cannot be satisfied by piggyback,
   **when** the fallback executes, **then** Interest construction/queueing,
   producer lookup/encode/sign/put, network wait, validation, decode, cache,
   and subscription delivery are independently visible.
4. **Given** two nested spans cover the same work, **when** summaries are
   produced, **then** aggregate spans are not added to leaf-stage CPU demand and
   no time is double-counted.

---

### User Story 2 - Measure The Five Synchronous Rate Boundaries (Priority: P1)

As an evaluator, I can exercise the same synchronous bidirectional PubSub usage
model at 200, 400, 600, 800, and 1000 publications/s per peer and preserve one
complete result per rate, so stage cost can be related to the onset of
throughput loss, queue growth, or delivery collapse.

**Why this priority**: A stage is a bottleneck only when its service demand or
waiting time explains the observed rate boundary; an isolated microbenchmark
is insufficient.

**Independent Test**: A sealed manifest contains exactly five ascending-rate
cells. Each cell launches one independent process on each of two MiniNDN
nodes. In each process, a single Face/io_context thread executes publication
timer callbacks, synchronous `publish()`, Sync processing, fetch processing,
and subscription callbacks for a 10-second warmup, 60-second measured window,
and bounded 10-second drain.

**Acceptance Scenarios**:

1. **Given** a formal cell at rate R, **when** it runs, **then** each peer
   targets R publications/s and concurrently subscribes to the opposite peer;
   aggregate offered load is reported as `2R`.
2. **Given** a slow synchronous call, **when** the Face/io_context thread misses
   later absolute release times, **then** missed release slots and deadline
   lateness increase while attempted rate decreases naturally; no catch-up
   burst, adapter queue, publisher thread, or worker hides the limit.
3. **Given** a crash, invalid trace, overload, or delivery loss, **when** the
   cell closes, **then** the negative outcome receives one immutable receipt
   and is not selectively rerun or replaced.
4. **Given** profiling is enabled, **when** any clean-control, instrumentation,
   logging, or total-overhead preflight comparison exceeds its bound, **then**
   the formal matrix is blocked instead of publishing biased bottleneck claims.

---

### User Story 3 - Rank Bottlenecks Without Conflating CPU And Waiting (Priority: P2)

As a researcher, I receive rate-by-rate tables that rank application-publication
CPU demand, remaining Face/io_context CPU demand, timer delay, and
external/network wait separately, so the report can explain which stage first
limits the shared serial event loop and how the limiting stage changes with
offered load.

**Why this priority**: The teacher needs a defensible explanation, not a list
of raw timestamps or a claim based only on the largest p99.

**Independent Test**: Synthetic stage records with known nested spans, missing
records, and piggyback/fallback branches generate the expected distributions,
exclusive demand shares, residuals, path ratios, and bottleneck classification.

**Acceptance Scenarios**:

1. **Given** one valid cell, **when** it is analyzed, **then** every stage
   reports operation count, sampled count, mean, p50, p95, p99, maximum,
   calls per delivered publication, estimated service demand, and thread role.
2. **Given** CPU stages and wait intervals, **when** they are ranked, **then**
   wait intervals never inflate CPU demand share and aggregate parents never
   inflate leaf demand.
3. **Given** the completed rate sweep, **when** a bottleneck is named, **then**
   the finding cites stage demand/share, rate-dependent queue or latency growth,
   and the corresponding attempted/delivered-rate boundary; otherwise the
   result is explicitly inconclusive.

### Edge Cases

- Mapping and publication payloads may arrive inside the Sync Interest, so a
  cell can use the piggyback fast path without Mapping or Payload Interests.
- At high rates, only part of the pending Mapping/payload set may fit in one
  ApplicationParameters block; branch counts must show the resulting fallback.
- Mapping candidate encoding may execute multiple times while selecting what
  fits. Operation count and time per final Sync Interest must both be retained.
- A scheduled 1 ms Sync Interest can be repeatedly delayed or replaced. Timer
  request, actual callback, lateness, and coalesced publication count are
  distinct measurements.
- Validation may be disabled, digest-based, HMAC-based, or certificate-based.
  The formal security profile and actual signer/validator mode must be recorded;
  a disabled validation stage is reported as zero calls, not missing evidence.
- Sampled records may be incomplete after a crash. Exact aggregate counters and
  the terminal receipt remain authoritative; partial distributions are labeled.
- Logging can become the bottleneck it is intended to measure. Logging overhead
  is tested before formal admission and is never subtracted post hoc.
- Mutex acquisition can still expose internal contention or instrumentation
  artifacts even though the formal application path has one execution thread.
  Lock wait is measured separately from protected work and never counted as
  leaf CPU demand.
- The historical README and public header do not state a threading contract.
  The chat examples claim thread safety and publish from an application thread
  while Face runs elsewhere, but the unit tests contain no concurrency
  coverage and sanitizer evidence shows that high-rate use can race. Formal
  profiling therefore uses the narrower single-I/O-thread model; the
  cross-thread example discrepancy remains a separately reported contract gap.
- The fixed 256-byte workload does not enter the segmented publication path,
  and compression is disabled. Neither path is silently generalized from the
  formal results.

## Requirements

### Functional Requirements

- **FR-001**: The profiling subject MUST be based on exact commit
  `a9944019f76791773604999f00128057b9534ace` and MUST contain no asynchronous
  publication API or internal parallel Sync/production worker behavior.
- **FR-002**: The active NDN-SVS checkout and the frozen Spec 131/132 worktrees,
  manifests, logs, and results MUST remain unchanged.
- **FR-003**: The subject MUST be built in a separate immutable worktree with
  the canonical Boost 1.71 build-only patch and one separately recorded,
  reviewed, hash-bound profiling-only patch.
- **FR-004**: Structured diagnostics MUST use the established NDN-SVS logging
  channel and MUST include cell, peer, thread role, stage, span kind,
  correlation key, start time, duration, outcome, byte count, and operation
  count when applicable.
- **FR-005**: The instrumentation MUST distinguish non-overlapping leaf CPU
  spans, aggregate parent spans, mutex lock waits, queue/scheduler waits,
  network/external waits, and application milestones. A protected leaf CPU
  timer starts only after its measured lock has been acquired.
- **FR-006**: The stage registry MUST cover synchronous publish, inner and outer
  Data creation/signing/encoding, datastore operations, Mapping creation,
  Mapping selection/encode/decode/sign/validation, state-vector encode/decode/
  merge, Sync Interest build/sign/verify/express, suppression scheduling,
  piggyback processing, fallback Mapping fetch, fallback publication fetch,
  content decode/validation, cache operations, callback delivery, and every
  explicit mutex acquisition on those measured paths.
- **FR-007**: Exact all-operation counters and deterministic sampled span
  records MUST be emitted. Sampling MUST retain all child spans of a sampled
  correlation key and use the same frozen rule in every formal cell.
- **FR-008**: Instrumentation MUST NOT add asynchronous logging workers,
  parallel processing, a publication adapter, a publisher/pacer thread, or any
  change to protocol decisions, names, NDN-SVS timers, signing mode, payload
  bytes, fetch behavior, or wire encoding. The harness MAY use one independent
  absolute-deadline timer on the existing Face io_context solely to release
  application publications on that same thread.
- **FR-009**: Before formal admission, one fixed 1000 publications/s-per-peer
  short three-arm preflight MUST run (A) the clean historical binary with no
  profiling patch, (B) the profiled binary with profiling disabled, and (C) the
  same profiled binary with the formal logging profile enabled. It MUST report
  A-vs-B instrumentation cost, B-vs-C logging cost, and A-vs-C total cost.
  Formal execution is blocked if attempted rate or delivery ratio changes by
  more than 5% relative, CPU utilization changes by more than 5 percentage
  points in any comparison, any enabled trace schema fails, or either peer
  fails. These three diagnostic arms are not formal cells.
- **FR-010**: The formal campaign MUST contain exactly five cells at
  `[200, 400, 600, 800, 1000]` publications/s per peer, in that order, with one
  attempt per cell and no automatic retry or selective replacement.
- **FR-011**: Every formal cell MUST use two MiniNDN nodes with one independent
  peer process per node. Each process MUST execute its application publication
  timer, synchronous `publish()`, Face processing, Sync/fetch work, and
  subscription callbacks on one Face/io_context thread. It MUST use a
  deterministic 256-byte payload, zero configured loss, the same two-node
  topology, compression disabled, 10-second warmup, 60-second measurement, and
  10-second drain. No application or NDN-SVS API call may originate from a
  second thread after event processing begins.
- **FR-012**: The analyzer MUST validate subject/build/log/manifest hashes,
  stage schema, monotonic timestamps, parent-child containment, call-count
  reconciliation, exact/ambiguous/censored correlation classification, path
  classification, peer/direction identity, and one terminal receipt per cell
  before producing claims. Ambiguous correlations MUST NOT produce network-wait
  or critical-path claims.
- **FR-013**: For every rate and stage, the analyzer MUST report call count,
  sample count, mean, p50, p95, p99, maximum, total or estimated exclusive CPU
  demand, calls per publication/delivery, path, thread role, and missing/invalid
  record counts.
- **FR-014**: The analyzer MUST separately report synchronous API duration,
  state-discovery delay, Mapping-availability delay, payload-availability delay,
  application delivery delay, timer deadline lateness, missed release slots,
  attempted rate, delivered rate, delivery/attempted ratio, piggyback ratio,
  Mapping fallback ratio, and Payload fallback ratio in both directions.
- **FR-015**: A bottleneck conclusion MUST distinguish mutually exclusive
  application-publication CPU, NDN-SVS Face/io_context CPU, timer/scheduler
  delay, and external/network wait even though the CPU work shares one thread;
  it MUST NOT add aggregate and child spans or describe one formal observation
  per rate as an inferential population result.
- **FR-016**: The final artifacts MUST include a rate-by-stage table, a grouped
  critical-path table, a ranked bottleneck table with evidence, a path-frequency
  table, instrumentation-overhead evidence, raw structured logs, and an explicit
  limitations section.
- **FR-017**: NDNSF runtime, UAV/codec logic, asynchronous/latest NDN-SVS code,
  Spec 132 formal execution, NFD internal profiling, and payload-size/topology/
  security-algorithm sweeps MUST remain outside Spec 133.

### Key Entities

- **ProfileSubject**: Exact base commit, Boost patch, profiling patch, source
  tree, binary/library hashes, build/linkage record, logging configuration, and
  verified absence of asynchronous/parallel behavior.
- **StageDefinition**: Stable stage identifier, operation unit, path, thread
  role, leaf/aggregate/wait/milestone kind, parent, and interpretation rule.
- **StageSpan**: One sampled structured duration bound to a cell, peer,
  correlation key, stage, start, end/duration, outcome, bytes, and counts.
- **StageSummary**: Exact calls plus valid sampled distribution and exclusive
  demand estimates for one stage/peer/rate.
- **ProfileCell**: One immutable rate, two peers, fixed timing/topology/payload,
  raw logs, summaries, and terminal receipt.
- **BottleneckFinding**: Ranked stage or wait with quantitative evidence,
  affected rate boundary, confidence/limitation, and alternative explanation.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Source and binary audits prove the formal subject is the exact
  synchronous pre-feature commit plus only the two declared patches, with no
  asynchronous or parallel-worker symbols reachable from the benchmark.
- **SC-002**: The instrumentation contract contains every stage listed in
  FR-006, and a smoke trace exercises at least one complete piggyback path plus
  contract fixtures for the Mapping- and Payload-fallback paths.
- **SC-003**: The three-arm clean/instrumented-disabled/instrumented-enabled
  admission satisfies every FR-009 perturbation bound before a formal manifest
  can be sealed.
- **SC-004**: The sealed formal manifest contains exactly five unique ascending
  cells and the campaign closes with exactly five immutable terminal receipts,
  including any negative subject outcome.
- **SC-005**: For every valid cell, exact top-level call counts reconcile, every
  valid sampled child is contained by its parent, no negative duration exists,
  and residual/unattributed time is reported rather than silently assigned.
- **SC-006**: The final report provides all FR-013/FR-014 metrics for both peers
  and both directions and identifies the first tested rate where attempted or
  delivered performance departs from the lower-rate trend.
- **SC-007**: Every named bottleneck is supported by at least two quantitative
  signals from demand/share, queue/wait growth, path-frequency change, and the
  observed rate boundary; unsupported cases are labeled inconclusive.
- **SC-008**: A reviewer can trace every table row back to the subject manifest,
  cell receipt, exact aggregate counter, and raw sampled diagnostic records.

## Assumptions

- Both peers share the host `CLOCK_MONOTONIC_RAW` clock across MiniNDN
  namespaces.
- The fixed security profile uses HMAC for Sync Interests and digest signing
  for Data, matching the existing synchronous benchmark; actual resolved
  signing and validation modes are recorded in each receipt.
- Deterministic one-in-100 correlation-key sampling is the initial formal rule;
  exact aggregate counters cover every operation. If the FR-009 gate fails,
  formal execution remains blocked and this sampling contract must be revised
  before any new campaign is sealed.
- The formal experiment intentionally uses a stricter model than the historical
  chat example: one Face/io_context execution thread per peer process. README
  and the public header are silent, tests do not cover concurrent publication,
  and the example's thread-safety claim is contradicted by sanitizer evidence.
- With one formal observation per rate, results support descriptive stage and
  rate-boundary analysis, not replication-based confidence intervals or
  population-level significance tests.
