# Feature Specification: SVS PubSub Payload Interoperability

**Feature Branch**: `117-svs-pubsub-payload-interop`

**Created**: 2026-07-16

**Status**: Complete (measured-negative; positive compatibility criteria unmet)

**Input**: Add an NDNSF-owned bidirectional SVSPubSub interoperability test
between the C++ NDN-SVS implementation and a real TypeScript NDNts peer. The
test must exchange text, binary data containing zero and non-UTF-8 bytes, a
large object, and a segmented object, and verify exact length and SHA-256 in
both directions.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Verify Bidirectional Application Payloads (Priority: P1)

As an NDNSF maintainer, I can run one independently owned interoperability
suite and determine whether C++ NDN-SVS and NDNts understand each other's
SVS-PS mappings, publication names, encapsulation, segmentation, and payload
bytes rather than merely converging their StateVectors.

**Why this priority**: StateVector convergence alone does not prove that either
implementation can retrieve or decode the other implementation's application
publication.

**Independent Test**: Start one C++ peer and one TypeScript peer, publish the
same deterministic four-case corpus from both peers, and compare every received
name, byte length, SHA-256 digest, and direction against the corpus manifest.

**Acceptance Scenarios**:

1. **Given** a text payload and a binary payload containing zero and non-UTF-8
   bytes, **when** each peer publishes to the other, **then** both receivers
   report the exact expected name, length, and SHA-256 digest.
2. **Given** a larger single-object payload and a payload that cannot fit in one
   NDN Data packet, **when** each peer publishes them, **then** both receivers
   reassemble and report byte-identical payloads without truncation,
   duplication, or substitution.
3. **Given** a mapping, fetch, validation, or reassembly incompatibility,
   **when** the suite reaches its bounded deadline, **then** it reports the
   direction, payload case, last completed protocol stage, and observed packet
   evidence without claiming application-data interoperability.

---

### User Story 2 - Verify Behavior on MiniNDN Links (Priority: P2)

As a reviewer, I can distinguish a localhost-only success from real
cross-implementation behavior over NFD and bounded packet loss.

**Why this priority**: Both implementations must register and fetch through
their normal NFD paths; injecting routes to transient local application faces
or using a shared in-process forwarder would not prove deployable
interoperability. MiniNDN may install the explicit inter-host topology route to
the neighbor NFD.

**Independent Test**: Run fresh MiniNDN cells at 0% and 5% loss after the
standalone gate passes, using the same immutable peers and corpus, and inspect
machine-readable per-direction receipts and packet captures.

**Acceptance Scenarios**:

1. **Given** two MiniNDN hosts and unmodified peer registration, **when** the
   corpus is exchanged at 0% loss, **then** every expected payload is delivered
   exactly once in both directions within the bounded deadline.
2. **Given** the same identities and corpus on a 5% loss link, **when** bounded
   protocol retransmission is exercised, **then** every payload either passes
   exact verification or produces a preserved, classified negative result.

### Edge Cases

- An empty payload is excluded because C++ segmented publication rejects an
  empty object; zero-byte handling is instead exercised inside a non-empty
  binary payload.
- Duplicate callbacks must not satisfy a missing expected payload and must be
  reported separately.
- A received payload with the correct length but wrong bytes must fail the
  SHA-256 gate.
- A StateVector update without a matching Mapping response or publication Data
  must be classified as partial sync, not payload interoperability.
- A failed standalone gate prevents the expensive MiniNDN matrix; the failure
  remains evidence and is not retried into a pass.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The interoperability peer sources, orchestration, fixtures, and
  evidence tooling MUST remain in the NDNSF repository; NDN-SVS production and
  unit-test trees MUST remain unchanged by this feature.
- **FR-002**: The TypeScript peer MUST use the pinned NDNts SVS publisher and
  subscriber APIs, and the C++ peer MUST use the public NDN-SVS SVSPubSub API.
- **FR-003**: Both directions MUST exchange a deterministic corpus containing
  UTF-8 text, binary bytes including `0x00` and invalid UTF-8 octets, a large
  object, and an object guaranteed to require segmentation.
- **FR-004**: Each expected and received payload receipt MUST include direction,
  application name, case identifier, byte length, SHA-256 digest, and sequence
  number; segmented cases MUST also record the configured or observed segment
  boundary evidence.
- **FR-005**: Acceptance MUST compare full names, lengths, and SHA-256 digests;
  StateVector equality, callback count, or payload length alone MUST NOT count
  as application-data success.
- **FR-006**: Mapping retrieval, outer Data retrieval, inner Data decoding,
  validation, and segment reassembly failures MUST be bounded and classified
  separately where the implementation exposes that information.
- **FR-007**: The standalone suite MUST run before MiniNDN and MUST stop the
  acceptance campaign on an interoperability failure without modifying either
  implementation or silently introducing a compatibility adapter.
- **FR-008**: MiniNDN validation MUST use separate C++ and TypeScript hosts,
  normal local producer registration, packet capture, and distinct 0% and 5%
  loss cells. The harness MAY install the explicit inter-host sync-group route
  required by the topology, but MUST NOT inject routes to transient local
  application faces or publication names to compensate for failed producer
  registration.
- **FR-009**: Results MUST preserve negative outcomes and distinguish
  implemented, executed, and measured claims; only complete bilateral payload
  receipts may support a compatibility claim.
- **FR-010**: The existing SVS V3 StateVector interoperability tests and their
  canonical results MUST remain valid and separately identified from payload
  interoperability.

### Key Entities

- **Payload Case**: Stable case identifier, application name, deterministic
  bytes, expected length and digest, and whether segmentation is required.
- **Payload Receipt**: One sender-to-receiver observation containing the
  publication sequence, application name, length, digest, and protocol-stage
  diagnostics.
- **Interop Cell**: Immutable peer identities, link loss, corpus identity,
  deadline, receipts, rejects, packet evidence, and final verdict.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The standalone gate accounts for 100% of the four expected
  payloads in each direction using exact name, length, and SHA-256 checks.
- **SC-002**: Every segmented payload receipt equals its source bytes after
  reassembly and contains evidence that more than one segment was involved.
- **SC-003**: Each admitted 0% and 5% MiniNDN cell accounts for 100% of expected
  bilateral payloads, contains zero unexplained duplicates, and records zero
  unclassified errors.
- **SC-004**: A failure identifies the affected direction and case within one
  bounded run and leaves a reproducible artifact rather than a false success
  or indefinite wait.
- **SC-005**: A repository inspection finds no feature-owned peer, Node
  dependency, orchestration, or evidence file added to the NDN-SVS source tree.

## Assumptions

- The fixed NDNts dependency version and the current local NDN-SVS candidate
  are the interoperability subjects; upgrading either dependency is separate
  work.
- Shared HMAC test credentials authenticate protocol packets but do not claim
  production trust provisioning.
- The test may reveal that core SVS V3 synchronization is compatible while
  SVS-PS application transfer is not. Such a result is a valid feature outcome
  but does not satisfy the positive compatibility criteria.
- Any protocol repair discovered by this suite requires a separately audited
  change in the owning implementation; this feature does not patch around it.
