# Feature Specification: Adaptive Sample-Atomic Prefetch

**Feature Branch**: `Experimental`

**Created**: 2026-07-19

**Status**: Complete — the original frozen MiniNDN result remains retained as
negative evidence, and the user-authorized `confirm06` cell passes the corrected
60-second GUI, Interest, future-hit, latency, and continuity gates

**Input**: Replace the fixed video/FEC group assumption with a bounded predictor
that learns different item counts for different sample classes, while ensuring
that the prefetch scheduler never truncates a predicted frame group merely to
fit a packet-count window.

## User Scenarios & Testing

### User Story 1 - Variable Frames Stay Atomic (Priority: P1)

As a live-stream consumer, I want the scheduler to request one predicted media
sample as a complete group so that a packet window does not fetch only the first
part of a large frame and force another network round trip.

**Independent Test**: Deterministic Mapping traces containing alternating 3+1,
12+1, and 27+1 groups prove that scheduling either admits every predicted item
of the next group in one round or defers the whole group; it never ends inside a
group.

**Acceptance Scenarios**:

1. **Given** a predicted group is larger than the current steady packet window
   but within configured limits, **When** capacity is available, **Then** the
   effective window expands to the group boundary and issues the whole group.
2. **Given** aggregate capacity cannot hold the complete next group, **When**
   scheduling runs, **Then** no payload Interest from that group is issued and a
   bounded capacity reason is reported.
3. **Given** pressure reduces demand, **When** a new window is chosen, **Then**
   the reduction removes complete future groups rather than a suffix of the
   next group.

### User Story 2 - Learn Key and Ordinary Frames Separately (Priority: P1)

As a Provider and consumer, I want opaque sample classes to keep separate,
bounded item-count histories so that a large key frame does not permanently
inflate every ordinary frame and ordinary frames do not cause key-frame
underfetch.

**Independent Test**: A deterministic GOP trace with independently changing
key and delta sizes proves that each class predictor changes only from admitted
observations of that class, remains within its declared cap, survives bounded
outliers, and exports prediction-error evidence.

**Acceptance Scenarios**:

1. **Given** key samples require many more items than delta samples, **When**
   both histories are warm, **Then** their predicted group sizes remain
   independent.
2. **Given** a new or reset class, **When** no trustworthy history exists,
   **Then** its declared conservative seed is used instead of another class's
   mean.
3. **Given** actual count is below the prediction, **When** the first admitted
   item reveals the authenticated actual extent, **Then** no additional
   unnecessary Interests are issued for that sample and the overprediction is
   counted.
4. **Given** actual count exceeds the prediction, **When** the extent is
   authenticated, **Then** the missing tail is requested immediately, the
   event is counted as an underprediction, and it receives no future-prefetch
   success credit.

### User Story 3 - UAV Uses Real Frame Extents (Priority: P2)

As a UAV operator, I want H.264 access units to retain their actual variable
segment counts so that empty padding shards and a fixed `12+1` assumption do
not waste Interests or hide key/delta behavior.

**Independent Test**: A fresh 60-second zero-loss MiniNDN run records distinct
key/delta group-size distributions, no padded empty source items, complete GUI
playback, and sample-atomic scheduling evidence.

**Acceptance Scenarios**:

1. **Given** a small delta frame, **When** it is segmented and protected,
   **Then** only its real source items plus the selected repair items are
   published.
2. **Given** a larger key frame, **When** it is produced, **Then** the key-class
   predictor and group scheduler accommodate its predicted extent without
   changing the delta-class predictor.
3. **Given** a completed run, **When** results are analyzed, **Then** key and
   delta prediction error, atomic deferrals, underprediction RTT penalties,
   Interest work, future-hit ratio, and capture-to-decode latency are reported.

### User Story 4 - Simple Generic Prefetch API (Priority: P1)

As an NDNSF application author, I want to declare stream timing, bounded sample
classes, and semantic naming once while Core owns prediction and Interest-window
control, so that applications do not reimplement the prefetch algorithm.

**Independent Test**: Equivalent C++ and Python examples publish a variable
two-class opaque stream using only sample announcement/publication APIs, while a
consumer retrieves it through the existing `openLiveStream(...)` lifecycle
without setting packet windows or parsing class metadata.

**Acceptance Scenarios**:

1. **Given** an APP knows a fixed sample period and future class schedule,
   **When** it announces bounded future samples, **Then** Core predicts counts,
   creates their semantic names through the APP callback, and commits Mapping.
2. **Given** a consumer opens the descriptor in adaptive mode, **When** group
   sizes change, **Then** Core adjusts whole-group Interest issuance without an
   APP window callback.
3. **Given** an APP needs diagnostics, **When** it queries status, **Then** it
   can inspect class predictions and errors without controlling the algorithm.

## Edge Cases

- A group whose declared maximum exceeds the aggregate Interest limit is
  rejected before the session becomes active; the scheduler cannot deadlock on
  an impossible atomic group.
- A class label is an opaque bounded identifier. Unknown classes use their own
  seed and cannot borrow another class's state.
- Predictor state is session-scoped and is not accepted from unauthenticated,
  rejected, incomplete, replayed, or recovered-with-ambiguous-boundary data.
- A prediction is not an exact promise. Absolute no-underfetch requires an APP-
  supplied hard upper bound that is actually valid; without one, the runtime
  reports rather than conceals unprecedented larger samples.
- FEC-disabled groups contain only sources. FEC-enabled groups use their actual
  variable source count and the configured repair rule.
- A one-source sample is valid. XOR-one-repair MAY encode its repair as a
  byte-identical recoverable copy under a distinct repair Data name, or the APP
  may disable repair for that sample; empty synthetic sources are forbidden.
- A Mapping block split must not make a group boundary ambiguous. Each entry
  carries enough canonical group context for independent block validation.
- Sample class and predicted extent are signed scheduling metadata and may leak
  coarse frame structure. Privacy-sensitive APPs may use opaque class IDs or a
  single padded class, at the cost of extra Interest work.

## Requirements

- **FR-001**: Core MUST represent a predicted sample group independently from
  the global packet window, including session, group ID, opaque class ID,
  item index, predicted source count, and selected repair count.
- **FR-002**: Signed Mapping MUST bind every reserved cursor/name to canonical
  group metadata before that cursor is eligible for future prefetch.
- **FR-003**: The adaptive Mapping contract version MUST change explicitly;
  each descriptor pins v1 or v2 and mixed/ambiguous semantics in one session
  MUST be rejected. Existing v1 manual sessions MAY retain their old behavior
  but cannot claim sample-atomic scheduling.
- **FR-004**: The scheduler MUST add or remove complete predicted groups and
  MUST NOT end a scheduling range inside a group.
- **FR-005**: Core MUST keep bounded, session-scoped prediction history per
  opaque sample class and MUST NOT use one undifferentiated average for all
  classes.
- **FR-006**: Prediction MUST be conservative: before the first authenticated
  observation use the declared class seed; afterward use the maximum extent in
  a bounded recent per-class history plus a configurable safety margin, clamped
  to `[1, hard maximum]`. The cold-start seed MUST NOT become a permanent lower
  bound; a plain EWMA mean or cross-class percentile is insufficient.
- **FR-007**: Only authenticated, complete sample extents MAY train a predictor.
- **FR-008**: Exact actual item count MUST be authenticated in Core-owned group
  metadata early enough to suppress the remaining excess predicted work or
  request an underestimated tail.
- **FR-009**: Underprediction, overprediction, atomic deferral, forced atomic
  expansion, and group-cap rejection MUST be observable in status and sampled
  `NDN_LOG` evidence.
- **FR-010**: Generic FEC MUST accept variable source counts within a declared
  maximum; it MUST NOT require every group to equal one session-wide source
  count.
- **FR-011**: UAV publication MUST stop padding small frames with empty source
  shards and MUST label real H.264 access units with opaque `key` or `delta`
  classes derived from authoritative encoder metadata where available.
- **FR-012**: Original semantic Data names, Provider signatures, Mapping digest
  continuity, Validator ownership, application encryption boundaries, and
  exact-name Interests MUST remain unchanged.
- **FR-013**: Core MUST NOT parse H.264, choose GOP structure, encryption, or
  bitrate; class assignment and valid per-class caps remain APP policy.
- **FR-014**: All capacities, histories, class counts, Mapping metadata, pending
  Interests, and diagnostic state MUST remain bounded.
- **FR-015**: Performance acceptance MUST preserve the original fresh 60-second
  MiniNDN negative result without overwriting or tuning it away. After a proven
  implementation defect is fixed, user-authorized confirmation runs MUST use
  new uniquely named result directories, retain every outcome, and continue
  until the 60-second GUI delivery gate passes or a genuine external blocker is
  demonstrated.
- **FR-016**: The public Provider API MUST offer one high-level future-sample
  announcement that accepts sample ID, opaque class ID, and an APP semantic-name
  factory, and one publication operation that supplies the real opaque source
  items. A bounded extent-preparation operation MAY accept the APP-observed
  real count before name-bound AEAD and MUST return Core-owned reservations;
  APP code MUST NOT calculate or pass the predicted packet window.
- **FR-017**: The public consumer API MUST retain the `openLiveStream(...)`
  handle lifecycle and make adaptive sample-atomic prefetch an option/default;
  ordinary consumers MUST NOT need to interpret key/delta classes or call a
  per-packet window controller.
- **FR-018**: C++ and Python MUST expose equivalent sample-class declarations,
  sample announcement/extent preparation/publication, adaptive open option, and
  read-only status.
- **FR-019**: A fixed bitrate, frame rate, or sample period MAY seed prediction
  and future announcement timing but MUST NOT be interpreted as a fixed
  per-sample byte or item count.
- **FR-020**: Packet demand MUST equal the sum of complete predicted groups for
  the next `ceil(networkRetrievalDelay / samplePeriod)` samples plus bounded
  whole-group recovery reserve; it MUST NOT multiply sample demand by one
  global mean.
- **FR-021**: Session validation MUST reject any class whose maximum source plus
  repair extent cannot fit the available payload capacity after bounded Mapping
  and retransmission reserves.

## Key Entities

- **SampleClassProfile**: Opaque class ID, conservative seed, hard maximum,
  bounded observation history, safety margin, and current prediction.
- **StreamSampleGroupBinding**: Signed Mapping metadata binding one cursor/name
  to group ID, class, item index, predicted sources, and repairs.
- **SampleExtentObservation**: Authenticated group ID/class and predicted versus
  actual source/repair counts used for bounded learning and diagnostics.
- **SampleAtomicFetchDecision**: Whole groups admitted or deferred, effective
  packet window, budgets, and explicit capacity/prediction reasons.
- **FutureSampleAnnouncement**: APP-provided future sample ID/class and semantic-
  name factory consumed by Core to reserve the predicted group ahead of data.

## Success Criteria

- **SC-001**: Alternating variable-size traces contain zero scheduling decisions
  whose payload end cursor falls inside an admitted predicted group.
- **SC-002**: Key-class observations do not change the delta prediction and vice
  versa; all predictor state stays within configured history and count caps.
- **SC-003**: Focused tests cover cold start, class isolation, overprediction,
  underprediction, group crossing a Mapping block, pressure decrease, aggregate
  capacity failure, variable FEC groups, and malformed signed metadata.
- **SC-004**: In the accepted UAV run, empty padded source items are zero and
  every decoded frame is assembled from its reported actual source count.
- **SC-005**: The accepted UAV run has zero key/delta underprediction events
  after bounded warm-up, or is retained as a negative result showing each
  additional-RTT event; no absolute guarantee is claimed without a valid hard
  cap.
- **SC-006**: Interest work is no more than 15% above actual source-plus-selected-
  repair items per decoded frame, Provider-confirmed future-hit success is at
  least 99%, capture-to-decode p95 is at most 250 ms and p99 at most 500 ms, and
  decoded/displayed video remains continuous through the final ten seconds.

## Assumptions

- Frame/item-count learning is about packetization demand, not network RTT;
  network delay and sample-period estimation remain separate controller inputs.
- The Gusev et al. design intentionally estimates key and delta segment counts
  separately and prefers some excess Interests to an extra RTT. NDNSF adapts
  that idea to signed semantic-name Mapping rather than copying its namespace.
- Provider-side prediction owns ahead Mapping reservation because only the
  Provider can sign future semantic-name bindings; consumers enforce the signed
  group boundary and independently record actual prediction error.
- Existing Spec 124 remains complete evidence for controller hold/DRD behavior;
  this feature supersedes only its fixed-group demand assumption.
- Fixed camera/encoder frame rate and target bitrate stabilize sample timing and
  average byte budget. A fixed GOP makes the expected key/delta schedule
  predictable, but scene complexity and rate control still permit variable
  individual frame sizes. A true hard count cap must come from a valid encoder
  or APP bound, not from average bitrate arithmetic.
