# Implementation Plan: UAV Stream Session-Key Delivery

**Branch**: current working branch; no branch switch required | **Date**: 2026-07-17 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from
`specs/118-uav-stream-session-key/spec.md`

## Summary

Keep the existing UAV camera-video start request but replace its transport-only
payload name with the application's original semantic NDN Data name. Spec 119's
generic `StreamNameMap` assigns each internal monotonic cursor to one original
name before the advertised production horizon. On successful `action=start`,
return a protected descriptor containing Mapping authority plus a 256-bit
session key, key epoch, and nonce salt. Sign Mapping Data, encrypt the existing
encoded `VideoPacket` with AES-256-GCM under its mapped original name, retain the
Provider signature, and validate Mapping/signature/name before decryption. Do
not change generic NDNSF messages or `ServiceProvider`/`ServiceUser` request
APIs.

## Technical Context

**Language/Version**: C++17 in the existing waf project

**Primary Dependencies**: Spec 119 `StreamNameMap` cursor/codec foundation,
ndn-cxx 0.9.0, OpenSSL AES-GCM/RAND, existing
`HybridMessageEnvelope`, existing NDNSF hybrid Response encryption, NAC-ABE,
existing UAV `VideoPacket` and video control service

**Storage**: Session key state is memory-only; no new persistent storage

**Testing**: Boost.Test unit/contract tests, Python contract/evidence parsers,
existing NDNSF regression scripts, MiniNDN UAV live-video campaign

**Target Platform**: Linux/MiniNDN; no real-UAV or container gate in this spec

**Project Type**: C++ framework plus UAV application

**Performance Goals**: Preserve the current bounded producer/consumer windows
and complete the existing 60-second video acceptance workload at matched 0%
and 5% configured loss; no throughput-improvement claim

**Constraints**: No generic Response change or second RPC protocol; no plaintext
fallback; no key persistence/logging; Mapping and original-name Data remain
Provider-signed; explicit Data names are globally immutable/version-unique; one
cursor has one immutable name or predeclared-tombstone binding; Face callbacks
must not add unbounded work

**Scale/Scope**: One live camera Stream per Drone session, current 600-packet
producer retention, existing ground-station window/backlog bounds

## Constitution Check

- **Canonical Dynamic Runtime**: PASS. The existing unified camera-video
  service and generic dynamic Request/Response API remain unchanged.
- **Security Is Part Of The Data Path**: PASS. The start Response continues to
  use `/PERMISSION/<camera-video-service>`; Data is encrypted and Provider-
  signed; negative permission/tamper/replay gates are mandatory.
- **CodeGraph First**: PASS. Current Response hybrid crypto, UAV start handler,
  packet publisher, ground fetcher, and Stream helpers were source-verified.
- **Spec-Driven Durable Change**: PASS. This directory owns requirements,
  contracts, tasks, and validation.
- **Right-Scope Verification**: PASS. Focused tests precede one 60-second
  MiniNDN campaign per required loss condition.
- **Cohesive Tasks**: PASS. Tests, implementation, and focused evidence for one
  behavior remain in the same task.

Post-design re-check: PASS. No constitution exception is required.

## Current-Code Baseline

```text
Ground Station
  postRequestForDrone(... /UAV/Camera/Video, action=start)
    -> encrypted NDNSF Response Fields
    -> stream_id, stream_session_epoch, stream_prefix, next_packet, hints

Drone VideoPublisher
  encode VideoPacket
  remember <= 600 packets
  respond to /<drone>/video/<stream-id>/<packetSeq>
  sign each NDN Data

Ground Station
  predict packetSeq, express exact Interests
  decode VideoPacket, perform UAV-owned XOR/FEC/reorder/decoder work
```

Current gaps are concrete but now span naming and security: live `VideoPacket`
Content is plaintext, transport sequence replaces the application's semantic
Data name, there is no cursor/name Mapping or Stream key context, and the direct
video fetch callback does not perform explicit Mapping and trust-schema
validation before decoding.

## Accepted Design

### 1. Application-Owned Start Descriptor

Only a successful camera-video `action=start` response adds:

```text
stream_cipher=aes-256-gcm
key_epoch=<non-zero decimal>
stream_key_hex=<64 lowercase hex characters>
nonce_salt_hex=<8 lowercase hex characters>
```

These fields join `stream_contract_version`, a fresh `stream_id`,
`stream_session_epoch`, a Provider-owned versioned `data_prefix`, the exact
unversioned `mapping_root`, separate `mapping_version`, fixed
`mapping_block_capacity`,
`max_name_reservations`,
`mapping_anchor_block`, `mapping_anchor_content_digest`, `sample_unit`,
`sample_period_ms`, `latest_join_cursor`, `latest_produced_cursor`,
`mapping_committed_through_cursor`, `oldest_retained_cursor`,
`next_reserved_cursor`, and `prefetch_eligibility` inside the existing
`encodeFields` application payload. They are not `ResponseMessage` fields.
Contract v1 uses one namespace version: the final Version component of
`data_prefix` equals `mapping_version`; Mapping block names append that Version
and a SequenceNum to the unversioned Mapping base root.
The protected `max_name_reservations` is one shared permanent-name bound for
both the Provider guard and Core resolver (65,536 by default in v1). Reaching
it closes the current publication authority instead of allowing one side to
continue after the other has stopped admitting Mapping.
`stream_prefix` and `next_packet` cease to be authoritative. A descriptor
contract version makes the cutover atomic: a legacy consumer rejects the new
session rather than constructing an old name, while pressure-only remains only
an algorithm rollback over the new wire contract.

Start enters a bounded application readiness warm-up before success: at least
three completed publication/FEC groups measure the period for that exact unit,
the UAV H264 adapter identifies a complete decoder-reset sequence beginning at
an access-unit boundary with required parameter sets and IDR, and Mapping is
committed through its first cursor. The warm-up limit is shorter than the
service response timeout. Failure clears the tentative key/session and returns
no secret.

Equivalent repeated starts do not call the session-reset path. They preserve
Stream/key/root/version identity but encode a fresh protected frontier/anchor
snapshot for the new request/token. Configuration changes continue through the
current stop/restart workflow.

### 2. Session Key And Nonce Contract

- Generate 32 key bytes and 4 salt bytes with the existing secure OpenSSL RNG
  before publishing any packet or successful descriptor.
- The first key epoch is 1. Rekey under one StreamId increments both the key
  epoch and Stream session epoch; it also advances the shared payload/Mapping
  namespace version before any newly encrypted Data.
  Stop/start normally allocates a new StreamId and independent epoch 1.
- Stream cursor is a unique unsigned 64-bit value under one session key. A
  tombstone consumes its cursor permanently but does not encrypt payload Data.
- Nonce is `nonceSalt[4] || cursor[8]`, with cursor in network byte order. Any
  duplicate, remap, rollback, overflow, or reuse attempt closes that epoch.

### 3. Signed Ahead-Of-Production Name Mapping

Spec 119 supplies the generic Mapping record/codec and cursor resolver. Before
returning the descriptor, the UAV Provider registers one bounded payload prefix
and Mapping prefix under the Drone namespace. A normative payload name is:

```text
/<drone>/video/<camera>/<stream-id>/v=<mapping-version>/
  <sample-kind>/seq=<sample-number>/<item-kind>/seg=<segment-number>
```

The application may refine semantic components but MUST remain below
`data_prefix`, use typed Version, SequenceNum, and Segment components, and never
reuse the complete name for different wire Data. A segmented sample sets
`FinalBlockId` to its last Segment; an application record intentionally modeled
as an independent item documents that it has no FinalBlockId.

The v1 UAV adapter is fully concrete: each source cursor is bound to the
corresponding `VideoPacket.packetSeq`, and one completed publication/FEC group
is one sample. Source Data uses
`<data_prefix>/fec-group/SequenceNum(frameSeq)/data/Segment(sourceIndex)`;
optional repair Data uses the same prefix with `repair/Segment(repairIndex)`.
Source Content is an encrypted VideoPacket envelope; repair Content is the Spec
119 signed FEC metadata plus opaque coded bytes and is not a VideoPacket. Each
branch sets `FinalBlockId` to its own final Segment and validates declared group
counts. This is an application naming adapter, not a Core naming template.

The Provider publishes immutable Mapping blocks under:

```text
/<drone>/NDNSF/STREAM-MAP/<stream-id>/v=<mapping-version>/seq=<block-number>
```

For fixed capacity `B`, `block-number = floor(cursor / B)` and slot
`cursor mod B`. Each block covers exactly `[B*block-number,
B*(block-number+1)-1]`, is one Provider-signed Data packet with
`ContentType=Manifest`, no `FinalBlockId`, and total signed wire size no larger
than the configured cap or `ndn::MAX_NDN_PACKET_SIZE`, whichever is smaller.
Only a sealed gap-free block advances `mapping_committed_through_cursor`.
Mapping Interests are exact and block SequenceNum is not a segmented-object
Segment component.

Each entry is committed once to an exact original name or a tombstone known
before signing. An ahead-predicted name that later produces no Data remains an
immutable name binding; authenticated actual sample extent cancels its Interest
and marks it `terminal-unproduced`. It is never rewritten as a tombstone or
reused. Underprediction allocates later cursors/blocks and is ordinary late
retrieval, not prefetch success. Mapping carries only names and bounded control
metadata, never nested Data or video bytes.

Mapping retention and payload retention are one advertised availability
contract. The Provider continues serving every block needed to validate the
interval from `oldest_retained_cursor` through the descriptor checkpoint. If a
required Mapping block is evicted, the covered payload availability is withdrawn
and `oldest_retained_cursor` advances atomically; an NFD cache copy is not enough
to advertise beginning-mode availability.

The application must know each complete name and commit its Mapping before the
advertised horizon. Production-dependent names are `retrieval-only`. Provider
and consumer monotonic timestamps are never compared: the Provider records its
local map-published-before-payload order, the consumer records its local
map-admitted-before-schedule-deadline order, and experiments correlate events
by session/cursor. Ahead Mapping exposes future names, batch volume, timing, and
unused predictions earlier than ordinary retrieval. Therefore names may not
contain secrets; this is an explicit accepted privacy cost, not an assertion of
zero extra disclosure.

### 4. Encrypted Video Data

Publish each payload under the exact original name in its accepted Mapping
entry and retain the outer Provider signature. Encode the existing `VideoPacket`,
then encrypt all bytes into the existing
`HybridMessageEnvelope` with:

```text
algorithm   = AES-256-GCM
keyId       = streamId
epochId     = decimal keyEpoch
messageType = uav-live-video-packet
nonce       = derived 12-byte nonce
cipherText  = encrypted encoded VideoPacket
authTag     = 16-byte GCM tag
```

Canonical AAD is a versioned TLV block containing the exact original Data Name
wire encoding, expected Provider identity, camera-video service, StreamId,
session epoch, Mapping version, key epoch, and cursor.
Length-delimited TLV avoids ambiguous string concatenation.
The exact private TLV assignments, nested Name containers, fixed child order,
and canonical-NNI rules are frozen in `contracts/camera-stream-security.md`.

The descriptor security parser consumes the raw canonical `encodeFields`
payload, detects duplicate/malformed keys before map construction, enforces
shortest decimal and fixed lowercase-hex forms, and only then builds the typed
descriptor. It does not reuse the existing permissive `decodeFields` path for
authority or secret fields.

The existing generic `HybridMessageEnvelope::WireDecode()` is permissive: it
ignores unknown TLVs and overwrites duplicate fields. The UAV consumer therefore
performs an application-owned strict structural pass before generic decode,
requiring exactly one instance of each allowed field, rejecting wrapped-key and
unknown fields, and checking exact nonce/tag/type sizes. This does not change
the generic envelope or `ResponseMessage` wire contract.

The encryption/FEC boundary is explicit:

```text
encode VideoPacket -> UAV AES-GCM -> opaque envelope bytes
  -> Spec 119 exact-name publication + optional XOR over opaque bytes
  -> Spec 119 Provider/digest admission -> opaque envelope bytes
  -> UAV AES-GCM/replay admission -> VideoPacket -> reorder/decoder
```

UAV selects `None` or `XorOneRepair` and supplies complete semantic source and
repair names, but Spec 119 owns parity generation, recovery, bounds, and generic
status. Core never receives the Stream key, nonce salt, or plaintext. A locally
recovered opaque item must pass the same UAV AAD, AEAD, session, and replay gate
as an ordinary signed source item. The old UAV XOR producer and `FecFrameState`
recovery authority are removed after migration.

### 5. Ground-Station Admission

The ground station accepts key material only in the response callback of its
pending successful start, after existing NDNSF Response validation/token checks.
It validates exact field presence, hex canonical form, sizes, cipher, Provider,
service, StreamId/session, Mapping authority/frontier, and non-zero epochs before
atomically replacing the in-memory consumer key state.

The ground station passes the validated descriptor and expected Drone identity
to Spec 119 `openLiveStream`. That handle validates Mapping name/signature/
continuity/authority, resolves cursors, issues exact semantic-name Interests,
and validates payload/repair name/signature and optional FEC metadata/digest
before its callback. The UAV callback then
checks the strict envelope, AAD, nonce, AEAD, replay, and VideoPacket session;
only success returns application admission and enters reorder/decoder.
Failure increments non-secret counters and returns reject, which produces no
prefetch observation or decoder callback. Stop, replacement, and shutdown stop
the handle and clear UAV key/decoder plus handle-owned FEC state.

The general `examples/trust-schema.conf` rule is not sufficient for this path
because it does not capture and equate the dynamic Drone identity/root. T004
creates a UAV Stream-specific validator configuration (or equivalent explicit
identity-equality check), wires it through `uav_runtime.conf` and deployment
preflight, and negative-tests a second certificate that is valid under the same
Trust Anchor but belongs to another Provider.

### 6. Security Semantics

The existing NDNSF Response hybrid encryption wraps its MessageKey under
`/PERMISSION/<camera-video-service>`. Consequently this is service-level shared
access, not requester-exclusive encryption. Once delivered, the Stream key is
a bearer capability until rotation; revocation cannot erase it. A new session
or explicit rekey is required to exclude a former consumer from future Data.

The feature emits key epoch, accepted/rejected packet counts, failure reason
codes, and rotation events, but never key/salt/nonce bytes or plaintext packet
content. Plaintext diagnostic response modes are inadmissible for the key-
bearing response; the start fails closed if the runtime cannot guarantee hybrid
encryption.

## Project Structure

```text
specs/118-uav-stream-session-key/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── contracts/
│   └── camera-stream-security.md
├── quickstart.md
└── tasks.md

ndn-service-framework/
├── HybridMessageCrypto.hpp/.cpp       # explicit-nonce AEAD helper only
└── NDNSFMessages.hpp/.cpp             # unchanged; non-regression gate

NDNSF-UAV-APP/
├── shared/UavProtocol.hpp/.cpp        # descriptor/envelope/AAD helpers
├── configs/uav-stream-trust-schema.conf # Drone identity/name capture rules
├── drone/DroneServiceContainer.inc.hpp
└── ground-station/GroundStationServiceContainer.inc.hpp

tests/
├── unit-tests/uav-protocol-state.t.cpp
├── python/test_ndnsf_uav_stream_control_isolation_campaign.py
└── run_uav_stream_security_contract.py

Experiments/
└── NDNSF_UAV_GUI_Minindn.py
```

**Structure Decision**: Reuse the current UAV application owners and generic
AEAD envelope, but consume Spec 119's complete public
`LiveStreamPublisher`/`LiveStreamConsumerHandle` API. Spec 118 adds no Mapping
route, Face scheduler, producer pending table, parallel RPC path, or duplicate
cursor/controller authority. UAV adapters only construct semantic video names,
encrypt reserved payloads before the API, select the optional generic FEC mode,
admit/decrypt opaque items after the API, and feed reorder/decoder logic.

## Delivery Order

1. Complete Spec 119 T001-T004: Mapping, controller, public C++/Python
   LiveStream API, and app-neutral MiniNDN gate.
2. Freeze descriptor, Mapping, envelope, nonce, and AAD contracts with
   deterministic and malformed vectors.
3. Generate/store the key and create a `LiveStreamPublisher`; use its successful
   readiness/descriptor snapshot as the sole key-bearing camera-start result.
4. Reserve UAV semantic source/repair names ahead, encrypt source VideoPackets
   with reservation cursor/name binding, and pass only opaque envelopes to
   `publish`/`publishGroup`. Open one `LiveStreamConsumerHandle`; after ordinary
   or `FecRecovered` delivery, decrypt/admit in the UAV callback before
   reorder/decoder.
5. Close rotation, replay, logging, compatibility, and MiniNDN gates while
   proving no UAV-owned Mapping/fetch/pending/XOR-recovery implementation remains.

Each step is independently revertible before release, but a deployed wire
cutover is session-atomic. Capability/contract-version negotiation starts a new
session; there is no mixed mode. Rollback selects the prior candidate as a
whole and never accepts an old transport-only name, unsigned Mapping, or
plaintext payload inside a semantic-name encrypted session.

## Complexity Tracking

One generic Mapping layer is introduced by Spec 119 because arbitrary semantic
names cannot be predicted from a numeric cursor without either a Mapping or
payload encapsulation. It is justified only when Mapping is committed ahead of
production; late Mapping is observable and receives no prefetch-success credit.
