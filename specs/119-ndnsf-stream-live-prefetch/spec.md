# Feature Specification: NDNSF Stream Live Prefetch

**Feature Directory**: `specs/119-ndnsf-stream-live-prefetch`

**Created**: 2026-07-17

**Status**: Complete in the MiniNDN scope. Public C++/Python integration,
app-neutral network validation, and protected UAV adoption pass;
`mapped-live-v1-future-on` remains experimental and `mapped-pressure` remains
the default because the frozen adoption gate rejected the former.

**Input**: Improve NDNSF Stream prefetch using the receiver-driven real-time
streaming design in Gusev et al. Preserve each application's original meaningful
NDN Data name. A signed, bounded, ahead-of-production `StreamNameMap` relates a
monotonic internal cursor to each original name, enabling future exact-name
Interests without wrapping Data inside Data.

## Scope

This feature adds one app-neutral cursor/Mapping substrate and a simple public
`LiveStreamPublisher`/`LiveStreamConsumerHandle` API that keeps exact
original-name Interests outstanding. It does not create another
Request/Response service: a normal or Targeted service response may carry the
validated descriptor, after which the independent live-stream API owns the
continuous data path.

Core owns immutable cursor/name-or-tombstone Mapping records, Mapping signing
and retrieval, Provider validation, bounded producer pending state, exact
semantic-name Interest expression, live-edge, pipeline-demand, burst/withhold,
recovery-deadline decisions, optional bounded FEC over opaque bytes, and a
reusable lifecycle handle. Applications own semantic-name construction,
explicit ahead reservation, payload production/admission, media sample
semantics, encryption/decryption, decoder, bitrate, and UI. Core never receives
a key or plaintext and never parses application content.

The UAV video application is the first integration target and MUST use this
public API rather than retain its own Mapping fetcher, sequence-name loop,
window authority, or producer pending table. Current code groups H264 bytes
into one publication/FEC group; that group is the initial sampling unit and its
measured production period—not configured camera FPS—drives the detector.

## User Scenarios & Testing

### User Story 1 - Join The Current Live Edge Quickly (Priority: P1)

A latest-mode consumer starts from an authenticated descriptor containing
Mapping authority, five distinct cursor frontiers, an application-safe join
cursor, and the production period of the declared sample unit. It pipelines
predictable Mapping blocks, resolves future cursors to original names, issues
exact original-name Interests ahead of production, and declares the live edge
only after frame-arrival timing is consistently compatible with the producer
period.

**Why this priority**: Prefetch is useful only if a consumer can reach current
live Data without guessing forever or remaining trapped behind cached history.

**Independent Test**: Feed deterministic cached-burst and live-production
traces into the policy and prove it enters Chasing, detects no false live edge,
then reaches Fetching after the required consecutive stable windows.

**Acceptance Scenarios**:

1. **Given** latest-mode and valid Mapping frontiers, **When** cached samples
   arrive faster and less regularly than the producer period, **Then** the
   consumer remains in Chasing and may increase bounded Mapping/Data pipelines.
2. **Given** admitted sample arrivals stabilize around the producer period for the
   configured consecutive windows, **When** the detector evaluates them,
   **Then** it enters Adjusting and then Fetching without selectors or non-exact
   payload names.
3. **Given** no valid Mapping, frontier, or production period, **When** latest-mode is
   requested, **Then** the consumer fails closed or uses the explicitly chosen
   rollback policy; it does not invent original names or claim live-edge status.
4. **Given** a validated descriptor, **When** an application calls
   `openLiveStream`, **Then** one lifecycle handle performs Mapping retrieval,
   exact semantic-name prefetch, bounded retries, and status reporting; the
   application does not construct Mapping names or manage a second Face loop.

---

### User Story 2 - Sustain A Bounded Future-Interest Pipeline (Priority: P1)

Once live, a consumer keeps Mapping resolution ahead of payload demand and
enough future exact original-name Interests outstanding to cover measured delay
and producer period, but not so many that Mapping, pending Interest, or decoder
state grows without bound.
For variable-size samples, it learns how many packets a sample normally needs
and requests any underestimated remainder as soon as the actual sample boundary
is authenticated.

**Why this priority**: A static or pressure-only window can be too small on a
long path and too large on a short or congested path. Both cases increase live
latency or timeout load.

**Independent Test**: Vary Mapping lead, producer period, retrieval delay,
segment count, backlog, timeout, and Nack traces; verify cursor ranges, resolved
original names, Mapping/Data windows, lifetimes, and hold intervals stay bounded.

**Acceptance Scenarios**:

1. **Given** accepted Mapping lead, sample period, and delay estimate, **When**
   conditions are stable, **Then** minimum sample demand is the ceiling of
   retrieval delay divided by sample period and is converted into a bounded
   packet window using the learned segments-per-sample estimate.
2. **Given** an authenticated sample has more items than predicted, **When**
   later-cursor Mapping arrives, **Then** missing original exact names are
   requested immediately as ordinary late retrieval and Mapping lateness is
   reported separately.
3. **Given** congestion, repeated future-probe timeouts, or decoder backlog,
   **When** pressure rises, **Then** the policy withholds or reduces new
   Interests and waits one detection period before another multiplicative
   change.

---

### User Story 3 - Recover Opaque Content Before The Deadline (Priority: P2)

A live consumer distinguishes an item that is merely not generated yet from one
that is late or missing. When enabled, the same API reconstructs one missing
opaque source item from exact-name, Provider-validated XOR repair Data and
signed digest metadata before considering bounded retransmission. Core never
decrypts the result. It returns to adjustment when the live edge, path delay,
or buffer condition changes.

**Why this priority**: Timeout alone is not a useful recovery clock for live
media. Recovery that finishes after the sample deadline wastes bandwidth and
still stalls playback.

**Independent Test**: Inject delay shifts, loss, Nacks, missing segments,
session replacement, and malformed/untrusted Data; verify recovery occurs only
inside the remaining playout budget and invalid observations never influence
the estimator.

**Acceptance Scenarios**:

1. **Given** FEC is enabled and exactly one mapped source item is absent,
   **When** the signed group metadata and repair Data validate within the
   recovery budget, **Then** Core reconstructs and digest-validates the opaque
   bytes and delivers them with `FecRecovered` provenance.
2. **Given** FEC is disabled or cannot safely recover the missing item and
   sufficient deadline budget remains, **When** the checkpoint is reached,
   **Then** one bounded exact-name retransmission is permitted; otherwise the
   sample is skipped explicitly.
3. **Given** a changed session, invalid signature, failed decryption, replay,
   or inconsistent sample metadata, **When** Data arrives, **Then** it contributes
   nothing to live-edge, delay, segment-count, or buffer estimators.

## Edge Cases

- A consumer starts after the advertised frontier has already advanced beyond
  producer retention.
- The descriptor's newest produced item is not a safe decoder join point; the
  application supplies an earlier complete-sample/keyframe join cursor.
- Publication-group production is bursty even though its period is stable.
- Application sample classes have sharply different packet counts.
- The path changes and retrieval delay steps upward or downward without loss.
- Multiple consumers prefetch the same future exact names and upstream Interests
  are aggregated.
- A malicious consumer requests names far beyond the current production
  frontier or attempts to exhaust producer pending state.
- FEC is disabled, repair Data is missing, two source items are missing, source
  lengths disagree, a signed digest does not match, or a repair group would
  exceed the NDN packet-size limit.
- Mapping is missing, late, replaced under one block name, signed by the wrong
  identity, contains conflicting cursor bindings, or points outside the
  Provider-authorized original-name namespace.
- Predicted entries exceed actual sample size; their immutable names remain
  unproduced and the associated Interests are cancelled/expire. A tombstone is
  allowed only if fixed before the block was signed. An underestimated sample
  needs newly allocated later cursors and Mapping.
- Interest lifetime is shorter than the generation time of the furthest valid
  lookahead name.
- Producer or consumer restarts while Interests from the old session remain in
  PITs or caches.
- The paper-derived detector parameters produce a false live-edge decision in
  a multi-hop topology.

## Requirements

### Functional Requirements

- **FR-001**: Payload Data MUST retain the application's original meaningful,
  globally immutable, version-unique exact NDN name below a descriptor-pinned
  Provider-owned routed prefix. A strictly increasing internal Stream cursor
  orders retrieval but MUST NOT become the payload Data name. Steady retrieval
  MUST NOT use rightmost-child, `CanBePrefix`, or per-item discovery.
- **FR-002**: Core MUST define one bounded `StreamNameMap` whose predictably
  named, Provider-signed blocks immutably bind fixed cursor spans to exact
  original Data names or predeclared tombstones. For capacity `B`, cursor `C`
  MUST resolve to `block=floor(C/B)` and slot `C mod B`; block Data names MUST
  use descriptor-pinned root plus typed Version and SequenceNum components.
  Each block MUST be one signed `ContentType=Manifest` Data packet, have no
  `FinalBlockId`, fit the configured and NDN maximum wire size, and contain
  neither nested NDN Data nor application payload bytes.
- **FR-003**: Latest-mode bootstrap MUST begin from a validated descriptor with
  contract/session/Mapping identities, fixed block capacity, descriptor-pinned
  Mapping anchor block/content digest, declared sample unit/period, and distinct `latest_join`, `latest_produced`,
  `mapping_committed_through`, `oldest_retained`, and `next_reserved` cursors.
  Latest-mode begins at the application-safe join cursor; beginning-mode starts
  from an explicit retained cursor. Success requires retained <= join <=
  produced <= Mapping-committed < next-reserved and a checkpoint anchor covering
  join. `oldest_retained` MUST be the oldest payload cursor for which the
  Provider can also serve the complete Mapping verification chain through the
  checkpoint; Mapping eviction MUST advance this frontier atomically with
  payload availability. Readiness timeout returns failure rather than a partial descriptor.
  Cursor zero is not an empty sentinel. Neither mode invents an original name.
- **FR-004**: Core MUST extend the existing `StreamAdaptiveFetcherState` as the
  single generic prefetch decision engine; it MUST NOT add a parallel generic
  fetch state machine.
- **FR-005**: The decision engine MUST expose Inactive, Chasing, Adjusting,
  Fetching, Recovering, and Stopped with deterministic reset/session transitions.
- **FR-006**: Live-edge detection MUST operate on one admitted application-
  declared sample unit, not raw packets. Initial UAV integration uses one
  publication/FEC group per sample and a Provider-measured period for that same
  unit; it MUST NOT equate configured FPS or `frameSeq` with an encoder frame
  without evidence. Skipped sample timing is normalized by identifier distance.
- **FR-007**: The detector MUST compare two adjacent bounded arrival-period
  windows with the producer period and require configurable consecutive stable
  results before declaring the live edge. The implemented NDNSF formula and
  thresholds MUST have NDNSF names; they MUST NOT be labeled as Gusev paper
  profiles unless they implement the paper's printed equation exactly.
- **FR-008**: The paper's printed Eq. (3), its prose interpretation, and any
  corrected/NDNSF detector MUST be separate deterministic profiles/evidence.
  The printed equation has a similarity-inequality tension with its prose, so
  it is reproduction evidence only; no profile becomes an unqualified default
  until the matched MiniNDN gate passes.
- **FR-009**: Stable pipeline demand MUST include the ceiling of accepted network
  retrieval delay divided by sample period, converted through a bounded
  segments-per-sample estimate and explicit reserve.
- **FR-010**: If generation delay is unavailable or unsafe to infer, Core MUST
  use a documented conservative network-delay estimator and MUST NOT subtract
  unsynchronized producer/consumer wall clocks.
- **FR-011**: Chasing/adjustment MAY use multiplicative burst/withhold actions,
  but one aggregate in-flight budget MUST cover Mapping, payload, and
  retransmission Interests. Every action MUST respect that budget, bounded
  Mapping reserve, lookahead, producer/forwarder caps, and one-action-per-
  detection-period hysteresis. Nack reason Congestion or CongestionMark causes
  multiplicative decrease/hold; a future-generation wait is not loss.
- **FR-012**: Interest lifetime MUST cover accepted network delay, the generation
  horizon of the furthest mapped future name, and bounded jitter.
- **FR-013**: Core MUST expose one public C++ API with Python parity consisting
  of `ServiceProvider::createLiveStream(...)`, `LiveStreamPublisher`,
  `ServiceUser::openLiveStream(...)`, and `LiveStreamConsumerHandle` (names may
  change only before the API contract is frozen). Applications provide
  semantic names and explicit ahead reservations; Core owns Mapping and payload
  network I/O, signing/validation, exact Interest expression/cancellation, the
  aggregate budget, and lifecycle status. The application callback owns
  payload-specific admission, and only callback-accepted samples update
  estimators.
- **FR-014**: Mapping MUST be committed and verifiable early enough that the
  payload Interest reaches the Provider before payload production. Complete
  names MUST be known before production to qualify as ahead-mapped. Late or
  production-dependent names remain valid for ordinary retrieval but are
  excluded from prefetch success. Missing/unverified Mapping pauses scheduling
  rather than falling back to cursor-derived names.
- **FR-015**: A bounded item-count estimator MUST learn from admitted complete
  samples. Underestimation allocates later cursors/blocks and is late retrieval.
  Overestimation leaves published cursor-to-name bindings immutable; once the
  authenticated actual count is known, extra Interests are cancelled or expire
  as `terminal-unproduced`, and their cursors are never reused. Tombstones may
  only be sealed before any name was published for that cursor.
- **FR-016**: Producer pending payload Interests MUST be admitted only when an
  accepted active-session Mapping resolves the exact original name to a cursor
  within the bounded frontier. Mapping blocks, reverse lookup, pending Interests,
  expiry, and cleanup MUST have independent caps and rejection reasons. One
  cursor occupies at most one application entry; near cursor/early deadline
  wins and may evict farthest-future work. Prefix registration precedes
  descriptor visibility; rejected far-future/unmapped names use immediate
  bounded Nack/no-allocation behavior. NFD PIT capacity exists before the APP
  callback, so ingress/lifetime/resource configuration and PIT monitoring are
  deployment prerequisites rather than an APP-only guarantee.
- **FR-017**: Recovery checkpoints MUST derive from accepted retrieval delay and
  remaining playout budget. Core owns the configured optional FEC attempt and
  then returns bounded exact-name retransmission/skip eligibility. Expired
  samples are skipped explicitly; neither FEC nor retransmission may exceed the
  aggregate Interest/state budgets.
- **FR-018**: Quality and bitrate remain application policy. Core MAY report
  congestion/path-change evidence but MUST NOT choose media rendition/encoding.
- **FR-019**: Only Mapping accepted by exact control name, Provider signature,
  session/epoch/range/immutability and original-name authority, payload/repair
  Data accepted by mapped exact name, signature, session and replay, or locally
  reconstructed opaque bytes accepted by descriptor-pinned Provider-signed FEC
  metadata and source digest may enter the application callback. Only the
  application's later acceptance, including Spec 118 decryption, may update
  estimators.
- **FR-020**: All Mapping, reverse, observation, decision, and pending state MUST
  be bounded and expose non-secret cache/live/future-wait/mapping-starved/late/
  conflict/congestion/recovery/stale/invalid diagnostics. The Provider MUST
  retain and serve every Mapping block needed to verify the advertised retained
  payload interval through the descriptor checkpoint; it MUST NOT advertise a
  payload as retained after the required Mapping block or chain has been evicted.
- **FR-021**: Existing `StreamChunk` payload semantics, UAV decoder, normal/
  Targeted NDNSF services, and finite-object segmented retrieval remain
  compatible. The former transport-only UAV payload name is deliberately
  replaced and MUST NOT remain an automatic fallback.
- **FR-022**: Pressure-only behavior remains a bounded rollback policy during
  validation, with one owner, comparative telemetry, and an explicit removal or
  diagnostic-only decision after the acceptance campaign.
- **FR-023**: Mapping and payload Interests MUST be exact
  (`CanBePrefix=false`, no ApplicationParameters). Freshness and `MustBeFresh`
  are cache policy, never security expiry or replay protection. PIT aggregation
  is an optional optimization only; correctness and caps MUST hold without it.
- **FR-024**: The descriptor and validator MUST pin collision-resistant session/
  versioned Mapping and payload roots plus the expected Provider identity.
  Same-name equivocation, old cached Data, a different valid Provider signer,
  or continuity fork MUST close the session and update no estimator.
- **FR-025**: Ahead Mapping's additional early/batch disclosure of future names,
  count, timing, and unused predictions MUST be documented. Applications MUST
  declare mapped names non-sensitive; this version does not claim name or
  traffic-analysis confidentiality.
- **FR-026**: The generic API MUST keep payload Data under its original semantic
  Name. Only Mapping block names are sequential and constructible. Mapping is
  `cursor -> original Data Name`; consumers MUST NOT fetch cursor-named payload,
  encapsulate payload Data inside Mapping Data, or treat Mapping as a substitute
  trust decision.
- **FR-027**: `LiveStreamPublisher` MUST provide ahead reservation followed by
  materialize-once publication. `LiveStreamConsumerHandle` MUST provide
  `start/status/stop` plus verified-item and status callbacks. Stop and session
  replacement MUST be idempotent, bounded, and suppress callbacks from retired
  sessions.
- **FR-028**: Publisher creation MUST enter `Preparing`; a descriptor MUST NOT
  become visible until atomic `activate(readiness)` supplies a positive measured
  sample period, application-safe join, committed Mapping coverage, and valid
  five frontiers. Multi-item applications MUST be able to report an accepted
  sample boundary asynchronously after item admission; an optional one-item
  adapter MAY combine those steps without weakening the ordering gate.
- **FR-029**: Public publish and receive APIs MUST treat application content as
  opaque bytes. Core MUST NOT accept a key, encrypt, decrypt, inspect payload
  fields, select a cipher, or expose a plaintext fallback. Protected
  applications encrypt before `publish`/`publishGroup` and decrypt only inside
  the verified-item callback.
- **FR-030**: FEC MUST be optional and disabled by default. Contract v1 MUST
  support `None` and one bounded XOR repair item over already-protected opaque
  source bytes through ahead-reserved semantic source/repair names. Source,
  repair, and FEC-control Data remain exact-name and Provider-signed; the signed
  group metadata MUST bind session/group, names/cursors, original lengths, and
  SHA-256 source digests and MUST fit one bounded NDN Data packet.
- **FR-031**: Recovered opaque bytes MUST be delivered only after Provider,
  group, length, and digest validation, with explicit `FecRecovered`
  provenance. They MUST NOT be republished or inserted into a cache as the
  missing original Data. Invalid/expired/over-limit recovery updates no
  estimator and falls through only to the bounded retransmission/skip decision.

### Key Entities

- **StreamCursor**: Monotonic internal ordering identifier; never the payload
  Data name. Applications such as Spec 118 may bind it into separate crypto
  state, but Core does not define nonce semantics.
- **StreamNameMapBlock**: Provider-signed immutable bounded cursor-to-original-
  name or tombstone bindings committed ahead when future prefetch is claimed.
- **StreamPrefetchObservation**: One admitted sample or network event with
  session, cursor/sample boundary, expression/arrival timing, retrieval delay,
  segment count, Mapping lead, buffer state, and validity classification.
- **StreamPrefetchPhase**: The receiver state controlling whether it chases,
  adjusts, fetches steadily, recovers, or stops.
- **StreamLiveEdgeEstimate**: Adjacent-window arrival statistics, producer
  period, confidence, consecutive stable count, and decision reason.
- **StreamSegmentDemandEstimate**: Bounded estimate of packets per application
  sample plus the authenticated actual range when known.
- **StreamFetchDecision**: The existing generic decision extended with phase,
  cursor range, Mapping/Data demand, hold/recovery timing, Mapping readiness,
  live-edge confidence, and reason.
- **LiveStreamFecOptions / LiveStreamFecGroup**: Default-off bounded recovery
  configuration and an immutable ahead reservation for semantic source/repair
  names. Core sees only opaque bytes.
- **LiveStreamFecManifest**: Provider-signed group commitment binding names,
  cursors, lengths, source digests, and repair scheme without carrying nested
  source Data.

## Success Criteria

- **SC-001**: Deterministic Mapping, tombstone, terminal-unproduced,
  late/conflict, stale-cache, live-production, sample-burst, path-change, and loss traces produce identical
  C++/Python decisions with zero false live-edge declarations.
- **SC-002**: Every production payload fetch uses an exact original name from
  one accepted Mapping entry inside the active frontier. Mapping, reverse,
  producer, and consumer pending counts never exceed configured caps; no APP-
  owned direct oracle duplicates Core validation or scheduling.
- **SC-003**: For all accepted delay/period test vectors, stable sample demand
  equals the specified ceiling calculation before bounded packet conversion,
  and every window/lifetime/recovery result remains within configured limits.
- **SC-004**: Wrong-session, malformed, unsigned, signer-invalid, replaced or
  conflicting Mapping, unauthorized original name, replayed, and decryption-
  invalid payloads change zero estimator fields and produce zero callbacks.
- **SC-005**: In every `mapped-live-v1-future-on` acceptance trace, at least 99%
  of all controller-generated eligible, nonterminal future cursor opportunities
  MUST be Mapping-resolved early enough and MUST arrive at the Provider before
  payload production. Both denominators MUST be nonzero. Future-off reports both
  future-hit gates as N/A by design. Events correlate Mapping admission,
  Interest expression/arrival, and production by session/cursor.
- **SC-006**: In matched 60-second MiniNDN 0% and 5% loss campaigns,
  `mapped-pressure`, `mapped-live-v1-future-on`,
  and `mapped-live-v1-future-off` share payload names, workload, and controller.
  The live candidate
  completes at least as many scheduled streams and controls as mapped-pressure,
  introduces no unbounded APP state or unexplained NFD PIT growth, and reports first sample/live edge, p50/p95 lag,
  timeout/Nack/CongestionMark, retransmission, decoded samples, aggregate
  in-flight, map bytes/Interest share/starvation, and buffer occupancy.
- **SC-007**: `mapped-live-v1-future-on` becomes default only if its matched
  comparison with mapped-pressure shows no correctness/reliability regression, improves
  either p95 live lag or timeout/Nack load in at least four of five paired runs
  with at least 10% median paired improvement, and does not worsen the other by
  more than 5%. Future-pending value is reported against the same mapped-live
  controller with future lookahead disabled. Mapping bytes and Interest share
  are reported directly. Otherwise the negative result is preserved and mapped-
  pressure remains default.
- **SC-008**: Existing Core Stream parity, UAV protocol, normal/Targeted NDNSF,
  finite-object segmented retrieval, and Spec 118 security gates pass. Public
  C++ and Python LiveStream smoke tests require no UAV types, selectors, or
  cursor-derived payload names.
- **SC-009**: App-neutral C++/Python tests publish random opaque byte strings
  with FEC off and with one XOR repair, lose each source position in turn, and
  recover byte-identical content only when Provider signature, group, length,
  digest, deadline, and cap checks pass. No Core API or diagnostic contains a
  key or calls application encryption/decryption; two-loss, corrupt-repair,
  wrong-signer, wrong-digest, oversize, and expired cases produce zero accepted
  callbacks and estimator changes.

## Assumptions

- The revised UAV descriptor supplies contract/session/versioned roots,
  Mapping anchor, five frontiers, sample unit/period, and eligibility fields;
  Spec 118 makes descriptor, Mapping, and payload admission fail closed.
- Stream cursors are global and strictly increasing within one session even
  though original Data names are application-defined.
- Identical exact Interests may be aggregated, but correctness and capacity do
  not depend on aggregation because selectors, forwarding hints, and lifetimes
  can prevent or shorten overlap.
- Current paper thresholds are research baselines, not evidence that the same
  values are optimal in MiniNDN or the UAV workload.
- MiniNDN is the acceptance environment. Real UAV and iTiger validation are
  outside this feature.

## Out Of Scope

- A second Request/Response invocation mode or a Stream-specific Controller.
- Application-specific encryption/decryption, codec, decoder, bitrate, quality,
  or media-aware FEC policy inside NDNSF Core. Core supplies only default-off,
  bounded opaque-byte XOR recovery.
- Selector-based latest discovery.
- Hiding application names from observers; original NDN names remain visible in
  Interests and Data even when payload Content is encrypted.
- Global anti-equivocation/transparency or guaranteed availability under
  repeated invalid same-name cache responses. Admission prevents use of invalid
  Data; it cannot pre-pin the implicit digest of payload not yet produced.
- Media codec, encoder, quality/rendition, or UI redesign.
- A general erasure-code/plugin SPI or a claim that optional XOR-FEC improves
  the UAV workload without new matched evidence.
- Static files, model artifacts, Repo objects, or DI tensor transfers.
