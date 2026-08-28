# Feature Specification: Bidirectional NDN-SVS Capability Comparison

**Feature Branch**: `132-svs-pacer-isolation`

**Created**: 2026-07-22

**Status**: Draft

**Input**: Replace the invalid one-way adapter benchmark with a pure NDN-SVS
comparison in which both peers publish and receive concurrently. The synchronous
subject publishes directly from the application main thread and exposes its
sustainable ceiling; the asynchronous/parallel subject publishes directly at
controlled rates. Execute exactly five formal rates per subject.

## Frozen Capability Subjects

| Capability subject | Exact NDN-SVS commit | Required behavior |
|---|---|---|
| `sync-publish-no-internal-parallelism` | `a9944019f76791773604999f00128057b9534ace` | Application main thread calls `publish()` directly; Face runs on its I/O thread; no internal parallel worker configuration |
| `async-publish-parallel-sync` | `6bb34545b4f89f1f6c265a68c18f1a40ade413eb` | Application main thread calls `publishAsync()` directly; Face runs on its I/O thread; four receive and production workers are enabled |

The subjects are immutable commit bundles. The comparison MUST NOT attribute
the entire delta solely to one API or threading change because intervening
correctness changes remain part of the treatment commit.

## User Scenarios & Testing

### User Story 1 - Exercise Real Bidirectional PubSub Use (Priority: P1)

As an evaluator, I can run two equal peers that each publish local data and
simultaneously subscribe to the other peer, so the test exercises sending,
Sync processing, fetching, and delivery in both directions.

**Why this priority**: A dedicated publisher and subscriber do not represent
the requested PubSub API workload.

**Independent Test**: One cell launches Peer A and Peer B with the same subject
and target rate. Both event files contain API calls for local publications and
validated deliveries from the opposite peer.

**Acceptance Scenarios**:

1. **Given** a cell at rate R, **when** it runs, **then** Peer A and Peer B each
   target R publications/s and each receives only the other peer's stream.
2. **Given** a synchronous subject cell, **when** publications are generated,
   **then** the application main thread invokes `publish()` directly while the
   Face thread processes events concurrently.
3. **Given** an asynchronous subject cell, **when** publications are generated,
   **then** the application main thread invokes `publishAsync()` directly at
   absolute deadlines while the configured internal workers process Sync work.

### User Story 2 - Locate The Synchronous Sustainable Ceiling (Priority: P1)

As an evaluator, I can identify the highest tested bidirectional rate that the
synchronous subject sustains rather than mistaking fast local API returns for
successful PubSub delivery.

**Why this priority**: Synchronous local publication may outpace the Face I/O
path and remote fetching, producing a sharp delivery collapse.

**Independent Test**: Execute the five synchronous formal cells in ascending
order at 200, 400, 600, 800, and 1000 publications/s per peer. The highest cell
where both directions meet the sustainable-rate contract is the tested-grid
ceiling; failures and crashes remain evidence.

**Acceptance Scenarios**:

1. **Given** a synchronous target rate, **when** the main-thread call duration
   prevents exact pacing, **then** actual API-enter/API-return rates and deadline
   lateness are reported without inserting an adapter or skipping the call.
2. **Given** a rate where local attempts remain high but remote delivery
   collapses, **when** the cell is classified, **then** it is not sustainable.
3. **Given** a subject crash or memory-safety failure, **when** the cell closes,
   **then** it receives one immutable terminal failure receipt and the remaining
   independent cells continue without retrying it.

### User Story 3 - Compare Controlled Async/Parallel Rates (Priority: P2)

As an evaluator, I can run the same five bidirectional targets against the
asynchronous/parallel capability subject and compare direction-specific
attempted rate, delivery ratio, and delay with the synchronous subject.

**Independent Test**: After all five synchronous receipts exist, execute five
asynchronous cells at the identical rates and produce a table with one direct
subject pair at every rate.

**Acceptance Scenarios**:

1. **Given** the asynchronous subject, **when** a cell completes, **then** each
   peer's attempted rate is checked independently against the target with a
   +/-2% tolerance.
2. **Given** a completed matrix, **when** results are summarized, **then** A-to-B
   and B-to-A metrics are reported separately and aggregate PPS is explicitly
   twice the per-peer target.
3. **Given** a sender-limited, crashed, missing, or invalid cell, **when** the
   matrix is summarized, **then** the negative outcome remains visible and no
   selective replacement is permitted.

### Edge Cases

- A peer can return from the publication API while the opposite direction is
  already backlogged; sustainable capacity therefore requires remote delivery.
- `publishAsync()` returns after preparation/staging, not after remote delivery;
  API-return latency and end-to-end delay are distinct.
- Concurrent synchronous `publish()` and Face processing may reveal a subject
  defect. The harness records it rather than moving the API call onto Face.
- A cell may be asymmetric even on a symmetric topology; both directions must
  pass independently.
- Late deadlines are recorded. The harness must not silently discard planned
  publications or add a catch-up queue.

## Requirements

### Functional Requirements

- **FR-001**: Every cell MUST launch exactly two equal bidirectional PubSub
  peers using the same subject and target rate.
- **FR-002**: Rates 200, 400, 600, 800, and 1000 MUST mean publications per
  second per peer; aggregate offered load MUST be reported as twice that value.
- **FR-003**: Each peer MUST run Face event processing on one I/O thread and
  invoke its publication API directly from the application main thread.
- **FR-004**: The synchronous subject MUST call `publish()` and MUST NOT use an
  eventfd, adapter queue, Asio post, scheduler-generated offered load, or an
  internal parallel-worker API.
- **FR-005**: The asynchronous subject MUST call `publishAsync()` and MUST
  explicitly enable four receive workers and four production workers.
- **FR-006**: Each peer MUST publish and subscribe concurrently for a 10-second
  warmup, 60-second measured window, and bounded 10-second drain.
- **FR-007**: The harness MUST record planned deadlines, API entry, successful
  API return, API error, peer delivery, duplicates, invalid payloads, and state
  updates using a shared host monotonic clock.
- **FR-008**: Publication generation MUST stop at the measurement boundary;
  overdue work MUST NOT be queued for execution after the window.
- **FR-009**: The analyzer MUST report metrics separately for A-to-B and B-to-A:
  API-entered, API-returned, delivered, missing, delivery/returned, attempted
  rate, deadline lateness, API duration, and delivery delay p50/p95/p99/max.
- **FR-010**: A synchronous rate is sustainable only when both peers return at
  least 95% of the per-peer target, both directions deliver at least 99% of
  successful returns, payloads are valid, and neither peer fails.
- **FR-011**: An asynchronous cell is rate-valid only when both peers' returned
  rates are within +/-2% of target. Delivery remains a separate outcome.
- **FR-012**: The formal matrix MUST contain exactly 10 unique once-only cells:
  five synchronous cells first in ascending rate, followed by five asynchronous
  cells in the same order.
- **FR-013**: Every formal cell MUST have exactly one terminal receipt; no
  automatic retry, selective rerun, rate tuning, or overwritten output is
  permitted after sealing.
- **FR-014**: Both subjects MUST use separate immutable worktrees and binaries,
  the same byte-identical Boost 1.71 build-only patch, and recorded source,
  library, binary, and command hashes.
- **FR-015**: Spec 131 formal evidence MUST remain frozen. The failed
  `treatment-1000-full60-20260722a` Spec 132 adapter run MUST remain labeled
  non-admissible harness evidence and MUST NOT enter the formal comparison.
- **FR-016**: NDNSF runtime code, UAV logic, codecs, and application-specific
  framework behavior MUST remain outside the experiment.

### Key Entities

- **CapabilitySubject**: Exact commit, API, internal-worker configuration, build
  patch identity, library identity, and binary identity.
- **BidirectionalCell**: One subject, one per-peer target, two peer streams, one
  attempt, and one terminal receipt.
- **DirectionSummary**: Sender API events joined with the opposite peer's
  delivery events for one direction.
- **RateComparison**: Direct synchronous/asynchronous pair at one per-peer rate.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Source tests prove both peers subscribe and publish, both APIs are
  called directly from the main thread, and no harness publication queue or
  Asio post remains.
- **SC-002**: The sealed manifest contains exactly 10 cells in the required
  subject/rate order, with 10 distinct paths and attempt number 1.
- **SC-003**: Every completed cell accounts for both peer streams without
  unexpected sender IDs or invalid payloads.
- **SC-004**: The final table reports both directions and identifies the
  synchronous tested-grid ceiling using the sustainable-rate contract.
- **SC-005**: All 10 terminal outcomes are preserved, including subject
  failures and sender-limited cells; no result is selectively replaced.
- **SC-006**: Spec 131 evidence and the non-admissible failed adapter diagnostic
  remain unchanged and excluded from formal aggregation.

## Assumptions

- Both MiniNDN peers share the host `CLOCK_MONOTONIC_RAW` time base.
- A target rate is per peer. Thus a 1000 cell offers up to 2000 aggregate
  publications/s.
- The fixed five-rate grid provides the requested tested ceiling; no extra
  formal capacity-search cells are added.

