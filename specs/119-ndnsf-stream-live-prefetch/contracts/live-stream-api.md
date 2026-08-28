# Contract: Simple NDNSF Live-Stream Prefetch API

## Purpose And Boundary

NDNSF owns one reusable receiver-driven live-stream API. An application gives
NDNSF meaningful immutable Data names and **opaque content bytes**. NDNSF
publishes predictable signed Mapping, signs and serves the named Data, keeps
bounded future exact-name Interests outstanding, and optionally applies FEC to
the opaque bytes. NDNSF never receives a key, never encrypts or decrypts, and
never parses the application payload.

For a protected application the order is normative:

```text
application payload -> application encryption -> opaque bytes
  -> NDNSF Mapping / exact-name prefetch / optional FEC
  -> opaque bytes -> application decryption -> application decoder
```

The payload Data Name is always the application's original semantic name. A
monotonic `StreamCursor` is an internal ordering key only. Predictability comes
from signed Mapping Data whose own names are sequential and constructible:

```text
/<provider>/NDNSF/STREAM-MAP/<stream-id>/<Version>/<SequenceNum(block)>
    cursor 42 -> /memphis/uav/A/video/front/<session>/group=7/data/seg=0
```

The consumer predicts and fetches the Mapping name, resolves cursor 42, and
then expresses an exact Interest for that semantic name. Mapping carries no
payload and no nested NDN Data packet.

## Provider API

Conceptual C++ surface:

```cpp
LiveStreamDefinition definition{
  .streamId = streamId,
  .semanticDataPrefix = videoPrefix,
  .mappingBlockCapacity = 16,
  .mappingAheadBlocks = 4,
  .retainedItems = 600,
  .maxNameReservations = 65536,
  .maxPendingInterests = 256,
  .fec = LiveStreamFecOptions::none(), // default; or xorOneRepair(...)
};

auto publisher = provider.createLiveStream(definition);

// No FEC: the application encrypts first, NDNSF receives only opaque bytes.
auto item = publisher.reserveAhead(originalName);
publisher.publish(item, encryptedBytes);

// Optional FEC: every source and repair name is semantic and reserved ahead.
auto group = publisher.reserveGroup(sourceNames, repairNames);
publisher.publishGroup(group, encryptedSourceBytes);

auto descriptor = publisher.activate({measuredPeriod, safeJoinCursor});
publisher.status();
publisher.stop();
```

Conceptual Python surface:

```python
publisher = provider.create_live_stream(definition)
reservation = publisher.reserve_ahead(original_name)
publisher.publish(reservation, encrypted_bytes)

group = publisher.reserve_group(source_names, repair_names)
publisher.publish_group(group, encrypted_source_bytes)
```

`reserveAhead` and `reserveGroup` accept complete original NDN Names, never URI
aliases or payload Data. Reservations are immutable and globally unique inside
one Mapping version. A name committed too late for future retrieval remains
valid ordinary retrieval but earns no ahead-prefetch credit.

`LiveStreamPublisher` owns Mapping encoding/signing/publication, route
registration, bounded reverse/pending/retention state, materialize-once payload
Data signing, optional FEC generation over opaque bytes, and lifecycle status.
The application owns name construction, payload production, encryption, sample
boundaries, decoder, bitrate, and UI.

Creation enters `Preparing`. Warm-up may reserve and publish bounded items, but
no descriptor is visible until `activate(readiness)` supplies a positive
measured period, an application-safe join cursor, Mapping coverage, and valid
frontiers. Activation is atomic; failure wipes tentative state. Each successful
`activate()` returns a fresh immutable checkpoint that includes every committed
reservation and produced item at that instant; repeated activation never moves
any frontier backward. `maxNameReservations` is a lifetime bound for one
Mapping version, while `retainedItems` bounds materialized payload retention.

## Optional FEC Contract

FEC is disabled by default. Contract v1 supports `None` and one bounded XOR
repair symbol for a group; a general erasure-code SPI is out of scope. FEC
operates on the exact opaque bytes supplied by the application. It cannot see
plaintext and cannot replace application AEAD.

For an enabled group:

1. The application reserves all semantic source and repair names before
   production and then supplies the already-protected source byte strings.
2. NDNSF pads only for the XOR calculation; original lengths are preserved.
3. Canonical FEC metadata inside each Provider-signed repair Data binds the
   group/session, source and repair names/cursors, original lengths, and SHA-256
   of every source opaque value. Its remaining Content is coded bytes, not a
   nested source Data packet or plaintext.
4. Every received source, repair, and FEC-control Data is exact-name and
   Provider-validated. Recovered opaque bytes are accepted only when their
   length and signed digest match.
5. A recovered item is local delivery evidence with
   `provenance=FecRecovered`; it is never inserted into a cache or republished
   as the missing original signed Data. The application still authenticates and
   decrypts the recovered bytes.
6. Malformed groups, inconsistent lengths/digests, more than one missing source,
   wire-size overflow, or expired recovery budget fail closed and fall through
   to bounded exact-name retransmission/skip policy.

This preserves NDN provenance without pretending that locally reconstructed
bytes carry the missing Data packet's signature: trust comes from the
descriptor-pinned Provider signature on the FEC metadata and the bound source
digest, followed by any application AEAD check.

## Consumer API

Conceptual C++ surface:

```cpp
LiveStreamOpenOptions options{
  .start = LiveStreamStart::Latest,
  .prefetchPolicy = LiveStreamPrefetchPolicy::MappedPressure,
  .aggregateInterestLimit = 64,
  .enableFecRecovery = true, // harmless when the descriptor says None
  .onItem = [] (const VerifiedLiveStreamItem& item) {
    // item.content is opaque; UAV decrypts and validates it here.
    // item.provenance is SignedData or FecRecovered.
    return LiveStreamItemAdmission::acceptItem();
  },
  .onStatus = [] (const LiveStreamStatus& status) { /* UI/telemetry */ },
};

auto stream = user.openLiveStream(validatedDescriptor, options);
stream.start();
stream.observeAcceptedSample(sampleObservation);
stream.status();
stream.stop();
```

Conceptual Python surface:

```python
stream = user.open_live_stream(
    descriptor,
    start="latest",
    prefetch_policy="mapped-pressure",
    enable_fec_recovery=True,
    on_item=decrypt_and_consume_opaque_item,
    on_status=show_stream_status,
)
stream.start()
```

`LiveStreamConsumerHandle` owns exact Mapping retrieval and Provider validation,
exact semantic payload/repair Interests, one aggregate Interest budget,
Chasing/Adjusting/Fetching/Recovering, burst/withhold, bounded retransmission,
optional FEC recovery, cancellation, and status diagnostics.

Three explicit policy identities support rollback and the frozen evaluation:

- `mapped-pressure`: the pre-Spec-119 pressure controller, with no mapped-live
  future pipeline;
- `mapped-live-v1-future-on`: the mapped-live controller with bounded future
  semantic-name Interests;
- `mapped-live-v1-future-off`: the same mapped-live controller but only the
  current cursor is scheduled, isolating the value and cost of lookahead.

All three share the same descriptor, Mapping validation, payload admission,
retry, and callback path. The default is selected by the Spec 119 matched
campaign, not by undocumented tuning.

The callback runs only after exact name/session/Mapping/Provider admission or
verified FEC reconstruction. Application decryption and payload validation
happen in `onItem`. Item acceptance updates item completion only. A multi-item
application calls `observeAcceptedSample(...)` after its application sample is
usable; only this observation updates live-edge and sample-demand estimates.
Rejected or decrypt-invalid items update no estimator.

## Paper And NDN Invariants

1. The receiver keeps multiple future exact-name Interests in flight; it does
   not emulate push or rediscover every item.
2. A measured producer sample period separates cached bursts from live arrival.
3. Pipeline demand covers retrieval delay versus production period and applies
   bounded burst/withhold and congestion feedback.
4. Every payload and repair Data has one meaningful immutable semantic NDN name
   and is independently signed. Only Mapping/control names are predictable.
5. Mapping committed after payload production is retrieval evidence, not a
   successful prefetch prediction.
6. FEC is an optional recovery tool inside the same exact-name pipeline, not a
   second transport, encryption scheme, or excuse to accept unsigned bytes.

## Failure Semantics

Missing or invalid descriptor, Mapping, signature, FEC metadata, session, name
authority, digest, or application admission pauses or closes the handle by
error class. No API invents an original name or falls back to plaintext. All
Mapping, FEC, cache, pending, retransmission, and observation structures are
bounded. Mapping exhaustion/invalidity is terminal because the semantic name
cannot be guessed. A mapped payload is attempted at most three times and then
explicitly skipped so one unavailable item cannot stall the live cursor
forever. Interest lifetime includes the bounded predicted generation horizon
and is capped at 30 seconds. `stop()` is idempotent and suppresses callbacks
from retired sessions.

`LiveStreamStatus` reports lifecycle/frontiers, retained and pending state,
aggregate in-flight work, accepted/rejected/recovered items, timeout/Nack
counts, Mapping/payload/future-payload Interest counts, Mapping bytes, and
Provider Interest-before-production opportunities/hits. These are operational
facts only: they never contain keys, plaintext, nonce material, or recovered
payload bytes.
