# Contract: Live Prefetch Controller

## Ownership

The pure controller calculates bounded decisions and owns the generic
cursor/name-map codec, resolver, continuity rules, tombstones, and bounded
cache state. The public NDNSF facade in `live-stream-api.md` owns Mapping
publication, signature validation, Face calls, timers, and lifecycle by
composing this controller with existing ServiceProvider/ServiceUser resources.
The application creates original semantic NDN names and owns explicit ahead
reservation, payload-specific admission, sample semantics, encryption, FEC,
decoder, and bitrate selection.

## Configuration And Reset

Conceptual C++ surface:

```cpp
state.configure(config);
StreamNameMapResolverConfig mapConfig{/* frozen descriptor fields and bounds */};
StreamNameMapCheckpoint checkpoint{/* five frontiers, block, digest */};
resolver.reset(mapConfig, checkpoint);
state.reset(validatedBootstrapDescriptor);
state.stop();
```

`reset` is required for a new session. Live mode cannot start without a positive
period for the declared sample unit, contract/versioned roots, expected Provider,
and validated join/produced/committed/retained/reserved frontiers. Reset removes
all prior-session estimator and Mapping state.
An already initialized resolver accepts reset only for a different
`sessionEpoch`, and that new epoch also requires a new `mappingVersion` and a new
payload prefix. The payload prefix ends in a typed Version component equal to
`mappingVersion`; repeating the epoch or reusing either namespace is rejected
before old state is cleared. Recovering a fault therefore creates a new session
rather than forgetting same-session equivocation. A newly constructed resolver
still relies on the application-validated descriptor to supply
collision-resistant identities.
The application emits success only after bounded readiness establishes
`retained <= join <= produced <= committed < nextReserved`, a measured period,
and a checkpoint anchor covering join; Core receives no partial-success reset.
The configured verified-block and reverse-name capacities must cover the entire
advertised block interval from `oldestRetained` through
`mappingCommittedThrough`; Core rejects a descriptor that its bounds can never
make executable.

## Signed Name Mapping

The Provider publishes one signed Mapping Data packet for fixed capacity `B`:

```text
blockNo(C) = floor(C / B)
slot(C)    = C mod B
M(C)       = /<provider>/NDNSF/STREAM-MAP/<stream-id>/
             v=<mapping-version>/seq=<blockNo(C)>
```

`v=` and `seq=` are typed Version and SequenceNum components. A block covers
exactly `[B*blockNo, B*(blockNo+1)-1]`, uses `ContentType=Manifest`, has no
`FinalBlockId`, and its complete signed wire encoding is no larger than the
configured cap or `ndn::MAX_NDN_PACKET_SIZE`. Mapping Interests are exact,
`CanBePrefix=false`, and carry no ApplicationParameters.
`B` is frozen in the session descriptor after checking configured maximum Name
length plus Content/MetaInfo/signature overhead. If actual encoding still
exceeds the cap, the block is not published and the frontier does not advance;
capacity cannot silently change mid-session.

The frozen v1 Mapping Content wire is one `StreamNameMapBlock` (`0xF633`) with
this exact child order. Every required NonNegativeInteger is present even when
its value is zero; unknown, missing, duplicated, or reordered fields are
rejected as non-canonical. Canonical comparison is byte-for-byte over the full
TLV wire, including the outer Type and Length encodings.

| Field | TLV | v1 rule |
|---|---:|---|
| contract version | `0xF634` | required NNI, value `1` |
| stream ID | `0xF613` | required bounded byte string |
| session epoch | `0xF614` | required nonzero NNI |
| mapping version | `0xF635` | required nonzero NNI |
| block number | `0xF636` | required NNI, including zero |
| block capacity | `0xF637` | required fixed nonzero NNI |
| first cursor | `0xF638` | required NNI equal to `B*blockNo` |
| previous Content digest | `0xF639` | absent for block 0; otherwise exactly 32 bytes |
| entry | `0xF63A` | exactly `B`, in slot order |

Each entry contains exactly one child: a standard NDN `Name` TLV (`0x07`) or
an empty predeclared tombstone (`0xF63B`). URI strings, payload bytes, and
nested `Data` packets are not Mapping wire. The shared frozen vectors are in
`tests/fixtures/stream-prefetch/map-wire-v1.json`,
`resolver-traces-v1.json`, `map-rejections-v1.json`, and
`frontier-retention-v1.json`; C++ and Python consume the same files.

`contentDigest = SHA-256(canonical StreamNameMapBlock Content TLV bytes)`; the
outer Data Name, MetaInfo, SignatureInfo, and SignatureValue are not part of
that digest. The authenticated descriptor pins the checkpoint block/digest
containing its latest join cursor, and every non-genesis block carries the
preceding content digest.
There is no digest field hashed into itself.

After the application validates the Data signature and Provider authority, it
passes the exact envelope evidence and canonical Content to Core:

```cpp
VerifiedStreamNameMapData input;
input.dataName = data.getName();
input.verifiedProvider = validatedSignerIdentity;
input.contentType = data.getContentType();
input.hasFinalBlock = data.getFinalBlock().has_value();
input.signedWireSize = data.wireEncode().size();
input.content = data.getContent();
input.receivedMonotonicMs = receivedMonotonicMs;
input.requiredBeforeMonotonicMs = localPrefetchDeadlineMs;
const auto result = resolver.admitVerifiedBlock(input);
```

Python exposes the same native codec and resolver rather than a second wire
implementation:

```python
block = StreamNameMapBlock.decode(content_block_wire)
resolver = StreamNameResolver(config, checkpoint)
result = resolver.admit_verified_block(
    data_name=data_name,
    verified_provider=provider_identity,
    content=block.canonical_content(),
    signed_wire_size=signed_data_size,
)
```

Malformed Python Content bytes return the same structured
`malformed-or-noncanonical-content` rejection as C++; ordinary construction or
configuration errors still raise `ValueError`.

Admission requires exact root/version/block number, fixed capacity/range,
canonical Content, required previous-Content SHA-256 continuity except genesis,
no overlap/remap/reuse, and globally immutable original Names below the
descriptor-pinned routed prefix. The application first proves the signer is the
descriptor-pinned Provider identity under its Trust Anchor; another valid
Provider is rejected. Core then atomically installs the full verified block.
The block carries names/control metadata only, never nested NDN Data or media
payload. The Provider commits all covering blocks before advertising the
corresponding frontier. Same-name different-content or continuity fork closes
the session.
An identical retransmission of the same block is an idempotent duplicate, not
an overlap fault. `verifiedProvider` is application-produced evidence, not a
boolean that makes Core a signature validator; actual signing and Trust Schema
validation remain Spec 118 ownership.
Out-of-order blocks may occupy a bounded quarantine, but none of their entries
is resolvable until the descriptor anchor or immediately preceding verified
digest establishes continuity.
Latest/future retrieval validates forward from the checkpoint. Beginning-mode
for a retained cursor before the checkpoint starts from that authenticated
checkpoint and walks backward: each verified successor supplies the expected
digest of its predecessor. Traversal stops at the target block or
`oldestRetained`; quarantined older entries are not exposed before the reverse
chain closes. `mapping anchor` always means checkpoint, never genesis.
`oldestRetained` is therefore a joint payload-and-Mapping availability frontier:
the Provider must continue serving every block required to verify the interval
from that cursor through the checkpoint. Evicting a required Mapping block and
the covered payload availability is one atomic frontier update; cached copies in
NFD are not used to justify descriptor availability.
Consumer-local cache eviction is different: it may make a cursor unresolved
and trigger exact refetch, but it never advances the Provider's advertised
`oldestRetained`. Compact immutable digest/name reservations remain bounded in
Core. Block-digest reservations may retire after an application-verified
retention advance because the old block number is then stale. Original-name
reservations remain for the complete Mapping-version lifetime and are cleared
only by a valid namespace-changing reset; exhaustion of the configured bound
requires a new session rather than unsafe name reuse.

A checkpoint refresh may keep the same block only with the same digest. Moving
the checkpoint forward requires that the new anchor block already be admitted
and connected to the current authenticated anchor; otherwise Core returns
`checkpoint-anchor-not-verified` without changing state. Conflicting checkpoint
digests are fatal session equivocation. This makes frontier/checkpoint movement
an explicit verify-then-commit operation.

```cpp
const auto resolved = resolver.resolve(cursor);
const auto cursor = resolver.reverseResolve(originalName);
```

An unresolved, unverified, tombstoned, or ambiguous cursor/name produces no
payload scheduling or producer pending state. There is no fallback to a
sequence-derived payload name. A late later-cursor block is usable for ordinary
retrieval but ineligible for future-prefetch credit. An ahead-predicted name
that never produces Data remains bound and becomes `terminal-unproduced`; it is
never rewritten as a tombstone. Tombstone is legal only before block signing.

## Accepted Observations

```cpp
state.observeAcceptedSample(observation);
state.observeAcceptedPacket(observation);
state.recordTimeout(cursor, wasFuture);
state.recordNack(cursor, reason);
state.recordCongestionMark(cursor, mark);
state.setBacklog(playableMs, pendingPackets, pressure);
state.recordRecovery(result);
```

An application MUST call accepted methods only after the mapping block and
payload Data have independently passed all applicable admission checks.
`recordInvalidObservation(reason)` is diagnostic-only.

All times used in one calculation are durations from one monotonic clock.
Provider records map-published, Interest-arrived, and payload-produced ordering
locally; consumer records map-admitted, Interest-expressed, and Data-arrived
ordering locally. Evidence is correlated by session/cursor, never by subtracting
the two hosts' clocks.

## Decision

```cpp
const StreamFetchDecision decision = state.decide(nowMonotonicMs,
                                                   playoutDeadlineMs);
```

The pure decision contains cursor and Mapping ranges and performs no I/O. The
`LiveStreamConsumerHandle` consumes that decision, fetches predictable Mapping
blocks, resolves eligible cursors, and expresses exact Interests
(`CanBePrefix=false`, no ApplicationParameters) for the returned original
names. Applications do not duplicate that loop. Freshness/MustBeFresh are
object-specific cache policy, not authorization or replay checks. Correctness
does not assume PIT aggregation.

The initial policy uses `MustBeFresh=false` for immutable version-unique Mapping
and payload names so beginning-mode and in-network cache reuse continue to work.
Producer `FreshnessPeriod` remains a cache hint chosen per object/retention; an
expired freshness period never invalidates a signature or authorizes name reuse.

## State Invariants

1. Only an explicit reset changes session identity.
2. Each unique sample contributes at most one arrival-period observation; gaps
   are normalized by monotonic sample-identifier distance.
3. A live edge requires two full adjacent windows and K consecutive stable
   evaluations.
4. At most one burst/withhold action occurs per detection period.
5. All histories, class maps, windows, lookahead, lifetimes, and recovery counts
   are bounded.
6. Invalid/stale/replayed observations change no estimator.
7. No decision can authorize a name outside the application's active session or
   configured future cap.
8. One cursor binds once to one original name or predeclared tombstone; Mapping
   versions, blocks, and advertised frontiers never permit remapping.
9. Payload scheduling never advances beyond the verified gap-free mapping
   frontier.
10. Mapping, payload, and retransmission Interests share one aggregate
    in-flight budget; Mapping reserve cannot starve payload.
11. Freshness and PIT aggregation are optimizations only. Security depends on
    version-unique names, descriptor/session admission, trust, and replay state.
12. Published predicted names remain immutable even if no Data is produced;
    only pre-signing empty slots may be tombstones.

## UAV Mapping

| Controller concept | UAV source/owner |
|---|---|
| contract/session/Mapping version and five frontiers | validated camera-start response |
| original payload name | UAV application naming policy |
| cursor reservation and mapping codec | LiveStreamPublisher + Core mapping contract |
| signed mapping publication/validation | LiveStreamPublisher/ConsumerHandle |
| sample identity | publication/FEC group (`frameSeq` is not codec-frame proof) |
| sample period | Provider-measured period of that publication/FEC group |
| sample arrival | first admitted packet of a new publication group |
| actual cursor range | admitted mapping plus authenticated frame metadata |
| segment count | `frameSegmentCount` |
| exact retrieval name | verified cursor-to-original-name resolver |
| FEC result | optional LiveStream opaque-byte recovery status |
| playout/backlog | existing decoder queue |
| bitrate action | existing UAV adaptive policy |

## Producer Admission

`LiveStreamPublisher` owns reverse resolution, prefix registration, and bounded
pending-Interest admission. Spec 118 supplies only UAV semantic names and
encrypted content. The producer may serve retained exact Data immediately. A missing future
semantic name may be stored only if reverse lookup binds it to exactly one
non-tombstoned cursor in the active committed Mapping version, that cursor is
within `currentCursor + maxAhead`, the total pending cap is not reached, and
its Interest-lifetime-derived local expiry is valid. Unmapped, ambiguous, old,
expired, malformed, too-far, wrong-session/version, tombstoned, or over-cap names
are rejected without application allocation using an immediate bounded Nack.
Duplicate exact names/cursors share one handle entry; the publisher chooses smallest
cursor/earliest deadline first and may evict the farthest-future entry so a
valid far-future flood cannot starve live-edge work. Bounded cleanup removes
expired entries. Since NFD created PIT state before dispatch, deployment owns
ingress rate, Interest-lifetime and PIT-resource safeguards; evidence records
their configuration and both handle-pending/NFD-PIT maxima.

## Compatibility

Existing pressure-only calls continue to compile and remain bounded. The old
`/<stream-prefix>/<packetSeq>` payload-name contract is deliberately replaced;
mixed-mode fallback is forbidden because it creates two payload identities.
Applications carry contract version, roots/version/capacity, five frontiers,
sample unit/period, and eligibility in their protected descriptor. Legacy
`StreamInfo.chunk_name()` consumers fail closed for the new contract.
Pressure-only remains an algorithmic rollback over the same semantic-name/
Mapping wire contract, not a wire-format rollback. Mapping overhead is measured
from Core counters rather than by adding a second APP-owned direct transport.
