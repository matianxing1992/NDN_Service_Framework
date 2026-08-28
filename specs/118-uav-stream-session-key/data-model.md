# Data Model: UAV Stream Session-Key Delivery

## Spec 119 Handle Bindings

- Provider holds one `LiveStreamPublisher`; UAV semantic source/repair names are
  passed to `reserveAhead` or `reserveGroup`, and each immutable source
  reservation cursor/name is bound into AEAD before `publish`/`publishGroup`.
- Ground station holds one `LiveStreamConsumerHandle`; its verified-item
  callback receives opaque bytes with `SignedData` or `FecRecovered`
  provenance, decrypts and validates the VideoPacket, then returns accepted
  sample identity/completion or a rejection reason.
- Generic Mapping cache, reverse lookup, Face timers, Interest windows,
  pending/deduplication, FEC group/recovery, and controller state are not UAV
  data-model entities. They are observed through the Spec 119 status snapshot.

## VideoStreamSessionKey

Volatile Provider state for one active live camera session.

| Field | Rule |
|---|---|
| `streamId` | Non-empty current UAV Stream identifier |
| `providerIdentity` | Exact Drone identity signing Mapping and payload Data |
| `serviceName` | Exact camera-video control service |
| `sessionEpoch` | Non-zero current application session; increments on rekey even when StreamId is retained |
| `keyEpoch` | Non-zero; increases on rekey within the same StreamId |
| `key` | Exactly 32 random bytes; memory-only |
| `nonceSalt` | Exactly 4 random bytes; memory-only |
| `lastCursor` | Optional last committed cursor; strictly increasing |
| `createdAtMs` | Diagnostic timestamp, not cryptographic authority |

States:

```text
EMPTY -> GENERATING -> ACTIVE -> CLOSING -> CLEARED
                  \-> FAILED
ACTIVE -> REKEYING -> ACTIVE
```

No successful descriptor or video packet is published before `ACTIVE`.

## VideoStreamDescriptor

Application fields inside one successful encrypted camera-start Response.

| Field | Wire form | Validation |
|---|---|---|
| `stream_id` | string | exactly `stream-` plus 32 lowercase hex digits; matches Provider current Stream |
| `stream_contract_version` | decimal | exact supported wire/application contract |
| `stream_session_epoch` | decimal | non-zero, current |
| `data_prefix` | NDN URI string | Provider-owned registered payload root whose final typed Version equals `mapping_version` |
| `mapping_root` | NDN URI string | exact unversioned `/<provider>/NDNSF/STREAM-MAP/<stream-id>` base root |
| `mapping_version` | typed Version value | non-zero and immutable; appended to Mapping block names and shared by `data_prefix` |
| `mapping_block_capacity` | decimal | positive fixed cursor span per block |
| `max_name_reservations` | decimal | shared positive Provider/Consumer permanent name-reservation bound; v1 default 65,536 |
| `mapping_anchor_block` | decimal | block containing latest join cursor |
| `mapping_anchor_content_digest` | lowercase hex | SHA-256 of anchored block's canonical Content |
| `sample_unit` | string | application-defined production/FEC group for current UAV implementation |
| `sample_period_ms` | decimal | positive period measured for that exact sample unit |
| `latest_join_cursor` | decimal | application-proven decoder-reset sequence start |
| `latest_produced_cursor` | decimal | greatest produced payload cursor |
| `mapping_committed_through_cursor` | decimal | greatest gap-free sealed Mapping cursor; covers join |
| `oldest_retained_cursor` | decimal | oldest payload whose complete Mapping chain through the checkpoint is also still serviceable |
| `next_reserved_cursor` | decimal | first cursor not yet reserved |
| `prefetch_eligibility` | enum | `ahead-mapped` or `retrieval-only` |
| `key_epoch` | decimal | non-zero, current |
| `stream_key_hex` | lowercase hex | exactly 64 characters / 32 bytes |
| `nonce_salt_hex` | lowercase hex | exactly 8 characters / 4 bytes |
| `stream_cipher` | enum string | exactly `aes-256-gcm` |

The descriptor exists only in protected Response payload and process memory.
The Provider nonce/name guard and the Core Mapping resolver are both configured
from `max_name_reservations`; a deployment cannot silently use different
producer and consumer limits.
The Provider publishes no successful descriptor until bounded warm-up has
measured at least the configured minimum completed publication groups, selected
a decoder-safe join sequence, and committed Mapping through that join. Timeout
returns a failed response with no key. Cursor zero is never an empty sentinel.
Evicting a Mapping block required by the advertised retained interval atomically
evicts/withdraws the covered payload interval and advances
`oldest_retained_cursor`; an NFD cache copy is not availability evidence.

## StreamNameMapBlock

Provider-signed immutable control Data. For fixed capacity `B`, cursor `C`
resolves to block `floor(C/B)` and slot `C mod B`. Its exact Data name is:

```text
/<provider>/NDNSF/STREAM-MAP/<stream-id>/v=<mapping-version>/seq=<block-number>
```

One block is one signed Data packet with `ContentType=Manifest`, no
`FinalBlockId`, exact Interest retrieval, and a total wire size at or below the
configured cap and `ndn::MAX_NDN_PACKET_SIZE`. Its canonical content contains:

| Field | Rule |
|---|---|
| `streamId/sessionEpoch/mappingVersion` | exact active descriptor context |
| `blockNumber` | typed SequenceNum and canonical monotonic block number |
| `firstCursor` / `capacity` | exactly `B*blockNumber` / fixed `B` |
| `entries[]` | one exact original NDN Name or permanent tombstone per cursor |
| `previousContentDigest` | SHA-256 of prior canonical Content, required except genesis |
| `schemaVersion` | canonical map-content schema version |

A cursor transitions exactly once from unassigned to a committed name or a
tombstone known before the block is signed. An ahead-predicted name that later
produces no Data remains bound and becomes local `terminal-unproduced` state;
it is never rewritten or reused. No block is replaced under the same Data name.
Mapping contains no video payload and no nested NDN Data packet. Names must be
globally immutable, prefetch-reservable when advertised ahead, and fall below
the descriptor-pinned prefix that the expected Provider is authorized to sign.

For UAV contract v1, each source cursor corresponds to
`VideoPacket.packetSeq`; one sample is a completed publication/FEC group
identified by `frameSeq`. Encrypted source and opaque repair items use separate
semantic name branches and zero-based typed Segment indices as specified in
`contracts/camera-stream-security.md`.

Ahead/late evidence is not a cross-host wire timestamp. Provider logs local
map-publication-before-payload order; consumer logs local
map-admission-before-scheduling-deadline order; experiments correlate them by
StreamId/cursor.

## EncryptedVideoPacket

The outer NDN Data name is the exact original application name in the accepted
Mapping entry. Content is one strict `HybridMessageEnvelope`:

| Envelope field | Rule |
|---|---|
| algorithm | `AES-256-GCM` |
| keyId | current StreamId; not secret |
| epochId | decimal key epoch |
| messageType | `uav-live-video-packet` |
| nonce | salt[4] plus cursor[8], network order |
| cipherText | encryption of the complete encoded VideoPacket |
| authTag | 16-byte GCM tag |

The signed outer Data Name, validated Mapping digest/cursor, and descriptor
supply the rest of the context. The current generic envelope decoder is
permissive: it ignores unknown
fields and does not reject duplicates. Before using it, the UAV application
strictly inspects the outer envelope block and requires exactly one occurrence
of every listed field, no unknown or wrapped-key field, no trailing bytes, and
the exact nonce/tag/type sizes. This application guard leaves the generic
decoder and generic wire format unchanged.

Before entering Spec 119 this complete envelope is one opaque byte string.
Optional FEC is calculated over these bytes. After ordinary or recovered
delivery the same strict envelope/AAD/AEAD/replay validation runs; Core never
receives key/salt/plaintext and never constructs `VideoPacket`.

## VideoConsumerKeyState

Volatile Ground Station state installed atomically from one matching successful
start response.

| Field | Rule |
|---|---|
| expected Provider/service/request | captured from pending start call |
| Stream descriptor | fully validated before activation |
| Mapping cache | bounded verified cursor/name/tombstone bindings |
| key/salt | decoded fixed-size bytes; never logged |
| accepted sequences | bounded duplicate/replay tracking |
| counters | validation, AEAD, stale-session, replay, and accepted counts only |

States:

```text
NO_KEY -> VALIDATING_RESPONSE -> READY -> CLEARING -> NO_KEY
                              \-> REJECTED
READY -> VALIDATING_DATA -> READY
READY -> REPLACED/STOPPED -> NO_KEY
```

Data failure never transitions to plaintext mode.

## Identity And Lifetime Relationships

```text
one Drone identity
  -> one active camera StreamId
       -> one sessionEpoch
            -> one mappingVersion, unversioned mappingRoot, and versioned dataPrefix
                 -> many immutable cursor/name-or-tombstone bindings
            -> one active keyEpoch/key/salt
                 -> many unique non-tombstone cursor nonces

one successful start response
  -> installs one matching VideoConsumerKeyState
```

A copied descriptor without the key is insufficient. A delivered key cannot be
revoked retroactively; stop/start or rekey establishes future confidentiality.
