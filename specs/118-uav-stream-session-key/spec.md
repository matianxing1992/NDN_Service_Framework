# Feature Specification: UAV Stream Session-Key Delivery

**Feature Directory**: `specs/118-uav-stream-session-key`

**Created**: 2026-07-17

**Status**: Complete in the MiniNDN scope. The cryptographic contract,
app-neutral LiveStream/FEC integration, and protected UAV validation pass;
real-hardware validation remains explicitly deferred.

**Input**: Protect the UAV live-camera stream with direct symmetric encryption
while keeping each application's original, meaningful NDN Data name. A signed,
bounded `StreamNameMap` assigns an internal monotonic cursor to each original
name early enough for future-Interest prefetch. Only a successful camera-start
service response carries the current Stream and key context. Generic NDNSF
Request/Response messages remain unchanged; generic cursor/mapping mechanics
are owned by Spec 119.

## Scope

This feature makes one existing application flow confidential:

```text
authorized camera start request
  -> encrypted successful response containing the live Stream descriptor
  -> signed, symmetrically encrypted UAV video Data
```

It applies only to the UAV camera-video control service and the live video Data
that service starts. Camera stop, camera status, failed start, telemetry,
flight-control, Repo, inference, and other service responses do not carry live
Stream keys.

The existing UAV `VideoPacket` remains the application payload model. Its
outer NDN Data name becomes the original application name resolved from a
verified Mapping entry, rather than a transport-only `streamPrefix/packetSeq`
name. Mapping Data carries names and control metadata only, never another NDN
Data packet or the video payload. Generic cursor/mapping structures and
prefetch decisions and all generic Mapping/payload network mechanics belong to
Spec 119. UAV owns permission to receive the key, semantic video-name planning,
AES-GCM protection/admission, VideoPacket sample/decoder behavior, and UI. It
selects Spec 119's default-off optional FEC but does not give Core a key or
plaintext and does not maintain a second XOR transport. It MUST use
`LiveStreamPublisher` and `LiveStreamConsumerHandle`, not implement a parallel
Mapping fetcher, Face pipeline, window controller, FEC recovery loop, or pending
table.

## User Scenarios & Testing

### User Story 1 - Start And Consume An Encrypted Camera Stream (Priority: P1)

An authorized operator starts a UAV camera stream through the existing video
control service. The successful response supplies everything required to
locate and decrypt the current live session. The ground station then retrieves,
authenticates, decrypts, reorders, and decodes the existing video packets.

**Why this priority**: This is the smallest end-to-end change that prevents
live-camera plaintext from crossing or residing in the NDN data plane.

**Independent Test**: Start one camera stream, inspect the service response and
captured video Data, and prove that an authorized ground station decodes video
while neither the response wire nor video Data Content exposes the Stream key
or encoded `VideoPacket` plaintext.

**Acceptance Scenarios**:

1. **Given** an authorized camera-start request, **When** the Provider starts a
   new live session, **Then** the successful encrypted response identifies one
   versioned payload namespace and one Mapping base root plus its typed version,
   carries the key context, Mapping capacity, sample unit/period, and distinct
   join, produced, committed, retained, and reserved cursor frontiers.
2. **Given** a valid successful response, **When** the ground station retrieves
   a signed Mapping and the mapped signed video Data, **Then** it validates the
   Provider and immutable cursor/name binding, requests the exact original
   name, lets Spec 119 optionally recover missing opaque ciphertext, decrypts
   the delivered bytes, and feeds unchanged `VideoPacket` semantics into the
   existing reorder/decoder path.
3. **Given** video is already running, **When** an authorized equivalent start
   request is repeated, **Then** it preserves the current key/session but returns
   a fresh protected frontier/anchor snapshot without resetting the active
   session; a materially different configuration
   follows the existing explicit restart path.

---

### User Story 2 - Reject Unauthorized Or Tampered Stream Data (Priority: P1)

An unauthorized observer may learn a Stream name or retrieve cached packets but
cannot obtain the session key through NDNSF or deliver modified packets to the
decoder. A permitted consumer also cannot mistake Data under another name,
session, epoch, or Provider for the requested stream.

**Why this priority**: Confidentiality without authenticated naming and
fail-closed consumption would permit cache poisoning, substitution, or partial
plaintext delivery.

**Independent Test**: Exercise missing permission, copied Stream identifiers,
wrong signer, wrong name, wrong session, stale key, corrupted ciphertext,
corrupted tag, and replayed packets; every case produces zero decoder input.

**Acceptance Scenarios**:

1. **Given** a principal without the camera-service permission, **When** it
   observes a start response or video Data, **Then** it obtains no usable Stream
   key or plaintext.
2. **Given** a permitted principal with a copied Stream descriptor but no key,
   **When** it retrieves video Data, **Then** decryption fails closed.
3. **Given** a correctly encrypted packet under a substituted name or signed by
   the wrong identity, **When** the ground station receives it, **Then** it is
   rejected before Core FEC admission, decryption, or decoder delivery.

---

### User Story 3 - Rotate Sessions Without Leaking Keys (Priority: P2)

An operator can stop and start the camera or restart the Provider without
reusing the prior live-session key context. Old keys cannot decrypt future
sessions, and secret material is absent from ordinary responses, telemetry,
logs, files, and crash evidence.

**Why this priority**: A session key is a capability. Rotation and secret
handling bound the damage of old responses or previously authorized users.

**Independent Test**: Start, stop, start again, and restart the Provider; verify
that session/key context changes, old keys fail on new Data, and secret scans
find no keys outside the intended encrypted start response and process memory.

**Acceptance Scenarios**:

1. **Given** a stopped or replaced session, **When** a new session starts,
   **Then** it receives fresh key material and a non-reused nonce context.
2. **Given** an old descriptor and key, **When** future-session Data arrives,
   **Then** the ground station rejects it and does not silently fall back to
   plaintext.
3. **Given** stop, status, failure, telemetry, or diagnostic output, **When** it
   is inspected, **Then** it contains no Stream key or nonce salt.

## Edge Cases

- Secure random generation fails while handling camera start.
- A successful start response is delayed or replayed after a newer session has
  become active.
- A sequence number is duplicated, rolls back, or reaches its nonce-space
  limit under the same key.
- One cursor is mapped twice, one original name is rebound to another cursor,
  a payload/Mapping name is reused after restart, or a Mapping block is
  replaced under the same explicit name.
- A Mapping arrives after its mapped Data is already produced, so it cannot
  support future prefetch.
- A Mapping points outside the Provider's authorized semantic namespace or to
  a name the Provider certificate is not allowed to sign.
- Predicted Mapping entries exceed the actual sample item count; the published
  name bindings remain immutable, the unused Interests are cancelled or expire
  as `terminal-unproduced`, and their cursors are never reused. A tombstone is
  legal only when committed before any name binding for that cursor.
- An application proposes a name that depends on content, capture time, or
  another fact unavailable before production; the item remains retrieval-only
  and cannot advance an ahead-prefetch frontier.
- A valid but different Provider certificate signs a Mapping or payload under
  the Drone namespace, or a stale cached packet from an old session satisfies
  an exact Interest.
- A forwarder repeatedly returns invalid same-name Data. Validation prevents
  its use, but signatures alone cannot guarantee availability against cache-
  poisoning denial of service when a future payload digest is not yet known.
- A packet has a valid outer signature but an invalid encryption tag, or a
  valid tag but an invalid Provider signature/name relationship.
- A packet is received before the ground station has accepted key material.
- Spec 119 FEC reconstructs one missing opaque encrypted item from authenticated
  repair metadata/Data; UAV then AEAD-validates and decrypts it. Two losses,
  wrong digest, wrong Provider, or expired repair reaches no decoder state.
- A repeated equivalent start arrives while streaming; a changed bitrate,
  frame width, FEC setting, or source requires the existing restart workflow.
- Plaintext crypto diagnostics are enabled in a development environment.
- Permission is revoked after a consumer has already obtained a session key.

## Requirements

### Functional Requirements

- **FR-001**: Only the successful `start` action of the UAV camera-video control
  service may return live Stream key material.
- **FR-002**: Generic NDNSF `ResponseMessage` fields, codecs, and Request/ACK/
  Selection/Response naming MUST remain unchanged. Generic `StreamNameMap` and
  cursor APIs are introduced only by Spec 119 and MUST NOT become another RPC
  protocol.
- **FR-003**: A successful start payload MUST contain
  `stream_contract_version`, collision-resistant `stream_id`,
  `stream_session_epoch`, Provider-owned `data_prefix`, exact unversioned
  `mapping_root`,
  `mapping_version`, fixed `mapping_block_capacity`, `mapping_anchor_block`,
  `max_name_reservations`,
  `mapping_anchor_content_digest`, `sample_unit`, positive
  `sample_period_ms`, `latest_join_cursor`, `latest_produced_cursor`,
  `mapping_committed_through_cursor`, `oldest_retained_cursor`,
  `next_reserved_cursor`, `prefetch_eligibility`, `key_epoch`,
  `stream_key_hex`, `nonce_salt_hex`, and the selected cipher identifier. In
  contract v1, `mapping_root` is exactly
  `/<provider>/NDNSF/STREAM-MAP/<stream-id>`; Mapping block names append the
  separate typed `mapping_version` and block SequenceNum. The final component
  of `data_prefix` MUST be the same typed `mapping_version`, so one immutable
  namespace version governs both payload and Mapping names.
  Contract v1 defaults `max_name_reservations` to 65,536 and uses the same
  protected value for the Provider's permanent explicit-name reservations and
  the Consumer Core resolver's permanent reverse-name reservations; neither
  side may continue the session after that shared limit is reached.
  A successful start MUST be delayed by a bounded readiness warm-up until all
  frontiers and the anchor exist, at least the configured minimum sample count
  has measured the declared period, and an application-proven decoder-safe join
  is Mapping-covered. Every advertised retained payload from
  `oldest_retained_cursor` through the join MUST also have a complete Mapping
  chain that the Provider can still serve. Readiness timeout fails the start and returns no key;
  cursor zero is never overloaded as an empty sentinel.
- **FR-004**: Stop, status, failed start, rejected request, telemetry, Repo,
  flight-control, and all unrelated service responses MUST omit
  `stream_key_hex` and `nonce_salt_hex`.
- **FR-005**: Each newly created Stream session MUST use a cryptographically
  random 256-bit key and a fresh nonce salt. Failure to generate either MUST
  fail the start operation before the session becomes visible or publishes
  Data.
- **FR-006**: The initial key epoch MUST be non-zero. Rekeying one StreamId MUST
  increment both `stream_session_epoch` and `key_epoch`, replace both key and
  salt, and allocate a fresh Mapping/payload namespace version. Every new camera
  session MUST allocate a CSPRNG StreamId with at least 128 random bits and a
  fresh namespace version; process restart, clock rollback, or key rotation
  MUST NOT reuse an explicit Mapping or payload Data name for different wire
  Data.
- **FR-007**: Every encrypted video packet MUST use a nonce that is unique under
  its session key. Its internal Stream cursor is the nonce sequence input.
  Duplicate cursor use, rollback, overflow, remapping, or possible nonce reuse
  MUST stop publication for that key epoch rather than retry insecurely.
- **FR-008**: Packet authentication MUST bind the exact NDN Data name, expected
  Provider identity, StreamId, session epoch, Mapping version, key epoch, and
  Stream cursor so that a valid ciphertext cannot be substituted into another
  name or context. Mapping integrity is established independently by its exact
  versioned name, Provider signature, canonical content, and continuity checks;
  no undefined or self-referential Mapping digest is placed in payload AAD.
- **FR-009**: The complete existing encoded `VideoPacket` MUST be encrypted and
  authenticated before it becomes NDN Data Content. No plaintext fallback or
  mixed plaintext/ciphertext session is permitted.
- **FR-010**: Every Mapping and video Data packet MUST retain an independently
  verifiable Provider signature. Separate trust rules MUST bind the exact
  descriptor-pinned Mapping base root/version and payload prefix to the expected Drone
  identity whose certificate chains to the configured Trust Anchor. A different
  otherwise-valid Provider certificate MUST be rejected before Mapping
  installation, decryption, FEC, or decoder delivery.
- **FR-011**: The key-bearing start response MUST remain protected by the
  existing NDNSF hybrid response path under
  `/PERMISSION/<camera-video-service>`. A diagnostic setting MUST NOT publish
  that payload in plaintext.
- **FR-012**: Service-level sharing is intentional: any principal that can
  decrypt the protected successful start response may use its Stream key. The
  feature MUST NOT claim requester-exclusive or retroactive secrecy.
- **FR-013**: Revocation protects future data only after rekey or a new Stream
  session. Documentation and events MUST state that a previously delivered key
  cannot be recalled.
- **FR-014**: Stream keys and nonce salts MUST remain only in the protected
  start payload and bounded process memory; they MUST NOT enter logs, status
  text, telemetry, metadata maps, persistent configuration, Repo objects, or
  error messages.
- **FR-015**: The ground station MUST accept key material and Mapping authority
  only from a successful response matching its pending start request, expected
  Provider, camera-video service, contract version, and active Stream session.
  It MUST validate the five cursor frontiers and begin latest-mode only at the
  application-supplied `latest_join_cursor`; stale/replayed descriptors,
  versions, impossible frontier ordering, and legacy consumers MUST fail closed.
- **FR-016**: Repeating an equivalent authorized start while the camera is
  already streaming MUST preserve the current StreamId, roots, versions, key,
  and salt but return a newly protected snapshot of the current frontiers and
  corresponding anchor bound to that request/token. It MUST NOT replay the
  initial stale descriptor. A materially changed configuration MUST use the
  existing stop/restart transition.
- **FR-017**: Mapping, FEC, encryption, and validation failures MUST be bounded,
  observable by non-secret reason counters, and produce no prefetch-estimator,
  decryption, or decoder callback.
- **FR-018**: The application MUST publish each payload under an original,
  meaningful and globally immutable NDN Data name below the descriptor-pinned,
  pre-registered `data_prefix`. In contract v1 the prefix's final typed Version
  MUST equal `mapping_version`. The name MUST include that fresh session/version
  component plus typed sample and, for a segmented sample, Segment components;
  different wire Data MUST never share one explicit name. Cursor-to-name
  Mapping MUST use the canonical single-Data block contract from Spec 119, be
  signed, immutable, bounded, and committed before the advertised frontier,
  and contain no nested NDN Data or video payload. Only names completely known
  before production are `ahead-mapped`; late or production-dependent names are
  retrieval-only and receive no future-prefetch credit.
- **FR-019**: Existing normal and Targeted NDNSF services, control-message
  terminal behavior, `VideoPacket` semantics, decoder policy, camera stop
  behavior, and large-data paths MUST remain unaffected. The current UAV-owned
  XOR publication/recovery implementation MUST be replaced by the equivalent
  optional Spec 119 opaque-byte FEC setting, with disabled mode preserved.
- **FR-020**: Mapping and payload retrieval MUST use exact Interests with
  `CanBePrefix=false` and no ApplicationParameters. Freshness and
  `MustBeFresh` are cache-selection policy, not authorization, replay, or
  replacement semantics; safe cache reuse depends on version-unique immutable
  names and full admission validation.
- **FR-021**: Before exposing a descriptor or frontier, the Provider MUST
  successfully create a Spec 119 `LiveStreamPublisher`, whose registered
  Mapping/payload prefixes and readiness are reflected in the descriptor. Stop,
  replacement, and restart MUST stop the Publisher/Consumer handles and clear
  UAV key/decrypt/decoder and handle-owned FEC state. Generic route, pending, deduplication,
  priority, expiry, and cancellation behavior MUST come from Spec 119 rather
  than a UAV-owned table. Because NFD creates PIT state before the application
  callback, deployment MUST separately configure/record ingress rate, accepted
  Interest-lifetime and PIT-resource safeguards and monitor PIT occupancy.
  Mapping-block
  retention MUST cover every advertised retained payload through the descriptor
  checkpoint; evicting a required block MUST atomically advance payload
  `oldest_retained_cursor` beyond its range and appear in the next descriptor snapshot.
- **FR-022**: Each cursor's ciphertext and signed payload Content MUST be
  materialized at most once and reused byte-for-byte for duplicate Interests or
  retransmission. Eviction, partial publication, or signing failure MUST never
  trigger re-encryption under the same cursor-derived nonce.
- **FR-023**: The producer ordering MUST be `encode VideoPacket -> AES-GCM ->
  opaque bytes -> LiveStream publish/publishGroup`. The consumer ordering MUST
  be `LiveStream Mapping/Provider/FEC admission -> opaque bytes -> strict UAV
  AEAD/replay admission -> VideoPacket decode/reorder/decoder`. No Core type or
  callback receives the Stream key, nonce salt, plaintext VideoPacket, or a
  crypto operation.
- **FR-024**: When UAV FEC is enabled, semantic source and repair names MUST be
  reserved before production and the encrypted source byte strings MUST be
  passed to Spec 119 `publishGroup`. A `FecRecovered` item MUST pass the same UAV
  AAD, AEAD, session, and replay checks as signed source Data before acceptance.
- **FR-025**: UAV MUST remove or disable its duplicate parity generation,
  `FecFrameState`, and XOR recovery authority after migration. UAV may retain
  video-specific configuration/UI counters, group/sample boundaries, and
  decoder diagnostics, but generic FEC status comes from the LiveStream handle.

### Key Entities

- **VideoStreamSessionKey**: One volatile 256-bit key context bound to a StreamId,
  session epoch, key epoch, nonce salt, Provider, service, and creation time.
- **VideoStreamDescriptor**: The camera-start response fields used to locate
  and decrypt the current session and validate its Mapping authority. It is
  application payload, not a generic NDNSF message extension.
- **StreamNameMapBlock**: Provider-signed immutable bindings from a bounded
  consecutive cursor range to original application Data names or permanent
  tombstones. It contains no application payload.
- **EncryptedVideoPacket**: Authenticated ciphertext of one unchanged encoded
  `VideoPacket`, published under its mapped original NDN Data name and bound to
  its Mapping/cursor/session context.
- **VideoConsumerKeyState**: Ground-station in-memory state accepted from one
  matching successful start response and cleared on stop, replacement, or
  shutdown.
- **ProtectedVideoStreamItem**: Opaque AES-GCM envelope bytes passed into and out
  of the LiveStream API. Core may mark them `SignedData` or `FecRecovered` but
  never decrypts them.

## Success Criteria

- **SC-001**: In a no-loss end-to-end camera run, 100% of video Data delivered
  to the existing decoder uses the original application Data name and was first
  resolved through a Provider-validated Mapping, Provider-validated, and
  successfully decrypted with the accepted session context.
- **SC-002**: Missing permission, copied identifier without key, wrong signer,
  substituted name/session/epoch, replay, corrupted ciphertext, and corrupted
  tag cases produce zero plaintext, FEC, or decoder callbacks.
- **SC-003**: Contract inspection proves exactly one successful camera-start
  response class contains key material and all stop/status/failure/unrelated
  response classes contain none.
- **SC-004**: Across at least 1,000 mapped cursors in each tested session, every
  non-tombstone cursor has exactly one original name, no Mapping/payload name is
  replaced within or across sessions, and no nonce repeats under one key;
  forced duplicate, stale-cache, restart collision, remap, rollback, overflow,
  and post-eviction re-encryption cases stop publication before reuse.
- **SC-005**: Stop/start and Provider-restart tests prove old Stream keys decrypt
  zero packets from the new session.
- **SC-006**: Secret scanning of logs, status output, telemetry, persisted
  files, and retained test evidence finds zero Stream keys or nonce salts.
- **SC-007**: Existing NDNSF message codec and normal/Targeted service
  regressions pass without changed generic Response bytes; Spec 119 Mapping
  contracts pass without creating another network RPC API.
- **SC-008**: Matched 60-second MiniNDN live-video runs at 0% and 5% configured
  loss complete without plaintext fallback, preserve bounded buffers, report
  authentication/decryption failures, and retain terminal outcomes for
  scheduled control requests.
- **SC-009**: With UAV FEC off and on, source bytes entering Spec 119 are already
  AES-GCM envelopes; every delivered or recovered item is decrypted only in the
  UAV callback. One-loss recovery yields the original VideoPacket byte-for-byte,
  while corrupt/two-loss/wrong-signer/wrong-digest/expired cases yield zero
  decrypt acceptance and decoder updates. Source inspection finds no remaining
  UAV-owned XOR publication/recovery authority.

## Assumptions

- The current NDNSF camera-video start response is encrypted through hybrid
  AES-GCM and its MessageKey is wrapped under the service-level
  `/PERMISSION/<service>` NAC-ABE attribute.
- The current Provider signing identity and ground-station Trust Anchor are
  available before video starts.
- A new collision-resistant StreamId is allocated for every new camera session;
  current-session cursors begin at zero and are unique. StreamId and typed
  version components make Mapping/payload names non-reusable across restart.
- Original semantic suffixes are application-defined, but complete names remain
  below a bounded Provider-owned, registered prefix and are globally immutable.
  Names are observable; ahead Mapping additionally reveals batches, timing, and
  unused predictions earlier than ordinary Interests, so names MUST contain no
  secret and this metadata-leakage tradeoff is explicitly accepted.
- An application that cannot commit a name before Data production may still use
  Mapping for retrieval, but that item is ineligible for future-prefetch claims.
- Service-level key sharing is acceptable. Per-operator keys and retroactive
  revocation are explicitly outside scope.
- Live video is ephemeral. Recorded video and durable history continue to use
  the existing encrypted Repo/large-data path.

## Out Of Scope

- Changes to generic NDNSF Request/Response wire formats.
- A second NDNSF RPC service for Mapping or payload transfer.
- A UAV-specific Mapping publisher/fetcher, Face timer loop, producer pending
  table, or independent live-prefetch policy.
- Requester input streams, collaboration streams, DI streams, or general
  service-independent key distribution.
- Per-user Stream keys, group rekey protocols, backward secrecy, DRM, or key
  escrow.
- Video codec, bitrate, camera capture, or FEC algorithm changes. Minimal
  application-owned H264 access-unit/parameter-set/keyframe boundary detection
  needed to prove a decoder-safe join is in scope; Core does not parse codecs.
  Adaptive prefetch policy and generic Mapping mechanics are owned by Spec 119.
- Container, iTiger, GPU, real-UAV, or long-term performance claims.
- A global transparency log or network-wide prevention of a forwarder caching
  invalid same-name Data. This feature rejects such Data and bounds retries; it
  does not claim to eliminate cache-poisoning availability attacks.
