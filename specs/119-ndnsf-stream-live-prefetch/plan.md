# Implementation Plan: NDNSF Stream Live Prefetch

**Branch**: current working branch; no branch switch required | **Date**: 2026-07-17 | **Spec**: [spec.md](spec.md)

**Input**: `specs/119-ndnsf-stream-live-prefetch/spec.md`

## Summary

Evolve the existing `StreamAdaptiveFetcherState` from a pressure-only window
calculator into one bounded, sample-aware live prefetch controller, and add a
generic `StreamNameMap` codec/resolver beside it. Payload Data keeps the
application's original semantic NDN name. A monotonically increasing
`StreamCursor` is only an internal ordering coordinate. The Provider publishes
signed, immutable, bounded mapping blocks before advertising a prefetch
horizon, allowing the consumer to resolve future cursors into exact original
names before expressing Interests. The controller uses the authenticated
five Mapping/payload frontiers, producer sample period, adjacent-window live-edge detection,
retrieval-delay demand, multiplicative chase/adjust actions, variable
items-per-sample estimation, and playout-aware recovery advice.

This design is based on Gusev et al.'s receiver-driven real-time NDN pipeline,
but replaces selector bootstrap with the existing NDNSF Stream descriptor and
uses declared sample arrivals—not raw packet arrivals—for live-edge detection.

## Technical Context

**Language/Version**: C++17 core/UAV runtime and Python 3 bindings/tests

**Primary Dependencies**: ndn-cxx 0.9.0, existing `Stream.hpp/.cpp`, existing
UAV `VideoPacket`/`StreamChunk` bridge, NFD PIT/cache behavior, Spec 118
signature/session/decryption admission

**Storage**: Volatile bounded receiver and producer state only

**Testing**: Boost.Test deterministic trace vectors, Python parity tests, UAV
protocol tests, and matched MiniNDN campaigns

**Target Platform**: Linux and MiniNDN; real UAV and container validation are
deferred

**Project Type**: C++ framework plus UAV application and Python bindings

**Performance Goals**: Reduce p95 live lag or timeout/Nack load without more
than 5% regression in the other metric, while preserving completion and
bounded pending state

**Constraints**: Original application names are the actual Data names; cursors
never become Data names; mapping contains names/control metadata but no nested
Data or media payload; mapping must be signed, immutable, bounded, and
available ahead of the advertised horizon; exact-name Interests only; no
unsynchronized wall-clock subtraction; no unvalidated observation; no codec or
rendition choice in Core; 60-second measured MiniNDN cells

**Scale/Scope**: One controller and one bounded mapping cache per active Stream
session; current 600-packet producer retention; bounded 4-640 packet consumer
windows, bounded mapping blocks, and explicit producer pending/lookahead caps

## Constitution Check

- **Canonical Dynamic Runtime**: PASS. Camera start remains the existing unified
  NDNSF service and continuous Data remains outside a second RPC protocol.
- **Security Is Part Of The Data Path**: PASS. Estimators accept only admitted
  Data; Spec 118 remains the authority for signer/session/decryption checks.
- **CodeGraph First**: PASS. Core policy, Python wrapper, UAV producer pending
  table, exact Interest pump, publication metadata, and tests were source-verified.
- **Spec-Driven Durable Change**: PASS. Protocol behavior, ownership, migration,
  experiments, and tasks are captured here.
- **Right-Scope Verification**: PASS. Deterministic parity precedes matched
  MiniNDN loss/path campaigns; no real-UAV claim is made.
- **Cohesive Tasks**: PASS. Each behavior keeps test-first implementation and
  its acceptance evidence together.

Post-design re-check: PASS. No constitution exception is required.

## Current-Code Baseline

```text
Core StreamAdaptiveFetcherState
  already has: pressure calculations plus a partial live phase/period prototype
  limitation: prototype is not wired to a semantic-name map, complete recovery,
              aggregate Mapping/payload congestion, or the UAV runtime

UAV Ground Station
  currently predicts globally increasing /<stream-prefix>/<packetSeq>
  currently limits future probes using advertised packet count + lookahead
  computes a separate video pressure policy
  limitation: packet sequence is embedded in the Data name, so application
              names cannot remain authoritative; future-probe timeouts do not distinguish
              cache chasing, not-yet-produced Data, path change, or loss

UAV Provider
  retains 600 packet payloads
  stores an unsatisfied exact Interest in m_pending until that Data is produced
  limitation: no explicit distance/total cap on future pending names

Core mapping support
  current state: absent
  required state: signed bounded cursor-to-original-name blocks, deterministic
                  resolver behavior, predeclared tombstones, immutable unused
                  predictions, and reverse lookup
```

## Accepted Architecture

### 1. One Core Decision Engine

`StreamAdaptiveFetcherState` remains the only generic algorithm. Existing
pressure fields and `decide()` callers remain source-compatible. New live mode
is activated only after an application resets the state with a positive sample
period and validated Mapping context/five frontiers. Without them, the same object returns
the bounded pressure-only rollback decision and never claims a live edge.

The pure controller remains independent of `ndn::Face`, but Spec 119 also adds
one thin NDNSF network facade around it. `LiveStreamPublisher` owns Mapping
signing/publication, prefix registration, payload materialization and bounded
producer pending state. `LiveStreamConsumerHandle` owns Mapping/payload exact
Interest expression, trust validation, timers, cancellation, and lifecycle.
The application still owns semantic-name construction, explicit ahead
reservation, payload-specific admission, encryption/decryption, sample meaning,
decoder, bitrate, and UI. The facade also owns a default-off bounded XOR option
over opaque bytes; it never receives keys or plaintext. There is one controller
and one network facade, not two policy implementations.

### 2. Sample-Aware State Machine

```text
Inactive
  -> Chasing       validated session + mapping frontier + sample period
Chasing
  -> Adjusting     K consecutive stable adjacent-window observations
Adjusting
  -> Fetching      window converges to measured demand while stability holds
Fetching
  -> Adjusting     buffer shortage, delay step, stability loss, or congestion
Fetching/Adjusting
  -> Recovering    incomplete sample reaches recovery checkpoint
Recovering
  -> Fetching      optional opaque-byte FEC/retransmit/skip completes
Any
  -> Inactive      session replacement/reset
Any
  -> Stopped       explicit stop
```

One arrival observation is recorded only once per admitted application sample.
Current UAV `frameSeq` denotes a publication/FEC group formed from H264 byte
chunks, shard count, or timeout; it is not proven to be an encoded video frame.
The initial integration therefore declares that publication group as the sample
unit and measures its actual production period at the Provider. Configured FPS
is not substituted for that period. If sample identifiers skip, the observed
arrival delta is divided by identifier distance. Other packets update item
completion but not live-edge timing.

### 3. Bootstrap Without Selectors

The application start/status response supplies:

```text
contract version and collision-resistant stream/session identity
versioned Provider-owned payload prefix and exact Mapping root
Mapping anchor block and canonical-Content digest at the safe join point
latest join and produced cursors
mapping committed-through, oldest retained, and next reserved cursors
fixed Mapping block capacity
declared sample unit and its measured period
ahead-mapped or retrieval-only eligibility
```

Latest-mode begins at the application-proven safe join cursor, the first cursor
of a complete decoder-reset sequence behind the newest produced cursor. Core does
not interpret codec semantics. Beginning-mode begins at an explicit retained cursor. The
consumer first fetches and verifies the corresponding predictable mapping
block, resolves the cursor to the original semantic Data name, and only then
expresses the exact payload Interest. Neither mode uses rightmost-child
selectors or `CanBePrefix`.

For the UAV adapter, success follows a bounded warm-up of at least three
completed publication groups. The APP measures that unit's period and proves a
join sequence begins at an H264 access-unit boundary and includes required
parameter sets plus IDR. No data/no safe join before the service readiness
deadline fails camera start; configured FPS and the current synthetic
`keyChunk` flag are not accepted as evidence.

The paper's rightmost-child bootstrap is rejected because a nearby cache may
return an older rightmost child, selectors do not reliably enumerate a
high-rate sequence, and NDNSF already has an authenticated control response.

### 4. Live-Edge Detector

The detector keeps two adjacent non-overlapping windows of sample inter-arrival
periods. Gusev et al. print Eq. (3) with `m1/m2 <= theta1` and a second
similarity inequality that conflicts with the surrounding prose when arrival
period approaches `T`. That literal equation and its low/medium/high parameter
sets are retained only as reproduction fixtures.

The NDNSF controller uses a separately named and parameterized stability rule.
For previous/new means `m_old`/`m_new`, sample period `T`, relative-change
threshold `thetaChange`, and minimum period similarity `thetaSimilarity`:

```text
abs(m_new - m_old) / max(m_old, epsilon) <= thetaChange
1 - abs(m_new - T) / T >= thetaSimilarity
```

The result must hold for `K` consecutive evaluations. Seed profiles are NDNSF
profiles, not paper implementations:

| Profile | thetaChange | thetaSimilarity | N | K |
|---|---:|---:|---:|---:|
| ndnsf-fast-seed | 0.60 | 0.50 | 3 | 4 |
| ndnsf-balanced-seed | 0.30 | 0.70 | 10 | 4 |
| ndnsf-conservative-seed | 0.10 | 0.95 | 30 | 4 |

`ndnsf-conservative-seed` is only the initial validation candidate because the
paper observed false live-edge detection from its lower-precision setting in
multi-hop topologies. No default claim is made before MiniNDN evidence, and
every decision reports formula/profile identity.

### 5. Pipeline Demand And Hysteresis

Network sample demand is:

```text
sampleDemand = max(1, ceil(networkRetrievalDelay / samplePeriod))
packetDemand = sampleDemand * estimatedSegmentsPerSample + recoveryReserve
```

`networkRetrievalDelay` is derived from consumer-monotonic Interest-to-Data
observations. When an application can identify Data known to exist at Interest
expression, those observations update a bounded EWMA/upper-percentile network
estimate. Otherwise the controller may use a bounded minimum of recent
effective retrieval delays to avoid counting future-generation wait as network
delay, while Chasing remains responsible for correcting an underestimate. It
never subtracts producer wall time from consumer wall time. An explicit
same-clock generation-delay observation may be supplied only when the
application can prove its validity.

During Chasing, one allowed action may double the packet window. During
Adjusting, one action may reduce it to three quarters. After either action the
controller holds further multiplicative changes for at least one detection
period. All results are clamped to window, lookahead, Interest-lifetime, and
pending-state limits. A single aggregate congestion window caps Mapping,
payload, and retransmission Interests; Mapping has a bounded reserve but may not
starve payload. A Congestion Nack or non-zero CongestionMark triggers bounded
multiplicative decrease and hold. Timeout is loss evidence only for a name known
to have been produced; waiting for future generation is not congestion.

Interest lifetime covers:

```text
network delay + future sample horizon + bounded jitter margin
```

so a valid future Interest is not mistaken for loss merely because its Data has
not yet been generated.

### 6. Cursor Reservation And Signed Name Mapping

Core maintains a bounded item-count estimator updated only by authenticated,
complete-sample boundaries. It may keep application-provided classes, such as
key/delta, but does not understand those labels. The application chooses the
class and predicted value for the next sample.

For each predicted sample, the Provider reserves bounded cursors and publishes
canonical `StreamNameMapBlock` objects under:

```text
/<provider>/NDNSF/STREAM-MAP/<stream-id>/
  v=<mapping-version>/seq=<block-number>
```

For capacity `B`, `block-number=floor(cursor/B)` and slot=`cursor mod B`.
Every block covers exactly `B` cursors, is sealed gap-free, fits in one signed
Data under the configured and NDN wire cap, uses `ContentType=Manifest`, and has
no `FinalBlockId`. Version and block number are typed NDN components. A required
SHA-256 chain over canonical block Content detects forks around the descriptor-
pinned checkpoint. Core installs a whole verified block atomically; it never
interprets `seq=` as segmented-object `seg=`.

The Provider retains every Mapping block needed to verify the advertised
payload-retention interval through that checkpoint. A required Mapping-block
eviction atomically advances `oldestRetained` beyond the block and makes the
covered older payload unavailable; the descriptor never relies on an NFD cache
copy to make beginning-mode executable.

Each entry binds one cursor exactly once to an application-provided globally
immutable original Data name or a tombstone known before signing. The complete
name must be below a registered Provider-owned prefix and known before payload
production to be ahead-mapped. A Mapping block is immutable, contains no nested
NDN Data or media payload, and advances the frontier only after sealing.

For UAV's variable packet count:

- predicted count reserves a bounded cursor range and freezes complete names;
- the application supplies the original semantic name for each real segment;
- underestimation allocates later cursors/blocks, records Mapping lateness, and
  is ordinary retrieval rather than future-prefetch success;
- overestimated published names remain bound. Once authenticated actual extent
  is known, extra Interests are cancelled/expire as `terminal-unproduced`; the
  cursors are not reused and mappings are not rewritten as tombstones;
- tombstones exist only when sealed before any name binding for that cursor;
- a late but valid mapping permits ordinary retrieval, but receives no future
  prefetch-success credit.

The resolver rejects overlapping blocks, remapping, version mismatch, duplicate
cursor bindings, unbounded names, and gaps inside an advertised committed
frontier. A missing or unverified mapping pauses payload scheduling; it never
falls back to `/<stream-prefix>/<packetSeq>`.

### 7. Recovery, Opaque Content, And Application Policy

The public facade has a hard crypto boundary:

```text
APP protect -> opaque bytes -> Core prefetch/FEC -> opaque bytes -> APP unprotect
```

Core exposes no cipher, key, nonce, encrypt, or decrypt parameter. FEC is
default-off. `reserveGroup`/`publishGroup` reserves all application-provided
semantic source/repair names ahead and optionally creates one XOR repair over
the supplied opaque source bytes. This bounded v1 option is intentionally
smaller than a general codec SPI.

XOR padding never changes the source values. Provider-signed group metadata
binds session/group, source and repair names/cursors, exact lengths, and SHA-256
of each source opaque value. A recovered value is delivered locally with
`FecRecovered` provenance only after those checks; it is never cached or
republished as the missing original signed Data. The application then performs
its own AEAD/decode admission. More than one missing source, malformed metadata,
digest/length mismatch, deadline expiry, or wire-size overflow fails closed.

Core schedules this optional recovery before one bounded exact-name
retransmission/skip decision inside the remaining budget. Core reports
delay-step, congestion, or stability-loss evidence. UAV retains all encryption,
bitrate, encoder, decoder, and media-sample decisions.

### 8. Public LiveStream API

The normative surface is frozen in `contracts/live-stream-api.md`:

```cpp
auto publisher = provider.createLiveStream(definition);
auto reservation = publisher.reserveAhead(originalName);
publisher.publish(reservation, opaqueProtectedBytes);
auto fecGroup = publisher.reserveGroup(sourceNames, repairNames);
publisher.publishGroup(fecGroup, opaqueProtectedSources);
auto descriptor = publisher.activate(readiness);

auto stream = user.openLiveStream(validatedDescriptor, openOptions);
stream.start();
stream.observeAcceptedSample(sampleObservation);
stream.status();
stream.stop();
```

The Provider API makes the necessary two-stage operation explicit: a name must
be reserved and committed to Mapping before payload production to qualify for
future prefetch. Creation remains `Preparing` while warm-up measures the sample
period and safe join; only atomic `activate(readiness)` exposes a descriptor.
The Consumer API hides Mapping names, Face calls, timers,
window changes, retries, and cancellation behind one handle. Its callback sees
only Provider-validated original-name Data. Application rejection—for example
UAV AEAD failure—is diagnostic-only and cannot update controller state.
Multi-item applications explicitly report a complete accepted sample after item
admission; one-item streams may use the built-in adapter.

Python binds the same native objects and wire. Normal/Targeted Request/Response
remains the way an application obtains a descriptor and application-specific
key material, but the continuous stream is independent of request lifetime.

### 9. Security And Resource Admission

Mapping and payload admission are separate ordered gates. The NDNSF facade
verifies Mapping and payload signatures, Provider authority, Mapping version,
block bounds, immutability, cursor binding, and exact semantic name before
delivery. The application then performs payload-specific checks such as Spec
118 decryption/replay validation. Only an accepted callback result may update
sample estimators. Invalid Mapping, payload, or callback results are counted
separately and cannot change any estimator.

The descriptor pins the expected Provider identity, payload prefix, Mapping
root/version, and checkpoint block/digest covering latest join. Separate
validator rules bind each root to that same
identity under the configured Trust Anchor. A different valid Provider signer,
old cached version, same-name equivocation, or continuity fork closes the
session. A signature proves provenance; it does not by itself authorize a name.

The UAV producer reverse-resolves an exact semantic payload Interest through
its committed mapping state and accepts it as a pending future Interest only
when:

- the name is bound to exactly one cursor in the active Mapping version;
- the cursor is not behind evicted retention or beyond
  `currentCursor + maxAhead`;
- the pending map is below its cap;
- its local expiry, derived from the Interest lifetime, is still in the future.

Each cursor occupies one deduplicated APP entry. Nearest cursor/earliest deadline
has priority and may evict the farthest future entry. Expired entries are removed
with bounded incremental cleanup; rejection is observable and immediately
Nacked without per-name allocation. NFD has already created PIT state before
the callback, so deployment—not this APP table—owns ingress rate,
Interest-lifetime, and forwarder resource safeguards. Campaign evidence records
those settings and both LiveStream-handle pending/NFD PIT maxima.

Mapping and payload exact Interests use `CanBePrefix=false` and no
ApplicationParameters. Freshness/MustBeFresh affect cache selection, never
authorization or replay. PIT aggregation is an optional optimization; all caps
and correctness hold without it.

Mapping names are visible control metadata. They are signed for integrity and
authority but not encrypted. Ahead Mapping additionally exposes batches of
future names, production volume/timing, and unused predictions earlier than
normal Interests. Applications must declare names non-sensitive; payload
confidentiality remains Spec 118's responsibility.

## Project Structure

```text
specs/119-ndnsf-stream-live-prefetch/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── contracts/
│   ├── live-prefetch-controller.md
│   └── live-stream-api.md
├── quickstart.md
├── tasks.md
└── traceability.md

ndn-service-framework/
├── Stream.hpp/.cpp
├── ServiceProvider.hpp/.cpp
└── ServiceUser.hpp/.cpp

pythonWrapper/
├── src/ndnsf/_ndnsf.cpp
└── ndnsf/streaming.py

NDNSF-UAV-APP/
├── shared/UavProtocol.hpp/.cpp
├── drone/DroneServiceContainer.inc.hpp
└── ground-station/GroundStationServiceContainer.inc.hpp

tests/
├── fixtures/stream-prefetch/
├── unit-tests/stream.t.cpp
├── unit-tests/uav-protocol-state.t.cpp
└── python/test_ndnsf_core_streaming.py
```

**Structure Decision**: Extend `Stream` plus the existing ServiceProvider/User
network owners. Add lifecycle classes, not another service invocation mode or
codec-specific policy engine. The Face event loop remains the existing NDNSF
event loop; the API adds no independent scheduler thread.

## Delivery And Rollback

1. Implement the generic cursor/Mapping codec, resolver, bounds, predeclared tombstones,
   reverse lookup, and deterministic C++/Python parity vectors (Spec 119 T001).
2. Complete the Core controller and public C++/Python
   `LiveStreamPublisher`/`LiveStreamConsumerHandle` facade. Prove it first with
   an app-neutral semantic-name producer/consumer MiniNDN regression, including
   no-FEC and opaque-byte XOR recovery cases.
3. Use that complete foundation in Spec 118. UAV supplies semantic names,
   ciphertext, sample admission, and decoding through the public API; when
   enabled it selects Core's opaque-byte FEC option instead of maintaining a
   second XOR transport. It does not own Mapping routes, Face scheduling,
   producer pending state, or generic FEC recovery.
4. Compare three predeclared controls in matched MiniNDN campaigns:
   `mapped-pressure`; `mapped-live-v1-future-on`;
   and `mapped-live-v1-future-off`, which uses the same controller and wire but
   schedules only the current cursor rather than a future lookahead. The future
   toggle estimates Provider pending benefit without introducing a second APP
   network loop. Future-on must have a nonzero eligible denominator and at
   least 99% Provider Interest-before-production hits.
   Record map lead/bytes, aggregate in-flight, NFD PIT, and Provider
   Interest-before-production events.

The pressure-only branch remains inside the same controller as a bounded
rollback during validation. Its mode and comparative counters are visible. It
is removed as an operational choice or retained only as diagnostics after the
matched campaign passes every required repetition; a negative result keeps it
default and is documented.

## Complexity Tracking

No constitution violations are introduced. Mapping is conditionally justified
for semantic names that cannot be derived from cursor plus descriptor; directly
constructible application names are simpler and form the experiment control.
The production generic contract chooses one bounded indirection rather than a
second payload protocol or nested Data envelope. Mapping cost is reported
directly as bytes and Interest share; an APP-only direct oracle was rejected
because it would duplicate validation, pending, and scheduling authority outside
Core. The state additions replace application duplication rather than create a
second generic policy engine.
