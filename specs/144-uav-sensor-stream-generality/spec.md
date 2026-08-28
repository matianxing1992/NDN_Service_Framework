# Feature Specification: UAV Sensor Stream Generality

**Feature Branch**: `[144-uav-sensor-stream-generality]`

**Created**: 2026-07-24

**Status**: Formal freeze approved; immutable after campaign start

**Input**: User description: "Define a fresh generality-validation Spec without
rerunning or modifying Specs 127/128. Validate a 20 Hz, 256-512 byte,
single-item, no-FEC UAV telemetry stream and a short-block, 2-4 source,
two-repair acoustic/audio stream. Report delivery, AoI/end-to-end
mean/p50/p95/p99, longest gap, future hits, Mapping/Payload Interests, retry,
timeout, Nack, recovery, and useless-Interest ratios. Audit Core for the
absence of UAV, telemetry, audio, codec, or workload special cases."

## Context and Claim Boundary

Spec 125 established that the generic live-stream and adaptive prefetch API is
useful for encrypted variable-extent UAV video. Spec 127 then withheld the
cross-application claim, and Spec 128 preserved a negative 2/5 result for the
periodic one-item impaired boundary while establishing 5/5 recovery for its
variable multisegment workload.

Spec 145 subsequently corrected the UAV Video APP integration and passed one
fresh 20-fps, 60-second, zero-loss MiniNDN acceptance cell. That corrected path
is the formal APP-side reference implementation for session-frozen
announcement/publication truth, existing Core `AdaptiveSampleAtomic`
consumption, exact-name/security admission, callback failure containment, and
generation-fenced display of actual Core state. It is not a reusable
video/codec workload template: telemetry and acoustic/audio retain their own
class, extent, protection, cadence, and payload semantics.

This feature does not reopen, repair, tune, or rerun either historical
campaign. It creates two fresh UAV application workloads that exercise the
same generic Streaming/Mapping v2 surface. A shared generality claim is allowed
only if both workload families independently meet their preregistered gates.
A failed formal cell is evidence, not permission to change a threshold or run
that cell again.

The two workload semantics are intentionally distinct:

- **Telemetry** is a latest-state stream. It reports complete-delivery metrics,
  but its primary latency outcome is Age of Information (AoI), and a consumer
  must never regress to an older accepted state.
- **Acoustic/audio** is a continuous-media stream. Its unit is one short
  playout block, and its primary outcomes are complete-block delivery,
  capture-to-delivery latency, longest inter-block gap, and bounded two-erasure
  recovery.

This Spec validates transport and prefetch behavior. It does not validate
microphone hardware, acoustic fidelity, a particular audio codec, human
perception, physical wireless performance, or population-wide reliability.

## User Scenarios & Testing

### User Story 1 - Fresh UAV Telemetry Stream (Priority: P1)

As a Ground Station operator, I want compact UAV telemetry to arrive
continuously at 20 Hz so that position, motion, battery, readiness, and link
state remain fresh without polling a full status snapshot every 1.5 seconds.

**Why this priority**: This reuses data that UAV-APP already produces and
directly targets the periodic single-item/no-FEC boundary that remained
negative in Spec 128. It introduces no microphone or codec confound.

**Independent Test**: Run only the telemetry workload with a deterministic
compact payload schedule and verify 1,200 measured samples, monotonic accepted
sample IDs, byte identity, AoI distribution, longest gap, and complete
Interest/retry attribution.

**Acceptance Scenarios**:

1. **Given** a 20 Hz provider and a latest-start consumer on the zero-loss
   two-node topology, **when** the 60-second measured window completes, **then**
   the consumer receives all 1,200 unique samples in order and reports the
   preregistered AoI and prefetch-utility metrics.
2. **Given** a valid signed Mapping that announces future one-item samples,
   **when** payload production follows the deterministic 256/384/512-byte
   cycle, **then** every accepted sample is bound to its declared provider,
   stream session, sample ID, name, and exact bytes.
3. **Given** an impaired cell, **when** one payload attempt times out or is
   Nacked, **then** retry remains exact-name, bounded, observable, and unable
   to make the application state regress.
4. **Given** the live stream is unavailable or stopped, **when** the Ground
   Station needs a complete current snapshot, **then** the existing
   request/response `GetStatus` path remains available and unchanged.

---

### User Story 2 - Short-Block Acoustic/Audio Stream (Priority: P2)

As a UAV application developer, I want opaque short acoustic/audio blocks to
use the same streaming and prefetch API so that continuous multisegment sensor
data can be delivered with bounded two-erasure protection and without any
audio- or codec-specific Core behavior.

**Why this priority**: This independently validates a continuous-media shape
with variable 2-4 item extents and two repair symbols. It complements, rather
than duplicates, the fixed one-item telemetry workload.

**Independent Test**: Run only the acoustic/audio workload using deterministic
40 ms blocks whose source-item counts cycle 2/3/4. Verify 1,500 complete blocks,
exact source bytes after direct or recovered delivery, repair provenance,
capture-to-delivery latency, longest gap, and Interest utility.

**Acceptance Scenarios**:

1. **Given** an application-defined acoustic block and the exact APP-owned
   extent class for two, three, or four opaque source items, **when** the
   provider announces and publishes the block, **then** the consumer delivers the
   complete block exactly once or reports an explicit terminal skip.
2. **Given** no more than two missing source items in a block, **when** two
   valid repair symbols are available within the recovery budget, **then** the
   original source bytes are reconstructed and marked as recovered.
3. **Given** more than the declared recovery capacity, malformed repair data,
   a wrong signer, a stale session, or a name-binding mismatch, **when**
   recovery is attempted, **then** the block fails closed and later blocks can
   continue.
4. **Given** a zero-loss cell, **when** repair Data is fetched but not consumed
   for reconstruction, **then** the analyzer reports it as protection cost
   rather than silently labeling it useful application delivery.

---

### User Story 3 - Trustworthy Cross-Application Evidence (Priority: P3)

As an NDNSF evaluator, I want a fresh, one-shot MiniNDN matrix and an auditable
neutrality report so that I can determine whether both application workloads
support a bounded generality claim without changing historical evidence or
adding application-specialized Core branches.

**Why this priority**: Performance numbers are meaningful only when workload,
fault profile, source identity, metric definitions, and rerun policy are fixed
before observation.

**Independent Test**: Verify a 32-row campaign manifest containing exactly one
invocation for every preregistered cell, complete raw-to-summary metric
traceability, unchanged Spec 127/128 digests, and a CodeGraph/source audit of
all Core changes.

**Acceptance Scenarios**:

1. **Given** the two workload families and four frozen network profiles,
   **when** the formal campaign starts, **then** telemetry executes 16 unique
   cells and acoustic/audio executes 16 unique cells, with no automatic or
   selective rerun.
2. **Given** a crashed, timed-out, incomplete, or failed formal cell, **when**
   the campaign is summarized, **then** that cell remains in the denominator
   and is never replaced by a diagnostic run.
3. **Given** source changes under generic Core or bindings, **when** neutrality
   is audited, **then** every changed decision branch is shown to depend only
   on generic stream state, Mapping, timing, extent, retry, or recovery
   information.
4. **Given** either workload family misses any controlling treatment-level
   gate, **when** the final verdict is produced, **then** the shared generality
   claim is withheld and the measured negative result is preserved.

### Edge Cases

- A consumer joins after the stream begins or reconnects after retention has
  advanced.
- The provider announces a sample but stops before producing it.
- A telemetry sample arrives after a newer sample has already been admitted.
- The deterministic telemetry payload is shorter, longer, duplicated, or
  bound to the wrong sample ID.
- An acoustic block's actual extent is smaller than predicted, reaches the
  four-source limit, or exceeds it.
- Zero, one, two, or more than two source items are missing in one block.
- Mapping Data validates out of order while payload Data and retries are also
  in flight.
- A retry Data packet arrives after its deadline or after the same item was
  recovered by FEC.
- Repair Data is fetched in a zero-loss cell but never needed for recovery.
- The stream session changes while old Mapping, payload, or repair Data remains
  cached.
- MiniNDN qdisc installation, application readiness, measured-window sample
  count, process ownership, or clock-domain validation is incomplete.

## Requirements

### Functional Requirements

- **FR-001**: The feature MUST treat
  `specs/127-cross-application-stream-generality` and
  `specs/128-generic-multiloss-recovery`, together with their canonical
  results, as immutable baseline evidence and MUST verify their before/after
  digests without executing their runners.
- **FR-002**: Both new workloads MUST use the current generic Mapping v2
  live-stream lifecycle: announce future samples, resolve authenticated actual
  extent when necessary, publish opaque protected source bytes, and consume by
  exact mapped names under the adaptive sample-atomic policy. Their APP
  integration MUST be reviewed against the corrected Spec 145 UAV Video
  reference while reusing only generic lifecycle, security, callback-safety,
  and truthful-Core-status patterns; it MUST NOT copy video class names,
  key/delta, FPS/GOP, codec, or payload logic.
- **FR-003**: The telemetry workload MUST publish at 20 Hz for a 60-second
  measured window, producing exactly 1,200 sample IDs whose encoded payload
  sizes follow a deterministic 256/384/512-byte cycle.
- **FR-004**: Every telemetry sample MUST contain exactly one source item and
  declare no FEC repair item.
- **FR-005**: The telemetry payload MUST represent a compact subset of existing
  UAV state, including identity, sample time, position/motion, battery,
  readiness, and link information, while the existing complete `GetStatus`
  request/response service remains unchanged as a snapshot/fallback path.
- **FR-006**: The acoustic/audio workload MUST publish one block every 40 ms
  for a 60-second measured window, producing exactly 1,500 block IDs whose
  source-item counts follow a deterministic 2/3/4 cycle.
- **FR-007**: Each acoustic/audio source item MUST be opaque to Core, bounded at
  512 encoded bytes, and protected by the generic two-repair scheme with a
  declared recovery capacity of two erasures.
- **FR-008**: Codec choice, capture device, audio interpretation, playout,
  concealment, telemetry interpretation, and application encryption MUST
  remain application responsibilities; Core MUST operate only on opaque bytes
  and generic stream metadata.
- **FR-009**: Both consumers MUST start at the latest safe join point, use the
  generic adaptive sample-atomic prefetch policy, and permit Core—not the
  application—to choose the runtime fetch window and lookahead within declared
  resource bounds.
- **FR-010**: Signed Mapping, provider identity, stream/session epoch, Mapping
  version, exact Data name, item kind/index, and source/repair extent MUST be
  validated before application admission; invalid or stale data MUST fail
  closed.
- **FR-011**: The application payload protection path MUST bind ciphertext to
  the stream/session and exact mapped Data identity. No plaintext sensitive
  telemetry or audio content may be introduced into Mapping or diagnostic
  logs.
- **FR-012**: The analyzer MUST report, separately for every node and formal
  cell: attempted and produced samples/blocks, unique delivered samples,
  complete delivered blocks, delivery ratio, duplicate/invalid/out-of-order
  counts, AoI or end-to-end mean/p50/p95/p99/max, and the longest accepted-item
  gap.
- **FR-013**: The analyzer MUST separately report Mapping Interests, validated
  Mapping Data, Mapping Data that returns new resolver information, Mapping
  novelty ratio, Payload Interests, initial/retry Payload Interests, provider-
  confirmed future Interests/hits, and future-hit ratio.
- **FR-014**: The analyzer MUST separately report retry attempts, retry
  successes, retry suppressions and reasons, timeout, Nack, late arrival,
  deadline skip, retry exhaustion, recovery attempt/success/exhaustion, and
  recovery provenance.
- **FR-015**: Interest utility MUST distinguish at least three disjoint
  outcomes: application-useful source delivery or consumed recovery,
  protection-only repair Data fetched but not consumed, and nonproductive
  Interest attempts that return no newly admissible source/repair information.
  It MUST report the nonproductive-Interest ratio, protection-only ratio, and
  combined non-application Interest ratio; repair Interests MUST NOT be hidden
  inside a generic "useful" count.
- **FR-016**: The formal network matrix MUST use the two-node MiniNDN topology,
  a 60-second measured window, and these four fixed profiles independently for
  each workload: zero-loss once; 1% iid loss five times; reorder-only five
  times with 20 ms added delay, 10 ms normal jitter, 25% reorder, 50%
  correlation, and gap 5; and the combined loss+reorder profile five times.
- **FR-017**: Every formal cell MUST use a unique output directory and one
  launcher/cleanup owner, record exact command/source/binary/config identities
  and qdisc state, and execute exactly once with no automatic retry.
- **FR-018**: Diagnostic runs MUST use separate destinations and MUST NOT count
  as formal repetitions. After formal execution begins, no threshold, workload
  schedule, fault profile, binary, or analyzer interpretation may change.
- **FR-019**: Core and binding changes MUST pass a CodeGraph plus scoped source
  audit proving that no decision depends on UAV identity, telemetry fields,
  audio/acoustic names, codec identity, workload name, or formal cell identity.
  Application literals are allowed only in UAV application, experiment,
  fixture, and documentation paths.
- **FR-020**: A reproducible application-independent Core defect discovered
  before formal execution MAY be corrected only with deterministic generic
  tests and a renewed pre-implementation audit. A failure observed in the
  formal matrix MUST be preserved and deferred to a new Spec.
- **FR-021**: Full native build, forced Python binding rebuild, full relevant
  unit suites, focused streaming/security tests, runner/parser tests, metric
  conservation checks, and pre-implementation audit MUST pass before any
  formal cell is admitted.
- **FR-022**: The final report MUST issue independent telemetry and
  acoustic/audio verdicts and MUST withhold the shared generality claim unless
  both workload families satisfy every controlling gate.

### Key Entities

- **TelemetrySample**: One compact latest-state observation with sample ID,
  source timestamp, UAV identity, position/motion, battery, readiness, link
  state, deterministic encoded size, and protected opaque bytes.
- **AcousticBlock**: One 40 ms continuous-media unit with block ID, capture
  timestamp, actual source count, exact source bytes, two repair symbols, and
  delivery/recovery provenance.
- **StreamWorkloadDefinition**: Immutable workload identity, cadence, measured
  count, payload schedule, source bounds, repair rule, retention, Mapping
  settings, and application semantics.
- **FormalCell**: Workload/profile/repetition tuple with unique invocation,
  source/binary/config identities, qdisc evidence, process ownership, terminal
  state, and output path.
- **InterestOutcome**: One Mapping or Payload Interest attempt joined to its
  Data, Nack, timeout, duplicate, late, retry, repair-consumption, or terminal
  outcome.
- **LatencyObservation**: One sample/block measurement with a declared clock
  domain, origin event, terminal event, duration, and inclusion/exclusion
  reason.
- **WorkloadVerdict**: Treatment-level accepted count, exact interval, failed
  gates, and positive/negative conclusion for one workload family.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Deterministic and security tests deliver no duplicate, partial,
  out-of-order, wrong-provider, stale-session, wrong-name, malformed-repair, or
  post-stop application mutation for either workload.
- **SC-002**: Telemetry zero-loss accepts 1/1 with 1,200/1,200 unique samples,
  zero state regression, AoI p95 no greater than 100 ms, AoI p99 no greater
  than 150 ms, and longest accepted-sample gap no greater than 100 ms.
- **SC-003**: Telemetry impaired profiles each accept at least 4/5 repetitions;
  every accepted repetition delivers at least 99.9% of measured samples, has
  AoI p95 no greater than 200 ms, AoI p99 no greater than 300 ms, and has no
  accepted-sample gap greater than 250 ms.
- **SC-004**: Acoustic/audio zero-loss accepts 1/1 with 1,500/1,500 complete
  blocks, exact source bytes, end-to-end p95 no greater than 150 ms, p99 no
  greater than 250 ms, and longest complete-block gap no greater than 160 ms.
- **SC-005**: Acoustic/audio impaired profiles each accept at least 4/5
  repetitions; every accepted repetition completes at least 99.9% of measured
  blocks, has end-to-end p95 no greater than 250 ms, p99 no greater than
  400 ms, and has no complete-block gap greater than 320 ms.
- **SC-006**: Provider-confirmed future-hit ratio is at least 99% for telemetry
  zero-loss, at least 95% for every accepted telemetry impaired repetition, and
  at least 95% for every accepted acoustic/audio repetition.
- **SC-007**: Mapping novelty is at least 99% in every accepted run;
  nonproductive Payload Interest ratio is no greater than 1% in zero-loss and
  10% in accepted impaired runs. Protection-only repair cost is reported
  separately and cannot satisfy these utility gates.
- **SC-008**: All 32 formal cells have unique IDs, paths, and one invocation;
  every failed/incomplete cell remains in its treatment denominator; no
  automatic retry or selective replacement exists.
- **SC-009**: Every required metric is present or explicitly marked unavailable
  with a failing evidence gate; the unsampled Core terminal-attempt ledger
  conserves exactly against source/repair and initial/retry counters, while
  measured application counts conserve against raw per-sample/block records.
- **SC-010**: The final neutrality audit finds zero Core/binding branch whose
  behavior is selected by UAV, telemetry, acoustic/audio, codec, workload, or
  cell identity.
- **SC-011**: Before/after digests for Spec 127 and Spec 128 are identical, and
  neither historical runner has been invoked.
- **SC-012**: A positive shared statement is limited to the two tested
  workload/fault families; if either family fails, the final report records a
  measured negative result and makes no cross-application generality claim.

## Assumptions

- The formal environment is one two-node MiniNDN topology on a shared host;
  one-way latency/AoI uses one monotonic host clock and is not evidence of
  unsynchronized physical-device clocks.
- A five-second readiness/warm-up interval precedes each 60-second measured
  window. Delivery and latency use measured samples only; traffic counters
  cover the full run and explicitly state that scope.
- The telemetry size cycle is 256, 384, and 512 bytes. The acoustic source-count
  cycle is 2, 3, and 4 items, with each source item bounded at 512 bytes.
- Existing UAV request/response command, safety, readiness, and `GetStatus`
  services remain authoritative for control and complete snapshots. Streaming
  is not used for safety-critical command acknowledgement.
- The formal acoustic source is deterministic or file-backed so microphone,
  sound-card, and codec timing do not confound transport results.
- Current Spec 127 and Spec 128 directory-root digests at definition time are
  `6756fba7fad110f267863f4bb9f5cb1abec5c68515642ba383235a6a6729136d`
  and
  `6f532069382d0dc6b3d853c3ab9251e71993e985ae98a1a57c955d719e03faee`.
