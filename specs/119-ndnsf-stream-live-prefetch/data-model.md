# Data Model: NDNSF Stream Live Prefetch

## LiveStreamDefinition

Application-supplied immutable configuration for one Provider session:

- collision-resistant stream/session identity and semantic Data prefix;
- sample unit; measured production period and safe join arrive later in the
  activation readiness record;
- fixed Mapping capacity/ahead horizon and retained-item limit;
- aggregate and producer-pending bounds;
- default-off FEC selection: `None` or bounded `XorOneRepair`, plus group and
  wire-size caps;
- expected Provider identity and rollback/profile choice.

It contains no encryption key, cipher, plaintext, payload parser, media codec,
or cursor-derived payload name rule.

## LiveStreamReservation

Immutable ticket returned by `LiveStreamPublisher::reserveAhead` containing the
active Stream identity, Mapping version, cursor, and complete original Data
Name. Publication must present this ticket; it cannot replace the Name or reuse
the cursor. Applications may bind the ticket into AEAD associated data.

## LiveStreamFecGroupReservation

Immutable group ticket returned by `reserveGroup`. It contains ahead-committed
semantic source and repair reservations, a bounded group identity, and the
descriptor-pinned `None` or `XorOneRepair` scheme. `publishGroup` accepts only
already-protected opaque source bytes and materializes the group once.

## LiveStreamFecManifest

Canonical bounded control metadata carried inside each Provider-signed semantic
repair Data for one group:

- Stream/session/group and scheme;
- exact source and repair names/cursors;
- exact unpadded source lengths and SHA-256 digests;
- repair count and maximum coded length.

It carries no nested source Data and no plaintext; the repair Content also
carries the coded opaque bytes. Source and repair Data must fit configured and
NDN wire limits before the group can be committed. No extra cursor-derived FEC
Data name is introduced.

## VerifiedLiveStreamItem And Admission

`VerifiedLiveStreamItem` contains cursor, original Data Name, Provider identity,
session/Mapping identity, opaque Content bytes, local retrieval timing, and
`SignedData` or `FecRecovered` provenance. Signed Data is delivered after
Mapping/name/signature admission. Recovered bytes are delivered only after the
descriptor-pinned Provider-signed FEC manifest, length, and digest validate and
are never cached/republished as the missing Data. The application callback returns
`acceptItem()` or `reject(reason)`. Rejection is observable but updates no live-
edge, item-count, recovery, FEC, or decoder-facing state. Multi-item applications
submit a separate accepted-sample observation when the complete sample boundary
is known.

## LiveStreamConsumerHandle

One idempotent `start/status/stop` lifecycle owner combining the Mapping
resolver, `StreamAdaptiveFetcherState`, exact Interest scheduler, validator,
timers, bounded in-flight state, optional opaque-byte FEC, and callbacks. It
never owns application encryption/decryption, media parsing, decoder, bitrate,
or UI policy.

## LiveStreamReadiness

Atomic transition input from `Preparing` to `Active`: positive measured sample
period, application-safe join cursor, produced/Mapping/retained/reserved
frontiers, and checkpoint anchor. No descriptor or key-bearing application
response may expose a tentative publisher before this record passes validation.

## StreamPrefetchConfig

Bounded policy configuration for one controller.

- production mode: `mapped-pressure` or `mapped-live-v1-future-on` only;
  experiment controls are not values of this configuration and cannot appear in
  the Core/Python production API or a Stream descriptor
- minimum/base/maximum packet window
- minimum/maximum lookahead and producer pending count
- minimum/maximum Interest lifetime and recovery timeout
- sample period and jitter margin
- detector profile: change threshold, period similarity, window length, stable
  count, detection period
- chase multiplier and adjust multiplier
- segment-estimator history and class count limits
- fixed mapping block capacity, signed-wire-size cap, cache limit, maximum
  advertised lead, and maximum unresolved-map wait
- one aggregate in-flight/PIT budget plus bounded Mapping reserve

Validation rejects zero sample period in live mode, reversed bounds, multipliers
outside their safe ranges, unbounded histories, and unknown profiles.

## StreamBootstrapDescriptor

Authenticated application control data consumed by Core after application
validation:

- contract version, collision-resistant Stream/session identity, expected
  Provider identity, versioned payload prefix, exact Mapping root/version, and
  fixed block capacity plus the authenticated checkpoint Mapping block/digest
  containing the latest join cursor;
- declared sample unit and positive period measured for that exact unit;
- `latestJoinCursor`: application-safe complete-sample/decoder start;
- `latestProducedCursor`: newest payload already produced;
- `mappingCommittedThroughCursor`: newest gap-free sealed Mapping entry;
- `oldestRetainedCursor`: oldest payload for which both the payload and complete
  Mapping verification chain through the checkpoint remain serviceable by the
  Provider;
- `nextReservedCursor`: first cursor not yet allocated;
- eligibility: `ahead-mapped` or `retrieval-only`.

Success requires all fields: `oldestRetained <= latestJoin <= latestProduced <=
mappingCommittedThrough < nextReserved`. Cursor zero is a real cursor, never an
empty sentinel. The checkpoint block/digest MUST equal the fixed block
containing latestJoin. A Provider that cannot establish this state before its
bounded start-readiness timeout returns failure, not a partial descriptor.
These fields are not collapsed into one `next_cursor`.

## StreamPrefetchPhase

`Inactive`, `Chasing`, `Adjusting`, `Fetching`, `Recovering`, or `Stopped`.

Session reset clears observations, pending decisions, live-edge confidence,
segment estimators, and recovery state before entering `Chasing`. Stop is
terminal until an explicit reset.

## StreamCursor

A monotonically increasing unsigned ordering coordinate scoped to one
collision-resistant Stream session and mapping version.

- cursor is not an NDN Data name and has no application semantics;
- one cursor binds exactly once to one globally immutable original name or a
  tombstone known before block publication;
- cursors and explicit payload/Mapping names are never reused across sessions;
- cursor ranges drive controller windows, lookahead, and recovery state.

## StreamNameMapBlock

Provider-signed control Data with fixed capacity `B` and predictable exact name:

```text
/<provider>/NDNSF/STREAM-MAP/<stream-id>/
  v=<mapping-version>/seq=<block-number>

block-number = floor(cursor / B)
slot         = cursor mod B
```

Fields:

- contract/schema version, Stream/session identity, and mapping version;
- block number, fixed capacity, and first cursor `B*block-number`;
- exactly `B` ordered entries containing the exact original NDN Name or a
  predeclared tombstone (cursor is derived from block/slot);
- required previous canonical-Content SHA-256 digest except for genesis;
- bounded schema/version metadata.

Invariants:

- one block is one signed `ContentType=Manifest` Data packet, with no
  `FinalBlockId`, whose total wire encoding fits both configured cap and
  `ndn::MAX_NDN_PACKET_SIZE`;
- entries cover the fixed range, are immutable and non-overlapping;
- names/control metadata only: no nested NDN Data and no media payload;
- each original name is version-unique, belongs below a descriptor-pinned
  routed prefix that the expected Provider identity may sign, and maps once;
- the advertised frontier advances only after all covering blocks are
  committed;
- an ahead-predicted name that produces no Data remains bound and becomes local
  `terminal-unproduced`; it is never rewritten into a tombstone;
- later blocks for underestimated samples are ordinary retrieval and marked
  ineligible for future-prefetch credit;
- same-name different-content or continuity fork is a fatal session fault.
- Provider Mapping retention covers every advertised retained payload through
  the descriptor checkpoint; evicting a required block atomically advances the
  payload `oldestRetainedCursor` beyond that block's range.

## StreamNameResolverState

Bounded consumer/producer state keyed by Stream identity and mapping version:

- verified block cache and highest gap-free committed cursor;
- cursor-to-original-name lookup;
- original-name-to-cursor reverse lookup plus a bounded Mapping-version-lifetime
  name reservation set, so Provider retention or consumer cache eviction cannot
  make an old exact Data name reusable;
- tombstone and terminal-unproduced sets, missing-block ranges, late-block
  counters, and eviction state;
- Mapping rejection counters by signature, authority, version, overlap, remap,
  gap, bounds, and malformed-name reason.

An accepted session/version reset removes all prior resolver state only after it
proves a new session epoch and that the new Mapping version and typed
payload-prefix version do not reuse the old session namespace. The configured block/name bounds must cover the advertised
retained-through-committed interval; reservation exhaustion requires a new
session. An unresolved cursor cannot produce a payload Interest.

## StreamPrefetchObservation

One admitted input event:

- session epoch, mapping version, cursor, and sample identity
- event kind: accepted sample, accepted packet, timeout, Nack, duplicate,
  backlog, path hint, invalid observation, or recovery result
- monotonic Interest expression and Data arrival times
- known-produced flag and optional proven same-clock generation delay
- mapping block full-name/content-chain identity and whether it was admitted
  before payload scheduling eligibility on the consumer monotonic clock
- Provider-local Interest-arrived-before-payload-production event correlated by
  session/cursor (never by cross-host clock subtraction)
- sample arrival period and authenticated sample boundary
- sample-identifier distance used to normalize skipped samples
- actual segment count and optional application class
- playable buffer and remaining deadline

Invalid observations increment diagnostics only. They do not mutate live-edge,
delay, segment, or recovery estimates.

## StreamLiveEdgeEstimate

- declared sample unit and Provider-measured sample period
- two bounded adjacent arrival-period windows
- old/new means
- NDNSF relative mean change and period similarity; separate literal-paper
  reproduction statistics when that fixture is selected
- consecutive stable count and required count
- confidence, profile, and reason

Confidence resets on session change, non-monotonic sample identity, or sustained
instability. A single delayed sample does not immediately cause a phase flip.

## StreamSegmentDemandEstimate

- bounded history by opaque application class
- EWMA and conservative upper estimate
- last authenticated actual first/last cursor and segment count
- predicted cursor count, reserved range, later underprediction cursors,
  predeclared tombstones, and immutable terminal-unproduced predictions

The application supplies class labels; Core treats them as opaque bounded keys.

## StreamFetchDecision

Existing fields remain:

- packet window, lookahead, Interest lifetime, missing timeout, pressure, reason

New fields:

- phase and policy mode
- sample/packet demand and recovery reserve
- hold duration until another multiplicative action
- recovery checkpoint and retransmission eligibility
- live-edge confidence/profile
- congestion/path-change/future-wait evidence
- mapping block range to request, payload cursor range eligible for scheduling,
  mapping-wait reason, and advertised/verified mapping frontier
- aggregate in-flight limit and Mapping/payload/retransmission allocations,
  congestion hold, Nack/CongestionMark reason, and starvation evidence

The facade consumes the decision to perform configured Core FEC and bounded
exact-name retransmission/cancellation. Applications perform encryption,
decryption, sample admission, skip/playout, bitrate, and decoder operations.

## ProducerFutureInterestAdmission

One bounded producer-side check after reverse resolution through committed
mapping state:

- active Stream/session/Mapping version
- requested exact original name and resolved cursor
- current produced cursor frontier and oldest retained cursor
- maximum future distance and total pending cap
- deduplication by cursor and priority by cursor distance/deadline, with
  farthest-future eviction
- local expiry derived from Interest lifetime
- outcome: serve retained, pend future, reject stale, reject too-far, reject
  capacity, or reject malformed

Unmapped, tombstoned, stale-version, or ambiguous names allocate no application
pending entry. Rejection uses immediate bounded Nack/no-APP-allocation behavior so forwarder PIT
state is measured rather than ignored. NFD ingress/lifetime/PIT safeguards are
deployment configuration because PIT allocation precedes APP dispatch. Expired
future entries are removed by bounded incremental cleanup. PIT aggregation is
optional and never a capacity assumption.
