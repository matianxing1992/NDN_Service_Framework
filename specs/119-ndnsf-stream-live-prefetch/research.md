# Research: Receiver-Driven Live Prefetch For NDNSF Stream

## Primary Source

Peter Gusev et al., "Real-Time Streaming Data Delivery over Named Data
Networking," IEICE Transactions on Communications, 2016,
DOI `10.1587/transcom.2015AMI0002`.

Primary PDF:
https://irl.cs.ucla.edu/data/files/papers/gusev2016realtime.pdf

NDN protocol constraints were checked against the current packet specification,
naming conventions, and trust-schema guidance:

- https://docs.named-data.net/NDN-packet-spec/current/interest.html
- https://docs.named-data.net/NDN-packet-spec/current/data.html
- https://named-data.net/wp-content/uploads/2021/10/ndn-tr-22-3-ndn-memo-naming-conventions.pdf
- https://named-data.net/publications/techreports/ndn-0030-2-trust-schema/

These sources make Data identity/name immutability, exact Interest semantics,
typed Version/Sequence/Segment components, and signer-to-name authorization
normative for this design. Freshness is cache selection, not authorization or
permission to replace one Data name with new content.

## What The Paper Establishes

### Decision: expose one reusable NDNSF network API, not only a policy object

**Finding**: The paper separates producer cache/pending state, consumer
Interest Pipeline, Data Buffer, and application ADU processing, but treats
their coordination as one reusable receiver-driven architecture. Leaving Face,
Mapping, timers, and retries in every application would reproduce the same
transport logic and make UAV the de facto framework implementation.

**NDNSF adaptation**: Keep `StreamAdaptiveFetcherState` pure, then compose it
with existing ServiceProvider/ServiceUser Face, validator, signer, and event-loop
owners as `LiveStreamPublisher` and `LiveStreamConsumerHandle`. Applications
provide original names and payload admission; NDNSF owns the generic network
mechanics. This is an implementation abstraction, not a claim that the 2016
paper specified this exact API.

### Decision: Keep future exact-name Interests outstanding without replacing application names

**Finding**: A consumer should learn the current sequence and producer rate,
then use predictable sequential exact names. Interests may arrive before Data
is produced and wait in the PIT. The paper explicitly identifies selector-only
streaming as unreliable for high-rate sequential Data.

**NDNSF adaptation**: Preserve the original, meaningful application name as
the actual Data name. Assign a consecutive `StreamCursor` only inside Stream
state. Before advertising a future cursor horizon, publish signed, immutable,
bounded `StreamNameMapBlock` control Data that resolves each cursor to the exact
original name or a predeclared tombstone. Consumers prefetch predictable map
blocks, then express exact Interests for the resolved names.

The paper itself does **not** use a cursor/name map: its payload names are
sequentially constructible. `StreamNameMap` is NDNSF's manifest/index-inspired
indirection for preserving semantic names that cannot be derived from a cursor.
Its extra Interest, wire bytes, PIT use, and privacy cost must be quantified.
This Spec does not automatically replace Mapping because its generic semantic-
name contract cannot assume every application has a reversible name grammar; a
material direct-name advantage is reported as negative Mapping-cost evidence and
opens a separate architecture decision rather than silently enabling a fallback.

**Rejected alternative**: Restore rightmost-child or `CanBePrefix` retrieval
for steady streaming. This would weaken predictability and can stop at stale
cache state.

**Rejected alternative**: Use `/<stream-prefix>/<packetSeq>` as the payload
Data name. It makes Stream order replace the application's naming semantics.

**Rejected alternative**: Encapsulate a complete original NDN Data packet in a
second sequentially named Data packet. It duplicates Data envelopes,
signatures, validation context, and caching identity. Mapping contains only
names and bounded control metadata.

### Decision: Mapping must lead production and remain immutable

**Finding**: Future Interest prefetch works only when the exact future name is
known before the payload is produced. A mapping learned after production adds
another round trip and no longer provides the paper's prefetch benefit.

**NDNSF use**: The Provider commits signed map blocks before advancing the
advertised frontier. A cursor is bound once to a name or predeclared tombstone
and cannot be reused or remapped. Underprediction allocates later cursors and is
recorded as late ordinary retrieval. When prediction is too large, published
name bindings remain immutable; after authenticated actual extent arrives, the
extra Interests are cancelled or expire as `terminal-unproduced`.

**Boundary**: A late but valid mapping still supports ordinary retrieval, but
it receives no prefetch-success credit. Missing or unverified mapping pauses
payload scheduling instead of falling back to a synthetic sequence name.

### Decision: Use fixed, single-Data Mapping blocks

**Finding**: A consumer can prefetch a Mapping only if cursor-to-control-name is
deterministic. Variable entry counts or segmented Map objects would require
another index and ambiguity around `FinalBlockId`.

**NDNSF use**: Choose fixed capacity `B`; block `floor(cursor/B)` covers one
fixed range and uses typed Version/SequenceNum components. One block is one
signed `ContentType=Manifest` Data packet below the configured and NDN wire cap,
with no `FinalBlockId`. The descriptor pins a join anchor digest and later
canonical Contents form a required SHA-256 chain.

**Rejected alternative**: Segment a Mapping block. That adds another discovery/
reassembly protocol and should be considered only if measured semantic names
cannot fit a useful fixed block under the NDN packet limit.

### Decision: Add Chasing, Adjusting, and Fetching phases

**Finding**: Cached stale Data follows Interest bursts and arrives irregularly;
current live Data arrives near the producer sample period. The paper doubles
the pipeline while chasing and reduces it to three quarters while adjusting,
waiting a detection period between changes.

**NDNSF use**: Add explicit phases and bounded multiplicative actions to the
existing Core state. Preserve hard caps and pressure feedback.

**Rejected alternative**: Apply the multipliers on every callback. That would
oscillate and turn future Data waiting into false congestion.

### Decision: Detect the UAV live edge per declared production sample

**Finding**: The paper's stability estimator assumes one arrival period per
producer sample. NDN-RTC frames can contain several segments and uses frame
metadata plus a segment-count estimator.

**NDNSF adaptation**: Current UAV code groups H264 byte chunks by FEC shard
count or timeout; `frameSeq` is not proven to be an encoded frame. The initial
sample is therefore that publication/FEC group. The Provider measures and
advertises the period for this exact unit. Only its first accepted packet
updates sample-arrival timing; other packets update completion/loss state.

**Rejected alternative**: Feed every video packet to the live-edge detector.
Frame-internal bursts would violate the estimator's sampling assumption and
could create false edge decisions.

### Decision: Derive demand from delay and sample period

**Finding**: The paper defines minimum Interest demand as
`ceil(DRDest / T)`, where `T` is the producer period and `DRDest` excludes
generation wait. Too few Interests fall behind; too many future Interests time
out.

**NDNSF use**: Compute sample demand from a consumer-monotonic network-delay
estimate, then multiply by learned packets per sample.

**Adaptation**: The current Data packet cannot safely carry a consumer-specific
generation wait and producer/consumer wall clocks are not guaranteed
synchronized. Therefore Core accepts a generation-delay value only when an
application proves a same-clock measurement. Otherwise it uses known-produced
samples or a bounded low-percentile/minimum effective retrieval-delay estimate.

**Rejected alternative**: Subtract `captureMs` from consumer arrival time. That
would silently turn clock offset into pipeline demand.

### Decision: Learn variable packets per sample

**Finding**: NDN-RTC predicts segment count separately for frame types; actual
count in segment metadata triggers more Interests when underestimated. Some
unanswered speculative Interests are preferred to another round trip.

**NDNSF use**: Add a generic bounded item-count estimator keyed by optional
application class. Reserve predicted names/cursors ahead. If actual count is
larger, allocate later cursors and fetch them without prefetch credit. If actual
count is smaller, cancel/expire the unused predicted Interests but retain the
immutable name bindings and never reuse those cursors. Tombstones apply only to
slots declared empty before the signed block is published.

**Boundary**: Core does not define key frames, codecs, or original payload-name
policy. UAV supplies any class label and original names; Core owns only cursor
order and generic mapping resolution.

### Decision: Tie recovery to playout budget

**Finding**: The paper sizes playable/reserved buffers using jitter and
retrieval delay and checks incomplete samples at a retransmission checkpoint.
It attempts FEC before retransmission.

**NDNSF use**: Core provides a default-off bounded XOR option over opaque bytes,
then one bounded exact-name retransmission/skip decision. It does not receive
keys or parse media. Provider-signed group metadata binds semantic source/repair
names, lengths, and source digests; recovered bytes remain local callback input
and the application performs its own decrypt/admission. UAV keeps decoder,
sample, bitrate, and playout policy but removes its duplicate XOR transport.

**Rejected alternative**: Treat Interest timeout as the only loss signal. A
timeout may represent future generation, and a late retransmission may miss the
playout deadline.

### Decision: Keep bitrate selection outside Core

**Finding**: The paper also adapts Interest rate/bitrate using delay variation,
loss, and higher-rate challenges.

**NDNSF boundary**: Core reports congestion and path-change evidence. The UAV
application chooses bitrate and whether to probe another rendition. This avoids
putting video policy into the generic Stream substrate.

## Evidence Limits From The Paper

- The low-precision detector started faster but produced false live-edge
  detection in more complex multi-hop topologies. Thresholds need validation.
- 8000-byte payloads performed poorly at 5% and 10% loss; lower-layer
  fragmentation and retransmission were suspected. NDNSF keeps its smaller
  application packets and does not generalize a larger segment as beneficial.
- Per-frame FEC was ineffective for one-segment frames in that implementation.
  The generic API proves only correctness and boundedness of optional recovery;
  NDNSF preserves its own previously observed FEC results and makes no new UAV
  performance-improvement claim.
- Available bandwidth utilization fell as RTT increased in the paper's tests,
  so delay/path-change scenarios belong in validation.
- The paper leaves security concerns largely outside its design. NDNSF requires
  an admitted Provider-signed mapping followed by admitted, signed,
  namespace-correct, session-correct, replay-safe, and encrypted payload Data
  before an observation influences state.
- The name map adds a control fetch before payload prefetch. Therefore mapping
  lead ratio, late mappings, mapping starvation, and map bytes are acceptance
  metrics rather than assumed overhead-free behavior.
- The printed Eq. (3) uses `m1/m2 <= theta1` and prints a second inequality that
  conflicts with the prose claim of similarity near `T`. Literal-paper
  reproduction and NDNSF's separately named stability rule must not share
  profile labels or thresholds without explanation.
- The paper predicts `M` segments and explicitly permits excess Interests to go
  unanswered when actual `N < M`; it does not authorize mutating a published
  name binding into a tombstone.
- The paper leaves namespace security and encryption for future work. Provider
  identity/name trust, immutable cross-session names, and Spec 118 encryption
  are NDNSF mechanisms, not paper results.
- Ahead Mapping reveals future names and batch/timing information earlier than
  ordinary payload Interests. Names must be non-sensitive; the design does not
  claim traffic-analysis confidentiality.

## Code-Reality Findings

- `StreamAdaptiveFetcherState` currently computes only pressure-scaled window,
  lookahead, Interest lifetime, and missing timeout.
- The UAV ground station currently expresses predictable sequence-derived
  packet names and limits future probes, but has no semantic-name mapping and
  has a separate video policy with no live-edge phase.
- `VideoPacket` exposes fields named as frame identity/count/range, but current
  capture code forms publication/FEC groups from byte chunks, shard count, or
  timeout. Those labels are not evidence of H264 access-unit boundaries.
- The UAV provider already retains produced packets and holds future exact
  Interests, but it has no signed ahead-of-production name map and its pending
  map lacks explicit distance and total caps.
- Spec 118 is the controlling security dependency; it is designed but not yet
  implemented, so network acceptance for this feature cannot precede its Data
  admission gate.

## Experimental Decision

Use three matched Core policies rather than assuming paper parameters transfer:
`mapped-pressure`, `mapped-live-v1-future-on`, the same live controller with
future lookahead disabled (`mapped-live-v1-future-off`). Future-off schedules
only the current cursor; it does not create another APP network path. Future-on
requires a nonzero eligible-future denominator and at least 99% Provider-side
Interest-before-production hits. Mapping cost is reported directly as Mapping
bytes and Interest share. Every `pair_id` fixes source-trace digest, topology,
control schedule, and logging; a recorded seed randomizes/counter-balances
policy order. The installed `tc netem` has no reproducible seed option, so the
kernel loss sequence is treated as independent run noise rather than falsely
claimed as paired. Each cell has at least five fresh-process 60-second runs with
failures preserved. Report run-level effects/raw distributions; do not treat
per-sample observations as independent runs.
