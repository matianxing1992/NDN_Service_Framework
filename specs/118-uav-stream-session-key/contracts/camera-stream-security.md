# Contract: UAV Camera Stream Security

## 1. Camera Start Response

This contract extends only the existing UAV camera-video application payload.
It does not modify `ResponseMessage`.

For a successful `action=start`, all current non-secret response fields remain,
and these fields are required:

```text
stream_cipher=aes-256-gcm
key_epoch=<decimal non-zero>
stream_key_hex=<64 lowercase hexadecimal characters>
nonce_salt_hex=<8 lowercase hexadecimal characters>
```

The existing fields below are part of the same accepted descriptor:

```text
stream_id
stream_session_epoch
stream_contract_version
data_prefix
mapping_root
mapping_version
mapping_block_capacity
max_name_reservations
mapping_anchor_block
mapping_anchor_content_digest
sample_unit
sample_period_ms
latest_join_cursor
latest_produced_cursor
mapping_committed_through_cursor
oldest_retained_cursor
next_reserved_cursor
prefetch_eligibility
```

The key-bearing payload is admissible only after the normal NDNSF successful
Response path validates the pending request, expected Provider, exact camera
service, request token, and `/PERMISSION/<camera-video-service>` encryption.
`mapping_root` is the unversioned
`/<provider>/NDNSF/STREAM-MAP/<stream-id>` base root. Mapping block names append
the separate typed `mapping_version`; the final typed Version of `data_prefix`
MUST equal that same value.
`max_name_reservations` is a positive shared permanent-name bound no larger
than the Core maximum; contract v1 defaults it to 65,536. The Provider's
explicit-name reservation guard and Consumer resolver use this same protected
value and close/rekey rather than diverge at capacity.

The application payload remains the existing `encodeFields` text format, but
the security decoder consumes the raw payload before constructing `Fields`. It
requires canonical lexicographic key order, one occurrence of every key,
lowercase `[a-z0-9_]+` keys, uppercase two-digit percent escapes only for `%`,
`;`, or `=`, and byte-identical `encodeFields(decoded)==raw`. Required decimal
fields use the shortest unsigned decimal spelling (`0`, or a non-zero digit
followed by digits) with overflow rejected. `stream_id` is exactly
`stream-<32 lowercase hexadecimal digits>`; key, salt, and anchor digest are
respectively 64, 8, and 64 lowercase hexadecimal digits. Canonical unknown
non-secret extension fields remain allowed. These rules prevent duplicate or
malformed authority fields from being silently overwritten by the generic
`decodeFields` helper.

Success is emitted only after a configurable readiness timeout has not expired,
at least three completed publication groups (or a higher configured minimum)
provide a measured period, an application-owned H264 boundary gate identifies
a complete decoder-reset sequence (required parameter sets plus IDR), and the
Mapping anchor/frontier covers its first cursor. Otherwise start fails, clears
the tentative session/key, and exposes no secret. Cursor zero remains valid.

The following payloads MUST NOT contain `stream_key_hex` or `nonce_salt_hex`:

- failed/rejected `start`;
- `stop`, including already-stopped;
- camera status and telemetry;
- ACK payloads;
- any other service.

Equivalent repeated start preserves the active StreamId, roots, versions,
key, and salt but returns a newly encoded/protected snapshot of current
frontiers and matching anchor, bound to the new request/token. A configuration
change follows stop/restart and receives a new StreamId/key context.

## 2. Signed Stream Name Mapping

For fixed capacity `B`, Mapping Data has predictable typed control names:

```text
blockNo(C) = floor(C / B)
slot(C)    = C mod B
M(C)       = /<provider>/NDNSF/STREAM-MAP/<stream-id>/
             v=<mapping-version>/seq=<blockNo(C)>
```

`v=` is a VersionNameComponent and `seq=` is a SequenceNumNameComponent.
`Data(M)` is one Provider-signed packet with `ContentType=Manifest`, no
`FinalBlockId`, exactly `B` consecutive entries, and total signed wire size at
or below both the configured cap and `ndn::MAX_NDN_PACKET_SIZE`. It is retrieved
with `CanBePrefix=false`, no ApplicationParameters. Each entry is exactly one
original application NDN Data name or a tombstone fixed before signing. The
consumer validates `M`, canonical Content, required previous-content SHA-256
continuity (except genesis), the expected Provider signature chain, active
Stream/session/Mapping version, fixed range, and authority over every original
name before atomically installing the whole block. A second validly signed wire
packet under the same explicit name is fatal equivocation, not a replacement.
Block capacity is frozen for the session after worst-case Name/signature sizing;
an oversized actual encoding fails publication without frontier advancement.
The descriptor anchor and each `previousContentDigest` use SHA-256 over only the
canonical `StreamNameMapBlock` Content TLV bytes; outer Data name, MetaInfo, and
signature fields are excluded, and no digest field hashes itself.
The Provider MUST retain and serve every Mapping block needed to verify all
advertised retained payloads through the descriptor checkpoint. A required
Mapping-block eviction atomically withdraws the covered payload availability and
advances `oldest_retained_cursor`; cached copies in NFD cannot justify a stale
retention claim. Equivalent repeated starts expose the updated frontier.

Mapping MUST be committed before the Provider advances its advertised frontier.
The Provider records map-before-production order on its local monotonic clock;
the consumer independently records map-admitted-before-scheduling-deadline on
its local clock. The clocks are never subtracted. A late Mapping remains valid
for ordinary retrieval but increments `mapping-late` and earns no future-
prefetch credit. A published cursor/name or Mapping block is never rebound.
If an ahead-predicted name produces no Data, its Interest is cancelled or
expires as `terminal-unproduced`; the binding stays. Tombstone is not an
after-the-fact correction for overprediction.

For a non-tombstone entry:

```text
cursor C -> originalDataName N
Data(N).Content = encrypted VideoPacket assigned to C
```

`N` MUST be below descriptor-pinned `data_prefix`, contain a fresh stream/
version context, and never identify different wire Data across restart/rekey.
For contract v1, each source `cursor == VideoPacket.packetSeq`,
`sample_unit=fec-group`, and the complete names are:

```text
data_prefix = /<provider>/video/<camera-id>/<stream-id>/v=<mapping-version>

source item = <data_prefix>/fec-group/seq=<frameSeq>/data/
              seg=<sourceIndex>

repair item = <data_prefix>/fec-group/seq=<frameSeq>/repair/
              seg=<repairIndex>
```

`camera-id`, `fec-group`, `data`, and `repair` are GenericNameComponents;
`v=` is Version, `seq=` is SequenceNum, and `seg=` is Segment. A source item
requires `sourceIndex < sourceCount`; its `FinalBlockId` is
`Segment(sourceCount-1)`. When `XorOneRepair` is enabled, contract v1 has one
repair item with `repairIndex=0` and `FinalBlockId=Segment(0)`. The repair has
its own Mapping cursor/name but no `VideoPacket.packetSeq` or UAV AEAD nonce;
its signed Content is the Spec 119 FEC metadata plus opaque coded bytes. The
measured sample period is the Provider-local interval between completed group
publications. Both prefixes are registered before
descriptor publication and removed on teardown. The former
`N = <stream_prefix>/<packetSeq>` derivation is not an accepted fallback.

## 3. Nonce

```text
nonce = nonceSalt[0..3] || uint64_be(cursor)
```

The Provider MUST encrypt each non-tombstone cursor at most once under one key. It
must not encrypt again after a possible nonce use, even if later publication or
signing fails. Duplicate, remap, rollback, overflow, or uncertain state closes
the key epoch. Tombstones permanently consume their cursor without consuming an
AEAD nonce.

The envelope carries the nonce. The consumer independently derives the expected
nonce and rejects a mismatch before decryption.

## 4. Associated Data

AAD is a canonical versioned TLV block, never delimiter-joined text:

```text
UavVideoAad
  version = 1
  exactDataName = wireEncode(N)
  providerIdentity
  serviceName
  streamId
  sessionEpoch
  mappingVersion
  keyEpoch
  cursor
```

Producer and consumer use byte-identical golden vectors. Any field mismatch
causes authentication failure.

Contract-v1 wire assignments are application-private and immutable:

| Field | TLV-TYPE | Encoding |
|---|---:|---|
| `UavVideoAad` | `0xF700` | outer container |
| `version` | `0xF701` | canonical NNI, exactly `1` |
| `exactDataName` | `0xF702` | container with exactly one canonical standard `Name` TLV |
| `providerIdentity` | `0xF703` | container with exactly one canonical standard `Name` TLV |
| `serviceName` | `0xF704` | container with exactly one canonical standard `Name` TLV |
| `streamId` | `0xF705` | UTF-8 bytes matching the descriptor StreamId |
| `sessionEpoch` | `0xF706` | canonical NNI, non-zero |
| `mappingVersion` | `0xF707` | canonical NNI, non-zero |
| `keyEpoch` | `0xF708` | canonical NNI, non-zero |
| `cursor` | `0xF709` | canonical NNI; zero is valid |

Children occur exactly in table order. No child is optional or repeated;
unknown, reordered, non-minimal, trailing, or non-canonical wire is rejected.
The exact/provider/service name containers distinguish the three nested Name
TLVs without converting them to delimiter-joined strings.

## 5. Data Content And Signature

```text
cursor C -> originalDataName N  // from accepted Mapping
plaintext = encodeVideoPacket(packet assigned to C)
content   = wireEncode(HybridMessageEnvelope{
  algorithm   = "AES-256-GCM",
  keyId       = streamId,
  epochId     = decimal(keyEpoch),
  messageType = "uav-live-video-packet",
  nonce,
  cipherText,
  authTag
})

Data(N).Content = content
Data(N).Signature = Provider signature
```

The complete `content` bytes, not `plaintext`, are the only value passed into
Spec 119. When optional FEC is enabled, Spec 119 computes repair over these
opaque envelope bytes. It receives no key, nonce salt, or `VideoPacket` object.

The consumer order is mandatory:

```text
exact expected version-unique name
  -> accepted immutable cursor/name Mapping lookup
  -> trust-schema rule binding source/repair/control names to expected Drone
  -> optional Spec 119 digest-verified opaque-byte recovery
  -> UAV-owned strict envelope/context/nonce validation
  -> AES-GCM authentication/decryption
  -> decodeVideoPacket
  -> current session/cursor checks
  -> reorder/decoder
```

There is no plaintext format detection or fallback.
`FecRecovered` content follows the same strict envelope and AEAD path; Core
recovery is never treated as permission to skip application authentication.

The strict envelope step requires exactly one occurrence of every permitted
field and rejects unknown, duplicate, wrapped-key, wrong-size, or trailing
content before decryption. It does not rely on the permissive behavior of the
generic `HybridMessageEnvelope::WireDecode()` implementation.

## 6. Failure Contract

| Failure | Required result |
|---|---|
| No camera-service permission | No usable start descriptor/key |
| Missing/malformed key field | Start rejected locally; no packet Interests |
| Missing/late/unverified Mapping | No payload Interest; bounded mapping-starved/late result |
| Conflicting cursor/name or replaced block | Reject Mapping; close affected session authority |
| Original name outside signer authority | Reject Mapping before payload Interest |
| Old cached Data from another version/session | Reject by exact descriptor-pinned name/context |
| Mapping signed by another valid Provider | Reject identity/name relation before installation |
| Duplicate Interest/retransmission | Reuse byte-identical materialized signed Content |
| Evicted cursor requested again | Never re-encrypt under the old cursor nonce |
| Wrong Provider/service/Stream/session/epoch | Clear/reject key state; no decoder input |
| Wrong Data name or signer | Reject before decryption |
| Wrong nonce/envelope field | Reject before decryption |
| Bad tag/ciphertext/AAD | AEAD failure; no VideoPacket/decoder input |
| Invalid or unrecoverable FEC group | Core rejects/falls through within deadline; no UAV decrypt/decoder input |
| Duplicate or replay | Bounded duplicate counter; no repeated decoder input |
| New session with old key | AEAD/context failure; no fallback |
| RNG failure | Start fails before active publication/descriptor |
| Plaintext diagnostic mode | Key-bearing start must fail closed unless hybrid encryption is guaranteed |

Diagnostics contain only Provider, StreamId, session/Mapping/key version, cursor,
Mapping block number, reason code, and counters. They never contain
key, salt, nonce bytes, ciphertext plaintext, decoded H264, complete protected
response payload, or a full sensitive application name.
