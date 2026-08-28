# Feature Specification: NDN-SVS PubSub Commit-Latency Comparison

**Feature Branch**: `131-svs-pubsub-commit-latency`

**Created**: 2026-07-21

**Status**: Closed; corrected 10-cell campaign complete, prior 50-cell campaign diagnostic-only

**Input**: Compare pure NDN-SVS PubSub synchronization delay at 200, 400,
600, 800, and 1000 publications per second between the last commit before the
asynchronous/multithread work and the latest commit. Run the old commit first,
then the latest commit. Do not involve the NDNSF runtime.

## Scope Boundary

This feature is an NDN-SVS benchmark, not an NDNSF service benchmark. The
experiment harness may be owned by this repository, but benchmark processes
MUST link only to NDN-SVS, ndn-cxx, and their normal system dependencies. It
MUST NOT start, link, import, or exchange NDNSF Request, ACK, Selection,
Response, permission, NAC-ABE, DI, Repo, UAV, or stream messages.

The two subjects are fixed as follows:

| Subject | Commit | Required behavior |
|---|---|---|
| `baseline-sync-serial` | `a9944019f76791773604999f00128057b9534ace` | Last commit before receive parallelization and ordered asynchronous PubSub publication; use synchronous `SVSPubSub::publish()` |
| `latest-async-parallel` | `6bb34545b4f89f1f6c265a68c18f1a40ade413eb` | Current latest commit; use `SVSPubSub::publishAsync()` and explicitly enable parallel Sync receive and production |

The comparison measures the complete old-versus-latest version bundle. It
MUST NOT be presented as a strict causal estimate of asynchronous publication
or multithreading alone because the commits between the subjects also include
Interest signing, V3 protocol, mapping recovery, atomic publication, and
bounded fetch/repair changes. To reduce avoidable protocol confounding, the
latest subject runs the V2 wire profile and the same V2 timers as the baseline.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Establish a Fair Pure-PubSub Benchmark (Priority: P1)

As an NDN-SVS developer, I can build an external PubSub publisher/subscriber
pair against either pinned commit and know that the two variants differ only
at explicitly declared compatibility and treatment boundaries.

**Why this priority**: A commit comparison is meaningless if it silently uses
different topologies, wire profiles, payloads, timers, routes, clocks, or an
NDNSF wrapper.

**Independent Test**: Build both temporary local branches from the exact pinned
commits with the same allowlisted Boost 1.71 build-only patch, run the driver
self-test and one non-formal 1000 publications/s admission smoke per subject, then inspect
the source/patch and binary dependency manifests and cell configuration.

**Acceptance Scenarios**:

1. **Given** the pinned baseline commit, **when** its driver is built, **then**
   it uses public `SVSPubSub::publish()` and contains no async/parallel API
   requirement.
2. **Given** the pinned latest commit, **when** its driver is built, **then** it
   uses public `publishAsync()` and enables the Sync receive and production
   worker pools with fixed parameters.
3. **Given** either binary, **when** its dependency and process manifests are
   audited, **then** no NDNSF library, binary, Python module, wire namespace, or
   controller appears.

---

### User Story 2 - Measure the Old Commit First (Priority: P1)

As an evaluator, I can execute the complete old-commit baseline at all five
offered rates before any latest-commit formal cell starts.

**Why this priority**: The requested temporal order is part of the experiment
contract and prevents an incomplete comparison from being reported as final.

**Independent Test**: A frozen campaign manifest admits exactly five fresh
baseline cells, one at each of 200, 400, 600, 800, and 1000 publications/s,
with a 60-second measured window in each cell and no treatment cell present.

**Acceptance Scenarios**:

1. **Given** a valid preflight, **when** the baseline block runs, **then** all 5
   baseline cells execute once before the treatment block is admitted.
2. **Given** a publication scheduled in the measured window, **when** it is
   delivered, missing, duplicated, or delivered out of order, **then** the
   outcome is accounted against its logical message identifier and SVS
   sequence number.
3. **Given** a sender that cannot sustain an offered rate, **when** its achieved
   rate differs by more than 2%, **then** the cell is retained as
   sender-limited rather than discarded or silently relabeled.

---

### User Story 3 - Measure and Compare the Latest Commit (Priority: P1)

As an evaluator, I can run the same five-rate matrix against the latest
async/parallel configuration and obtain an honest old-versus-latest latency,
delivery, saturation, and resource comparison.

**Why this priority**: The result must show whether the latest version improves
PubSub delivery under increasing publication load without hiding missing
publications or queue drops.

**Independent Test**: After the baseline block closes, execute 5 treatment
cells from the same immutable manifest and generate one direct old/new
comparison at each requested rate.

**Acceptance Scenarios**:

1. **Given** a matching rate, **when** both version cells close,
   **then** the analysis reports delivered-only p50/p95/p99 PubSub delay,
   state-update delay, delivery ratio, achieved rate, duplicates, reordering,
   and deadline-capped tail delay.
2. **Given** missing publications or worker queue drops, **when** results are
   summarized, **then** missing items remain visible and the latest version
   cannot be declared faster from the surviving callbacks alone.
3. **Given** one cell per subject-rate and sequential version blocks, **when**
   the comparison is interpreted, **then** it reports descriptive direct
   differences and labels the conclusion as version-bundle evidence rather
   than a causal feature attribution or statistical confidence claim.

### Edge Cases

- A dirty, missing, or non-resolving pinned NDN-SVS commit stops candidate
  creation before either formal block.
- A baseline cell that cannot reach its offered rate or loses publications is
  valid negative performance evidence, not an excuse to tune or replace it.
- A treatment worker queue overflow is recorded separately from NFD/network
  loss; unavailable baseline-only counters are `null`, never zero.
- Duplicate subscriber callbacks do not compensate for a missing publication.
- Out-of-order callbacks are accepted as deliveries but counted and analyzed
  separately.
- A publication received after the measured window but within the drain window
  is included with its full delay; one still missing at the drain deadline is
  censored at that deadline.
- Wall-clock adjustment does not alter latency because publisher and
  subscriber use the same host kernel's monotonic raw clock across MiniNDN
  namespaces.
- Per-message logging must not flush synchronously in the hot path; buffered
  events are written in batches or at shutdown.
- An infrastructure failure during a formal cell is retained. No selective
  automatic retry or cell replacement is allowed inside that campaign ID.
- An incomplete old block prevents the latest block from starting and prevents
  any final old-versus-latest claim.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The benchmark MUST exercise public NDN-SVS `SVSPubSub` APIs over
  MiniNDN/NFD and MUST NOT execute or link NDNSF runtime behavior.
- **FR-002**: The baseline subject MUST be exactly
  `a9944019f76791773604999f00128057b9534ace`, and the treatment subject MUST be
  exactly `6bb34545b4f89f1f6c265a68c18f1a40ade413eb`.
- **FR-003**: Candidate creation MUST create separate temporary local build
  branches/worktrees rooted at the two exact pinned commits and apply the same
  allowlisted `wscript` compatibility patch only: change the minimum-version
  guard from `107400` to `107100` and its diagnostic from `1.74.0` to `1.71.0`.
  Each build worktree MUST be clean after that single build-only commit; the
  manifest MUST record the base commit/tree, temporary branch/head/tree, exact
  patch bytes and SHA-256, Boost headers/libraries/version, configure/build
  commands, library/binary hashes, and `ldd` evidence. Any additional source
  difference is forbidden. The temporary commits MUST NOT be merged, rebased,
  pushed, or used to move the active NDN-SVS branches, and both subjects MUST
  use byte-identical compatibility patches.
- **FR-004**: The formal campaign MUST execute the complete baseline block
  before starting any treatment cell.
- **FR-005**: The baseline driver MUST call synchronous `publish()`; the
  treatment driver MUST call `publishAsync()` and explicitly enable parallel
  Sync receive and production with four workers and a queue limit of 4096 on
  both peers. Sync batching MUST remain disabled.
- **FR-006**: Both subjects MUST use the SVS V2 wire profile, 1 ms Sync Interest
  lifetime, 500 ms suppression period, 30 s periodic interval with 10% jitter,
  `useTimestamp=true`, `maxPubAge=0`, and otherwise matched common options.
- **FR-007**: The matrix MUST contain exactly one cell at each offered rate
  200, 400, 600, 800, and 1000 publications/s for each subject, totaling 10
  formal cells: five baseline cells followed by five treatment cells.
- **FR-008**: Every cell MUST use a fresh two-host MiniNDN topology with one
  publisher, one subscriber, one NFD per namespace, a direct 100 Mbps link,
  10 ms one-way delay, 0% configured loss, multicast strategy for the unique
  Sync prefix, and only the required inter-host routes for the Sync prefix and
  the two stable peer node prefixes. It MUST NOT inject a route for a concrete
  application publication name or a transient local application face.
- **FR-009**: Each cell MUST start the subscriber first, allow a bounded
  convergence interval, run a 10-second warmup, a 60-second measured window,
  and a 10-second drain, and use a unique group/node namespace.
- **FR-010**: Publications MUST use a deterministic 256-byte opaque payload
  containing a schema marker, logical message identifier, phase, scheduled
  publication time, and reproducible filler bytes; payload content and size
  MUST be identical across subjects.
- **FR-011**: The Face event loop MUST run on its own thread. A separate
  high-resolution publisher pacer thread MUST use absolute deadlines and call
  a thread-safe publication adapter directly. The adapter MUST use
  `io_context::post`, record acceptance as `attempted`, and execute the pinned
  subject's `publish()`/`publishAsync()` on the Face thread because ASan proves
  the historical scheduler cancel path corrupts memory under direct concurrent
  API entry. The Face scheduler MUST NOT generate offered load. The pacer MUST
  NOT run an unbounded catch-up loop; a slot more than two periods late MUST be
  skipped and counted, and every actual wake time MUST be recorded.
  The pacer MUST be pinned to logical CPU 0, the Face thread to logical CPU 1,
  and the pacer MUST request `SCHED_FIFO` priority 1 plus a bounded 50 us final
  spin; the result of each affinity/priority request MUST be recorded.
- **FR-012**: The primary endpoint MUST be publication-to-subscription delay
  from the publisher's scheduled/API-entry timestamp to the subscriber's
  `SubscriptionCallback` for the same logical item. State-update delay from
  that timestamp to the first covering NDN-SVS update callback MUST be reported
  separately.
- **FR-013**: Per-message evidence MUST bind logical item, returned SVS
  sequence, scheduled time, API entry/return, state-update time when observed,
  subscription callback time, phase, duplicate status, and order status.
- **FR-014**: Every cell MUST separately report scheduled, attempted, and
  API-completed, and delivered counts; attempted/scheduled and
  delivered/attempted ratios;
  offered and attempted rate; emitted,
  delivered, missing, duplicate, and out-of-order counts; p50/p95/p99 and max
  delivered delay; deadline-capped p95/p99; publisher API-call time; CPU, RSS,
  and network packet counters.
- **FR-015**: Treatment cells MUST additionally report receive/production jobs
  submitted, completed, dropped, stale, queue depth, worker processing time,
  and parallel total time from NDN-SVS. Fields unavailable on the baseline MUST
  be explicit `null` values.
- **FR-016**: The runner MUST avoid per-publication synchronous file or console
  I/O, preserve raw buffered event rows, process logs, NFD status, route state,
  `tc` state, dependency manifests, and source/build identities.
- **FR-017**: The formal manifest MUST freeze cell IDs, version order, a
  deterministic matched rate order, rates, duration, payload,
  topology, CPU assignments, worker settings, timeout, analysis schema, and
  `automaticRetry=false` before the first baseline cell.
- **FR-018**: Each formal cell MUST execute at most once under its campaign ID.
  An invalid or failed cell remains in the summary; a corrected campaign
  requires a new campaign ID and a complete new 10-cell matrix.
- **FR-019**: Analysis MUST retain delivered-only latency and delivery ratio
  together, classify cells outside ±2% achieved-rate tolerance as
  sender-limited, and prevent an improvement claim when delivery is worse or
  missing observations are hidden.
- **FR-020**: Per-rate comparison MUST report the direct old/new deltas and
  ratios for the matching cells. It MUST use
  `improved`, `regressed`, or `inconclusive` language without attributing the
  result solely to async publication or multithreading.
- **FR-021**: A non-formal build/self-test/1000 publications/s smoke for both
  subjects MUST achieve attempted rate within ±2% before the immutable formal
  campaign is admitted.
- **FR-022**: The implementation MUST include automated contract tests for
  manifest ordering, exact matrix cardinality, rate pacing, event accounting,
  percentile/censoring behavior, explicit unavailable metrics, no-retry
  enforcement, and NDNSF dependency exclusion.

### Key Entities

- **Version Subject**: Label, pinned commit/tree, source worktree, build flags,
  binary/library hashes, API mode, Sync profile, worker configuration, and
  dependency audit.
- **Campaign Manifest**: Immutable experiment identity, subject order, matched
  cell schedule, common controls, topology, CPU allocation, analysis version,
  and retry policy.
- **Benchmark Cell**: One subject, rate, repetition, namespace, timing windows,
  process identities, network state, raw events, counters, and terminal status.
- **Publication Observation**: One logical item and its publisher, Sync-update,
  subscriber, delivery, duplication, ordering, and censoring timestamps.
- **Comparison Summary**: Per-cell metrics, per-rate direct subject
  observations, saturation classification, and claim boundary; it contains no
  replication-based uncertainty interval.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Candidate evidence resolves both exact base commits, proves each
  clean temporary build branch differs only by the same hashed Boost 1.71
  `wscript` patch, confirms all linked Boost libraries are 1.71 with no 1.74
  residue, and proves both benchmark binaries have zero NDNSF runtime
  dependencies.
- **SC-002**: The frozen manifest contains exactly 10 unique formal cells: five
  baseline cells followed by five treatment cells, one at each requested rate,
  with `automaticRetry=false`.
- **SC-003**: Every started formal publication is accounted as exactly one
  first delivery, missing, and/or additional duplicate observation; aggregate
  counts reconcile mechanically with raw events.
- **SC-004**: Every cell summary reports the required delay percentiles,
  delivery/order/rate/resource metrics, explicit unavailable fields, source
  identity, and terminal classification without unclassified errors.
- **SC-005**: Every valid-rate cell achieves its configured rate within ±2%; a
  cell outside that range remains visible and is excluded only from a
  sustained-rate performance claim, not from the campaign report.
- **SC-006**: The final analysis contains one side-by-side direct comparison at
  each of all five rates and a highest-sustained-rate classification for each
  subject; it does not claim statistical confidence from unreplicated cells.
- **SC-007**: No claim of lower delay is made at a rate where the treatment has
  worse delivery, hidden missing items, or only faster surviving callbacks.
- **SC-008**: The formal campaign either closes with 10/10 once-only cell
  receipts or is explicitly `INCOMPLETE`; no partial, selectively rerun matrix
  is reported as the requested comparison.
- **SC-009**: The final report states that the result compares pinned version
  bundles and separately lists all intervening non-async/non-thread commits.

## Assumptions

- MiniNDN namespaces share one Linux kernel, so `CLOCK_MONOTONIC_RAW` values are
  directly comparable across the publisher and subscriber processes. This
  assumption must be checked by a preflight clock probe.
- The validated evaluation process is restricted to logical CPUs 0--3. The
  manifest pins the same complete four-CPU affinity set for publisher,
  subscriber, NFD, and orchestration in both subjects. The treatment retains
  four receive and four production workers even though the complete cell is
  CPU-contended; the final report MUST describe this as a shared four-core
  system comparison, not a per-peer dedicated-core result. Any affinity change
  after manifest sealing blocks continuation.
- One cell per subject-rate implements the requested direct 10-cell comparison;
  no inferential uncertainty claim is made.
- The 256-byte payload intentionally avoids segmentation and focuses this Spec
  on small-item PubSub synchronization. Large/segmented payload performance is
  a separate experiment.
- The latest `publishAsync()` API defers ordered advertisement/commit work; the
  benchmark does not assume that every preparation/signing operation occurs on
  a worker thread.
- Running all baseline cells before all treatment cells follows the requested
  order but creates temporal drift risk. The runner therefore uses the same
  deterministic within-block schedule and records per-process CPU/RSS plus
  link counters for both blocks. Host frequency and temperature were not
  captured, so residual temporal/thermal drift remains an explicit limitation.
