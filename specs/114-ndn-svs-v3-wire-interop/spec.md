# Feature Specification: NDN-SVS V3 Wire Compatibility and Interoperability

**Feature Branch**: `Experimental`

**Created**: 2026-07-16

**Status**: Draft — audited design only; implementation not started

**Input**: Bring the current NDN-SVS Experimental implementation into complete
agreement with the published State Vector Sync Version 3 core wire protocol,
preserve an explicit legacy V2 path where required for migration, isolate fork
extensions from the standard core envelope, and prove bidirectional
interoperability with an independent V3 implementation.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Exchange Standard V3 Sync Packets (Priority: P1)

An application using the C++ Experimental library can join the same sync group
as an independent, strict V3 peer. Either peer can publish first, and both peers
converge on the same state without a compatibility shim or private packet
interpretation.

**Why this priority**: The current hybrid packet combines a V2 Sync Interest
name and raw application parameters with V3 state-vector entries. That packet
cannot provide standards-based interoperability even though local homogeneous
tests pass.

**Independent Test**: Capture packets from one C++ peer and one independent V3
peer, verify the normative names and envelope, then publish from each side and
observe the corresponding update exactly once at the other side.

**Acceptance Scenarios**:

1. **Given** a C++ node configured for V3, **When** it emits a Sync Interest,
   **Then** the Interest uses the V3 group name, carries a parameters digest,
   and contains a signed State Vector Data packet.
2. **Given** an independently produced valid V3 Sync Interest, **When** the C++
   node receives it, **Then** it validates the embedded Data, decodes the state
   vector, and reports only the missing ranges.
3. **Given** one new publication on either implementation, **When** the Sync
   Interest is delivered, **Then** the other implementation converges without
   timeout, duplicate update, or private fallback.

---

### User Story 2 - Reject Invalid V3 State Safely (Priority: P1)

An operator can expose a V3 sync group to malformed, unsigned, incorrectly
named, stale, or future-dated packets without corrupting local state or losing
the participant process.

**Why this priority**: Sync state controls which publications are fetched.
Accepting an invalid envelope or partially applying an invalid vector can create
state divergence, while an uncaught validation exception can remove a node from
the group.

**Independent Test**: Feed individually corrupted V3 packets through both the
serial and parallel receive paths and prove that every packet is rejected as a
whole, the state digest is unchanged, no update callback fires, and the next
valid packet still succeeds.

**Acceptance Scenarios**:

1. **Given** an embedded Data packet with an invalid signature under the
   configured validation policy or a wrong V3 Data name, **When** it is received,
   **Then** the complete Sync Interest is rejected without state mutation.
2. **Given** a state vector containing sequence number zero or a bootstrap time
   more than 86400 seconds in the future, **When** it is decoded, **Then** the
   complete vector is ignored and the process remains usable.
3. **Given** a valid packet after any rejected packet, **When** it is processed,
   **Then** synchronization resumes normally in both processing modes.

---

### User Story 3 - Select One Explicit Protocol Version (Priority: P1)

An application or operator can deliberately run V3 or the retained V2
compatibility mode. The selected version determines the name, envelope,
security object, defaults, and receive route; the library never silently emits
or accepts a hybrid packet.

**Why this priority**: Existing users may still depend on the upstream C++ V2
format, but automatic cross-version fallback would make security verification
and deployment state ambiguous.

**Independent Test**: Start otherwise identical nodes in V2 and V3 modes,
capture their packets, verify that each mode uses only its declared contract,
and prove that mixed-version nodes remain isolated with a visible diagnostic.

**Acceptance Scenarios**:

1. **Given** no explicit version override in the Experimental candidate,
   **When** a new sync participant starts, **Then** it uses the complete V3
   contract rather than the historical hybrid format.
2. **Given** explicit V2 mode, **When** the participant sends and receives Sync
   Interests, **Then** reviewed V2 behavior remains available without V3 fields
   being partially applied.
3. **Given** V2 and V3 nodes under the same base group prefix, **When** they run
   concurrently, **Then** their versioned routes do not cross-apply state, each
   node exposes its selected profile, and the interop harness reports the
   incompatible profile pair without requiring broad-prefix packet capture.

---

### User Story 4 - Use Fork Extensions Without Redefining Core V3 (Priority: P2)

NDNSF can continue using sparse mappings, bounded repair, and segmented
publication recovery while a peer interested only in core V3 can ignore the
extension portion and still synchronize the core state vector.

**Why this priority**: Mapping and Repair data deliver real fork value, but they
are not defined by the linked core V3 specification and must not alter the
standard State Vector Data encoding.

**Independent Test**: Exchange the same core V3 packet with extensions enabled,
disabled, unknown, and malformed; prove that standard state synchronization is
unchanged and extension failures never partially change authoritative core
state.

**Acceptance Scenarios**:

1. **Given** extensions are disabled, **When** a V3 packet is emitted, **Then**
   its core envelope is a valid standalone V3 packet.
2. **Given** Mapping or Repair metadata follows a valid State Vector Data
   packet, **When** a peer does not implement that extension, **Then** it can
   ignore the trailing extension and still process the core state.
3. **Given** a malformed private extension after a valid core vector, **When** a
   fork peer receives it, **Then** extension processing fails closed without
   rolling back or duplicating the already validated core transition.

---

### User Story 5 - Produce Reviewable Interoperability Evidence (Priority: P2)

A maintainer can decide whether the candidate is ready to merge using
reproducible wire vectors, unit tests, bidirectional interoperation, MiniNDN
network evidence, and NDNSF regression evidence bound to one immutable source
identity.

**Why this priority**: Homogeneous unit tests currently encode the same V2
assumption as the implementation and therefore cannot prove V3 compatibility.

**Independent Test**: Rebuild from the candidate commit, execute the validation
guide from a clean environment, and reproduce the same packet digests,
acceptance counts, peer convergence, and negative-test outcomes.

**Acceptance Scenarios**:

1. **Given** published V3 examples and independently generated packets, **When**
   contract tests run, **Then** encoding and decoding agree byte-for-byte on all
   normative fields.
2. **Given** C++ and independent V3 peers in MiniNDN, **When** each publishes a
   bounded sequence under 0% and controlled-loss conditions, **Then** all peers
   converge or the candidate is rejected with preserved negative evidence.
3. **Given** the corrected NDN-SVS library is installed for NDNSF, **When** the
   focused segmented-response and Targeted regressions run, **Then** Spec 113
   correctness remains intact or the candidate is rejected.

### Edge Cases

- Empty state vectors before the first publication.
- Multiple bootstrap epochs for the same node name, including a rejoining node.
- Bootstrap times at exactly current time plus 86400 seconds and one second
  beyond that boundary.
- A valid embedded Data packet followed by unknown extension TLVs.
- A valid core packet followed by truncated or malformed Mapping/Repair data.
- Wrong version component, missing parameters digest, wrong embedded Data name,
  absent signature, invalid signature, malformed Content, duplicate entries,
  non-canonical names, and sequence number zero.
- V2 and V3 participants sharing the same base prefix during migration.
- Serial versus parallel parsing and production, including stale worker results.
- Local publication while a periodic or suppression timer is already pending.
- Restart with a preserved bootstrap time versus restart after persisted state is
  unavailable.
- Compression requested in V3 without an explicitly negotiated extension
  profile.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The library MUST expose one explicit protocol-version selection
  for every Core, SVSync, SVSyncShared, and SVSPubSub participant, and the
  Experimental candidate MUST default new participants to V3.
- **FR-002**: V3 Sync Interests MUST use
  `/<group-prefix>/v=3/<parameters-digest>` and V3 State Vector Data MUST use
  `/<group-prefix>/v=3`.
- **FR-003**: V3 ApplicationParameters MUST begin with a complete signed NDN
  Data packet whose Content contains exactly one V3 StateVector TLV.
- **FR-004**: V3 participants MUST sign the embedded State Vector Data and MUST
  direct any configured signature/trust validation to that Data; validating or
  signing only the outer Interest MUST NOT satisfy V3 security. When no trust
  validator is configured, the effective unverified policy MUST be explicit and
  observable rather than silently represented as successful validation.
- **FR-005**: V3 receive registration and parsing MUST require the selected V3
  name and envelope and MUST NOT accept a V2 or hybrid packet as V3.
- **FR-006**: The V3 Sync Interest lifetime MUST default to 1 second; the
  periodic timer MUST default to 30 seconds with ±10% uniform jitter; and the
  suppression period MUST default to 200 milliseconds with a bounded random
  timeout generated by the specification's exponential-decay function.
- **FR-007**: A new local publication MUST schedule the corresponding V3 Sync
  Interest immediately unless an explicitly documented experimental batching
  option is enabled; batching status and delay MUST be observable.
- **FR-008**: V3 StateVector encoding MUST use TLV types 201, 202, 210, 212, and
  214, group entries in canonical NDN name order, and preserve every distinct
  `[BootstrapTime, SeqNo]` tuple.
- **FR-009**: Encoded StateVector entries MUST use positive, one-indexed sequence
  numbers; zero MUST represent absence only and MUST NOT be accepted as an
  encoded entry.
- **FR-010**: A state vector containing any BootstrapTime more than 86400 seconds
  in the future MUST be ignored atomically without exception escape, state
  mutation, callback, or loss of subsequent progress.
- **FR-011**: A participant MUST accept a caller-supplied preserved bootstrap
  time and expose the active value so an application can persist and reuse it;
  when unavailable, it MUST use the current timestamp.
- **FR-012**: Data publication and fetch naming MUST retain the V3 bootstrap
  epoch in the application Data name, and re-bootstrap epochs MUST remain
  distinguishable during comparison, merge, mapping, repair, and fetch.
- **FR-013**: Serial and parallel send/receive paths MUST produce and enforce the
  same version, envelope, validation, error, timer, extension, and state-update
  semantics.
- **FR-014**: Explicit V2 mode MUST preserve the reviewed upstream-compatible V2
  wire behavior and MUST remain isolated from V3 routes and validation rules.
- **FR-015**: The implementation MUST NOT automatically downgrade, dual-publish,
  or reinterpret a packet across versions. Every participant MUST expose its
  selected profile at startup; direct wrong-version injection MUST produce a
  bounded rejection diagnostic, while the interop harness MUST diagnose
  naturally route-isolated V2/V3 peer pairs from their declared profiles.
- **FR-016**: MappingData and RepairData MUST be documented and processed as a
  fork extension profile after the standard V3 State Vector Data, not inside its
  Content and not as a replacement for the signed Data envelope.
- **FR-017**: Unknown trailing extension blocks MUST be ignorable by core-only
  peers; malformed known extensions MUST NOT partially apply extension state or
  corrupt the validated core state transition.
- **FR-018**: Whole-parameters LZMA wrapping MUST be disabled in V3 unless a
  separately versioned and explicitly negotiated extension profile defines its
  encoding and interoperability tests.
- **FR-019**: Tests MUST cover V3 packet production and consumption independently
  rather than constructing fixtures with the same helper used by production.
- **FR-020**: Validation MUST include byte-level golden vectors, negative packet
  vectors, C++-to-independent-peer and independent-peer-to-C++ exchange, V2
  regression, re-bootstrap, serial/parallel equivalence, and extension
  isolation.
- **FR-021**: Final network validation MUST use MiniNDN and bind the exact C++
  source commit, independent-peer package/version, dependency versions,
  configuration, commands, packet digests, and result artifacts.
- **FR-022**: The corrected library MUST rebuild affected NDNSF consumers and
  rerun the focused Spec 112/113 segmented-response, Targeted timeout, and
  provider-liveness regressions before any merge-readiness claim.
- **FR-023**: This feature MUST preserve the four reviewed Spec 113 reliability
  commits as independent concerns; V3 framing changes MUST be reviewable and
  revertible without discarding those fixes.
- **FR-024**: Completion MUST include strict Spec Kit structure/coverage checks,
  a post-implementation code-aware audit, and convergence with zero unresolved
  Critical or High finding.
- **FR-025**: V3 participants MUST NOT emit a Sync Ack Data packet in response
  to a Sync Interest; reconciliation MUST occur only through a subsequent Sync
  Interest according to the steady/suppression state machine.
- **FR-026**: NDNSF ServiceUser, ServiceProvider, and any shared operator
  configuration surface that launches them MUST preserve the selected protocol
  profile's resolved timer defaults when no timer override is supplied. In
  particular, missing `NDNSF_SVS_MAX_SUPPRESSION_MS` MUST leave V3 at 200 ms;
  an explicit override (including the historical 1 ms experiment value) MUST be
  applied and logged, and an invalid protocol version MUST fail startup rather
  than fall back silently.

### Key Entities

- **ProtocolProfile**: Selected V2 or V3 contract, including the versioned route,
  packet envelope, signing target, validation target, and timer defaults.
- **StateVectorData**: The signed V3 Data packet named by the group/version whose
  Content carries the canonical StateVector.
- **StateVector**: Canonically ordered node entries containing one or more
  bootstrap-epoch/positive-sequence tuples.
- **BootstrapEpoch**: Seconds since the Unix epoch identifying one node session;
  it may be restored by the application or regenerated after state loss.
- **ExtensionProfile**: Optional, non-core trailing blocks such as MappingData
  and RepairData, with independent parsing and failure boundaries.
- **InteropCandidate**: Immutable combination of C++ source, dependency
  identities, independent peer, protocol configuration, tests, and evidence.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: One hundred percent of normative V3 packet vectors match the
  expected name, parameters digest, embedded Data name, signature presence,
  StateVector TLVs, and canonical ordering byte-for-byte.
- **SC-002**: One hundred percent of malformed, cross-version, unsigned,
  incorrectly named, sequence-zero, and future-bootstrap negative vectors are
  rejected without state change, callback, process loss, or subsequent valid
  packet failure in both serial and parallel modes.
- **SC-003**: In three clean 0%-loss MiniNDN repetitions, 20 C++ publications and
  20 independent-peer publications per repetition yield coverage of 120/120
  unique remote sequence numbers, allowing callbacks to batch contiguous
  ranges, with zero duplicate sequence coverage, identical final vectors, and
  zero process restart.
- **SC-004**: In three controlled 5%-loss MiniNDN repetitions of the same bounded
  workload, every peer converges within 60 seconds after the final publication;
  any failure is retained as a failed candidate rather than rerun or tuned away.
- **SC-005**: Explicit V2 contract tests retain 100% of the pre-change V2 golden
  vectors, while all V2/V3 mixed-version tests show zero cross-applied state,
  both selected profiles in evidence, and a harness-level incompatibility
  diagnostic; direct wrong-version injection also produces one rejection
  diagnostic.
- **SC-006**: Extension-disabled, known-extension, unknown-extension, and
  malformed-extension cases all preserve the same authoritative core V3 vector;
  malformed known extensions produce zero partial extension update.
- **SC-007**: Bootstrap reuse and re-bootstrap tests preserve distinct epochs and
  retrieve 100% of the expected Data names without aliasing one epoch to another.
- **SC-008**: A clean rebuild passes 100% of NDN-SVS unit/interop tests and all
  affected focused NDNSF Spec 112/113 regressions, with no stale-header or
  stale-library identity mismatch.
- **SC-009**: Final traceability maps every FR and SC to executable tasks and
  evidence; strict audit and convergence report zero Critical, zero High, zero
  placeholder, and zero unchecked task before completion is declared.
- **SC-010**: Across valid, outdated, malformed, and suppression-triggering Sync
  Interest tests, the participant emits zero Sync Ack Data packets and uses only
  the expected Sync Interest reconciliation path.
- **SC-011**: NDNSF user/provider construction and its shared GUI/environment
  adapter resolve an unset V3 configuration to 200 ms, preserve an explicit
  1 ms override, select explicit V2, and reject an invalid version in 100% of
  focused configuration tests.

## Assumptions

- The linked State Vector Sync V3 page dated 2025-01-14 is the normative core
  protocol source for this feature, even where an existing C++ implementation
  still emits V2 packets.
- NDNts with its explicit V3 option is the first independent interoperability
  oracle because it separately implements V2 and V3 framing; tests pin an exact
  package/source identity rather than floating latest.
- Existing Spec 113 publication transactions, sparse mappings, bounded recovery,
  and callback-lifetime fixes are correct baselines and are changed only where
  the V3 envelope requires adaptation.
- MiniNDN is the authoritative network-validation environment for this feature;
  physical Wi-Fi, containers, and iTiger are deferred.
- V2 compatibility is explicit and temporary but has no deletion date in this
  feature because the current official C++ ecosystem still uses V2 framing.
- V3 is the default for the Experimental candidate; existing deployments that
  require V2 must select it deliberately and cannot interoperate across versions.

## Explicitly Out of Scope

- Rewriting or standardizing SVS-PS beyond isolating the fork extension profile.
- New recovery algorithms, repair heuristics, performance tuning, or publication
  transaction redesign unrelated to V3 framing.
- Merging Experimental into master, pushing any remote ref, or publishing a
  release.
- NDNSF-DI model execution, Docker, iTiger, physical UAV, or Wi-Fi validation;
  the shared NDNSF-DI GUI environment adapter is in scope only to prevent it
  from silently overriding the selected SVS profile.
- Treating current homogeneous C++ tests as interoperability evidence.
- Claiming LZMA, RepairData, or other private TLVs are part of core SVS V3.
