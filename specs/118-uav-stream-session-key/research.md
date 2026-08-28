# Research Decisions: UAV Stream Session-Key Delivery

NDN naming, exact Interest/Data semantics, typed components, and trust
relationships are constrained by the current packet specification, naming
conventions, and trust-schema report:

- https://docs.named-data.net/NDN-packet-spec/current/interest.html
- https://docs.named-data.net/NDN-packet-spec/current/data.html
- https://named-data.net/wp-content/uploads/2021/10/ndn-tr-22-3-ndn-memo-naming-conventions.pdf
- https://named-data.net/publications/techreports/ndn-0030-2-trust-schema/

Accordingly, one explicit name never identifies different wire Data, Freshness
is not a security/replay decision, and a signature is accepted only when its
signer-to-name relation matches the descriptor-pinned Drone identity.

## Decision 1 - Keep Key Delivery In Camera Start Payload

**Decision**: Add key material only to the successful UAV camera-video
`action=start` application payload.

**Rationale**: The current workflow already returns StreamId, session, prefix,
and fetch hints through this response. Adding the current key context here is
the smallest authenticated and authorized extension.

**Alternatives considered**:

- Generic `ResponseMessage` fields: rejected because keys are application data
  and other services have no Stream.
- A new key-distribution service: rejected because the encrypted start response
  already performs the required permission gate.
- Attach key material to ACK: rejected because ACK precedes selection/execution
  and should not expose a session that may never start.

## Decision 2 - Preserve Service-Level Permission Semantics

**Decision**: Rely on the existing Response mapping to
`/PERMISSION/<camera-video-service>`.

**Rationale**: Source inspection confirms ACK/Response hybrid keys use the
Provider-output permission attribute. This matches the accepted policy that
permitted members may share the live Stream.

**Alternatives considered**:

- Encrypt uniquely to one requester: rejected as a separate group-key and
  multi-consumer design.
- Treat StreamId as authority: rejected because names are observable and
  copyable.

## Decision 3 - Keep Semantic Data Names And Add A Signed Name Map

**Decision**: Publish payload Data under the application's original meaningful
name. Use Spec 119's bounded `StreamNameMap` to bind a monotonic internal cursor
to that exact name before the advertised production horizon.

**Rationale**: A transport-only suffix makes the payload name predictable but
discards application meaning. An immutable signed Mapping preserves original
NDN names while giving the prefetch controller a numeric ordering domain. A
Mapping published only after Data production adds an RTT and cannot support the
paper-inspired future-Interest claim, so lateness is measured and disqualifying.
This Mapping is an NDNSF manifest-like adaptation, not a mechanism claimed by
Gusev et al.; their future Interests use directly constructible sequential Data
names. NDNSF accepts the extra indirection only to preserve application names
that cannot be derived from a cursor.

The complete application name must still be immutable, routable, and known
before production to qualify as `ahead-mapped`. It is scoped below a
Provider-owned registered prefix and contains fresh session/version plus typed
sample/segment components. A production-dependent name remains
`retrieval-only`; Mapping cannot make an unknown future name predictable.

**Alternatives considered**:

- Keep `<streamPrefix>/<packetSeq>` as the payload name: rejected by the
  semantic-name requirement.
- Embed the original NDN Data inside transport Data: rejected because it adds
  packet-in-packet overhead, duplicates signatures, and obscures cache identity.
- Require every application name to follow one framework template: rejected
  because Stream is application-neutral.

## Decision 4 - Encrypt The Existing VideoPacket Bytes

**Decision**: Keep `VideoPacket` and encrypt its current encoded bytes before
placing them in NDN Data Content.

**Rationale**: This preserves capture, FEC, frame metadata, and receiver logic.
`VideoPacket` is an application record, not nested NDN Data; the outer name is
the mapped original application name.

**Alternatives considered**:

- Replace VideoPacket with generic StreamChunk on the wire: rejected as an
  unrelated migration.
- Encrypt only H264 payload bytes: rejected because unauthenticated metadata
  could redirect FEC/reassembly behavior.

## Decision 5 - Deterministic Unique Nonce From Salt And Cursor

**Decision**: Use a random 4-byte session salt followed by the 8-byte internal
Stream cursor in network order.

**Rationale**: The cursor is unique within a StreamId and provides a simple
auditable 96-bit GCM nonce without constraining the original Data name. A cursor
is committed once to a name or tombstone; fail-closed remap/rollback checks
prevent reuse. Ciphertext is materialized once and retained with the signed
Content, so duplicate Interests and retransmissions reuse identical bytes;
eviction never causes re-encryption under the old nonce.

**Alternatives considered**:

- Random nonce per packet: cryptographically acceptable but harder to audit for
  collisions and does not use the requested nonce-salt descriptor.
- Hash of the Data name: rejected because collision/encoding rules are less
  transparent than an explicit salt/sequence construction.

## Decision 6 - Keep Provider Signature And Add Consumer Validation

**Decision**: Retain one Provider signature per Mapping and payload Data, and
explicitly validate Mapping provenance and signer-to-original-name authority
before symmetric decryption.

**Rationale**: Every permitted consumer knows the shared AES key and could
otherwise forge valid AEAD records. The asymmetric signature preserves Provider
provenance and protects caches from a malicious group member.
`Provider-signed` alone is not sufficient: the descriptor pins the expected
Drone identity and roots, and separate Trust Schema rules require both Mapping
and payload names to be signed by that identity under the configured Trust
Anchor. A different valid Provider certificate is rejected.

**Alternatives considered**:

- AEAD only: rejected because shared-key holders become indistinguishable
  publishers.
- One signed manifest for many packets: deferred because it changes discovery,
  buffering, and recovery beyond this feature.

## Decision 7 - Reuse HybridMessageEnvelope, Not A New Packet Family

**Decision**: Reuse the existing AES-GCM envelope fields with an explicit-nonce
helper and an application-specific message type.

**Rationale**: This reuses the established field layout, tag handling, and
crypto dependencies while keeping the inner payload as the current
VideoPacket. The current generic decoder accepts unknown fields and does not
detect duplicates, so the UAV path adds an application-owned strict wrapper
that verifies one occurrence of every required field, rejects unknown or
wrapped-key fields, and checks exact sizes before using the decoded envelope.
The generic decoder and generic NDNSF message wire remain unchanged.

**Alternatives considered**:

- Custom fixed binary envelope: rejected as duplicate crypto framing.
- Reuse NDNSF Response envelope semantics wholesale: rejected because video
  Data has different names, lifetime, and key distribution.

## Decision 8 - No Automatic Plaintext Or Name Compatibility

**Decision**: One active session is entirely encrypted and Mapping-authoritative.
Missing keys/Mapping, malformed descriptors, validation failures, or decryption
failures stop delivery rather than trying the prior plaintext decoder or
constructing the former transport-only name.

**Rationale**: Automatic fallback would let tampering or configuration errors
downgrade confidentiality without operator visibility.

**Alternatives considered**:

- Per-packet format sniffing: rejected as a downgrade oracle.
- Permanent dual mode: rejected because it complicates testing and security.

## Decision 9 - Mapping Is Signed But Not Confidential

**Decision**: Mapping Data is Provider-signed and integrity-checked, but its
original-name entries are not confidential; the application explicitly accepts
the additional timing and batch-metadata leakage.

**Rationale**: The resolved original names appear in subsequent NDN Interests
and Data names. Encrypting only the Mapping would not hide them on the data
plane and would add a second AEAD nonce domain. Nevertheless, ahead Mapping
reveals future names, grouping, volume, and unused predictions earlier than
ordinary retrieval. Names therefore contain no secrets, and payload
confidentiality—not traffic-analysis resistance—is the claim.

**Alternatives considered**:

- Encrypt Mapping with the payload key: rejected as ineffective name privacy
  and additional nonce/key-domain complexity.
- Leave Mapping unsigned: rejected because forged bindings enable cache
  poisoning and ciphertext substitution attempts.

## Decision 10 - Generic Optional FEC Works On Ciphertext, Not Plaintext

**Decision**: UAV encodes and AES-GCM-protects each source item before calling
Spec 119. When enabled, Spec 119 computes and recovers one XOR repair over those
opaque envelope bytes. The UAV callback decrypts ordinary and `FecRecovered`
items identically, then owns VideoPacket sample/reorder/decoder policy.

**Rationale**: Exact-name retrieval, recovery deadlines, FEC bounds, Provider-
signed group commitments, and byte reconstruction are reusable stream
mechanics. Encryption keys, media parsing, safe join, playout, and decoder state
are application semantics. This boundary gives NDNSF the requested optional FEC
API without letting Core see a key/plaintext or making UAV the hidden generic
implementation. Signed group metadata plus source digest authenticates locally
recovered opaque bytes; UAV AEAD remains the final content-admission gate.

**Alternatives considered**:

- Keep XOR generation/recovery in UAV: rejected because completing Spec 119
  would still not deliver a generic optional-FEC API.
- FEC over plaintext: rejected because it crosses the encryption boundary.
- FEC over full encoded NDN Data packets: rejected because it recreates
  packet-in-packet-like overhead and couples recovery to a wire packet.
- A general erasure-code plugin SPI: deferred; `None` plus bounded one-repair
  XOR is the smallest useful v1 contract.
