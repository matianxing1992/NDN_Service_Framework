# Feature Specification: DistributedRepo Large-Artifact Transport

**Feature Directory**: `specs/164-distributed-repo-large-artifact-transport`

**Created**: 2026-07-29

**Status**: Draft — ready for planning

**Input**: Improve NDNSF-DistributedRepo API usability and large-object
throughput. Authenticate a scalable manifest once with an asymmetric
publisher signature, verify bulk Data through manifest-bound cryptographic
digests, avoid per-packet asymmetric-verification cost, and determine whether
the current persistence design can sustain line-rate immutable-object
delivery.

## User Scenarios & Testing

### User Story 1 - Publish and retrieve a large trusted artifact (Priority: P1)

An application publisher can publish a large immutable artifact once, and an
authorized consumer can retrieve it at a rate close to the matched raw NDN
transfer ceiling without performing a public-key verification for every Data
packet. The consumer can prove both publisher provenance and complete content
integrity before using the artifact.

**Why this priority**: Large model shards, datasets, checkpoints, and media
objects are not practical if repository overhead dominates the available
network bandwidth or if faster transfer weakens provenance.

**Independent Test**: Publish a large deterministic file, retrieve it through
one repository replica, verify its signed root manifest and complete content
digest, and compare application goodput with a raw segmented NDN transfer over
the same topology and measurement window.

**Acceptance Scenarios**:

1. **Given** a trusted publisher and an artifact not present in the repository,
   **When** the publisher completes publication and a consumer retrieves the
   artifact, **Then** the repository exposes it only after complete integrity
   verification and the retrieved bytes exactly match the publisher's content
   digest.
2. **Given** a valid signed root manifest, **When** a consumer retrieves its
   manifest pages, chunks, and Data packets, **Then** provenance requires one
   bounded set of asymmetric trust validations and bulk verification uses
   manifest-bound cryptographic digests.
3. **Given** a modified Data packet, manifest page, name binding, or final
   object, **When** verification runs, **Then** the artifact is rejected and
   never becomes active or reusable.
4. **Given** an artifact large enough to contain millions of Data packets,
   **When** its manifest is published, **Then** no single root object grows in
   direct proportion to the packet count.

---

### User Story 2 - Resume safely after interruption (Priority: P2)

An operator can resume an interrupted publication or retrieval without
retransmitting already verified content, while partial or corrupt data remains
undiscoverable.

**Why this priority**: Large transfers are long-lived and failures are normal.
Restarting from byte zero wastes bandwidth, time, and TigerCluster allocation.

**Independent Test**: Interrupt a transfer at several deterministic points,
restart the publisher, repository, or consumer, and confirm that only missing
or invalid ranges are transferred and that the final active artifact is
byte-identical to the original.

**Acceptance Scenarios**:

1. **Given** a partially received artifact with verified ranges, **When** the
   same immutable artifact is requested again, **Then** the transfer resumes
   from verified progress rather than restarting.
2. **Given** partial content with a mismatching object identity or manifest,
   **When** a resume is attempted, **Then** the repository refuses to combine
   the two versions.
3. **Given** a repository crash between verification and publication,
   **When** the repository restarts, **Then** the object is either safely
   finalized or remains non-active; no ambiguous visible state is permitted.
4. **Given** an expired upload lease or abandoned partial artifact, **When**
   garbage collection runs, **Then** reclaiming it cannot remove a committed
   artifact or race with a valid active session.

---

### User Story 3 - Use a simple artifact API (Priority: P3)

An application developer can publish or fetch a file using one documented
high-level operation, while advanced users can opt into asynchronous progress,
cancellation, explicit replication, and resumable session control.

**Why this priority**: A high-throughput data plane is not useful if every
application must understand repository control operations, private client
fields, packet batching, or replica-specific details.

**Independent Test**: Implement one minimal publisher and one consumer using
only public APIs, then repeat the same workflow with asynchronous progress,
cancellation, resume, and multiple replicas without accessing private runtime
state.

**Acceptance Scenarios**:

1. **Given** a local file and a logical artifact name, **When** the developer
   invokes the simple publication operation, **Then** it returns a stable,
   content-bound artifact reference after the requested durability condition
   is met.
2. **Given** a stable artifact reference, **When** the developer invokes the
   simple retrieval operation, **Then** the destination is atomically exposed
   only after required verification succeeds.
3. **Given** an asynchronous application, **When** it publishes or retrieves an
   artifact, **Then** it receives bounded progress events and can cancel or
   resume through public APIs.
4. **Given** an application that needs explicit control, **When** it uses an
   advanced upload session, **Then** begin, progress, commit, abort, and receipt
   states have documented idempotency and failure semantics.

---

### User Story 4 - Preserve existing repository behavior during migration (Priority: P4)

An existing application can continue using the exact-packet repository mode
while large immutable artifacts adopt the scalable manifest mode. Operators
can roll forward or back without silently changing the trust semantics of
already stored objects.

**Why this priority**: Exact wire-packet preservation and scalable artifact
distribution serve different needs. Replacing one with the other without an
explicit compatibility contract risks data loss and security regressions.

**Independent Test**: Run old and new object modes in the same repository
deployment, retrieve objects created before and after the upgrade, and roll
back the new publication path while preserving readable committed objects.

**Acceptance Scenarios**:

1. **Given** an object stored in the legacy exact-packet format, **When** an
   upgraded consumer retrieves it, **Then** its existing wire and trust
   semantics remain unchanged.
2. **Given** a large object stored in the manifest-based format, **When** an
   older or incapable consumer requests it, **Then** capability negotiation
   fails explicitly rather than returning incompletely verified content.
3. **Given** a mixed-version replica set, **When** a publisher requests the new
   format, **Then** only capable replicas are committed and the achieved
   durability is reported accurately.
4. **Given** rollback of the new publication path, **When** the repository
   restarts, **Then** committed legacy objects remain available and new-format
   objects are not reinterpreted as legacy objects.

---

### User Story 5 - Produce academically defensible performance evidence (Priority: P5)

A researcher can distinguish physical-network capacity, raw NDN transfer
capacity, repository data-plane cost, cryptographic cost, and persistence cost
using matched, reproducible measurements.

**Why this priority**: A throughput claim is not meaningful unless it is
normalized against the same topology, payload, packet geometry, concurrency,
logging, and measurement window.

**Independent Test**: Run the frozen experiment matrix on a controlled
emulated NDN topology, retain all negative results, and generate a machine-
readable report containing raw samples, environment identity, exact commands,
and derived distributions.

**Acceptance Scenarios**:

1. **Given** a physical or virtual network path, **When** a repository
   benchmark is reported, **Then** it includes matched network and raw NDN
   ceilings rather than comparing against a theoretical link rate alone.
2. **Given** legacy per-packet signatures, digest-only transfer, and
   signed-manifest transfer, **When** the matrix completes, **Then** all three
   modes use the same object bytes, topology, packet geometry, concurrency,
   and measurement interval.
3. **Given** repeated measurements, **When** results are summarized, **Then**
   the report includes sample count, completion and failure counts, goodput
   distribution, tail latency, CPU, memory, disk I/O, retransmissions, and
   control-operation counts.
4. **Given** a failed or slower configuration, **When** the campaign closes,
   **Then** the negative result remains in the canonical evidence and is not
   tuned away or selectively discarded.

### Edge Cases

- The artifact is empty, smaller than one Data packet, exactly one chunk, or
  spans millions of Data packets.
- Two publishers use the same logical name for different content or republish
  a name while an older version is cached.
- The root manifest is valid but a manifest page is missing, cyclic,
  duplicated, oversized, too deep, or refers to content outside its naming
  scope.
- A publisher certificate is expired or revoked after content was cached.
- A consumer trusts the publisher but does not support the manifest version,
  digest algorithm, or signature algorithm.
- A repository receives valid content under an unexpected object name,
  segment geometry, policy epoch, or total size.
- One replica commits while another times out, rejects the policy, loses
  capacity, or restarts.
- The publisher, selected repository, or consumer restarts during begin,
  transfer, verification, commit, receipt publication, or catalog activation.
- The destination already contains a complete matching artifact, a complete
  conflicting artifact, or an interrupted temporary file.
- Concurrent sessions publish the same digest, different digests under the
  same logical name, or contend for the same bounded execution queue.
- Storage becomes full after a positive advisory ACK or after a task is
  queued, and actual stored bytes differ from the declared size.
- A malicious peer floods Interests, manifests, manifest pages, resume ranges,
  or invalid digests to consume CPU, memory, storage, or trust-verification
  budget.

## Requirements

### Functional Requirements

#### Artifact identity and API

- **FR-001**: The system MUST identify every immutable artifact by a
  cryptographic content identity in addition to its human-readable logical
  name.
- **FR-002**: The public artifact reference MUST bind at least the logical
  name, content identity, manifest version, total size, and publisher trust
  context needed for later retrieval and reuse.
- **FR-003**: The system MUST provide one high-level publication operation and
  one high-level retrieval operation that do not expose internal control
  operations, packet batches, replica RPCs, or private client state.
- **FR-004**: The system MUST provide asynchronous publication and retrieval
  with progress, cancellation, timeout, and resume through public interfaces.
- **FR-005**: The system MUST provide an advanced resumable session interface
  with explicit begin, commit, abort, status, and receipt semantics.
- **FR-006**: Repeating a publication, commit, abort, or retrieval operation
  with the same immutable identity and idempotency context MUST have
  deterministic behavior and MUST NOT create conflicting visible objects.
- **FR-007**: Artifact APIs and wire contracts MUST remain generic to immutable
  models, datasets, checkpoints, media, and other large byte objects; they
  MUST NOT depend on LLM, Qwen, ONNX, GPU, or distributed-inference roles.

#### Control plane and data plane

- **FR-008**: Repository discovery, authorization, and replica selection MUST
  use the existing NDNSF control and collaboration security model.
  Repository ACKs MUST carry only advisory queue/capability/capacity
  snapshots and MUST NOT reserve bytes or lock resources. After ACK_CLOSED,
  commit_plan emits exact Selection assignments; each selected Provider MUST
  place accepted work in a bounded execution queue. Any internal ownership
  lease needed for resumable writes, finalization, or GC begins during task
  execution and is not an ACK-side reservation.
- **FR-009**: Bulk artifact bytes MUST travel through an NDN segmented data
  plane rather than one NDNSF service invocation per chunk or Data packet.
- **FR-010**: Control-plane operation count for one artifact MUST be bounded by
  the number of selected replicas and lifecycle transitions, independent of
  artifact bytes and Data-packet count.
- **FR-011**: The data plane MUST support a bounded adaptive request window,
  retransmission, out-of-order arrival, duplicate suppression, and
  backpressure without buffering the full artifact in memory.
- **FR-012**: Publication MUST return the achieved replica receipts and
  durability state; requested replication MUST NOT be reported as achieved
  when only a subset committed.

#### Manifest trust and scalable verification

- **FR-013**: Every manifest-based artifact MUST have a publisher-authenticated
  root manifest validated against the configured NDNSF trust policy.
- **FR-014**: The authenticated root manifest MUST bind the artifact identity,
  logical name or naming scope, total size, packet and chunk geometry, digest
  algorithm, manifest hierarchy root, publisher identity, policy epoch, and
  validity information.
- **FR-015**: The manifest structure MUST be hierarchical or equivalently
  bounded so that no single root object or trust-verification step grows
  linearly with the artifact's Data-packet count.
- **FR-016**: Every manifest page, chunk, and Data range used to reconstruct an
  artifact MUST be transitively authenticated by the validated root manifest
  through collision-resistant cryptographic digests.
- **FR-017**: The new public artifact format MUST NOT require distribution of a
  shared HMAC secret to consumers. HMAC MAY be negotiated for a closed-domain
  session-authentication purpose, but it MUST NOT replace publisher provenance
  or the immutable content identity.
- **FR-018**: Signature and digest algorithms MUST be explicitly identified,
  policy-controlled, and capability-negotiated; the protocol MUST NOT hard-
  code one asymmetric algorithm.
- **FR-019**: The verifier MUST enforce limits on manifest version, encoded
  size, page size, entry count, tree depth, cycles, naming scope, total
  referenced bytes, and cryptographic work before allocating unbounded
  resources.
- **FR-020**: A repository or consumer MUST reject unknown critical fields,
  unsupported algorithms, invalid trust chains, expired policy contexts,
  manifest substitution, name substitution, digest mismatch, truncation,
  extension, and mixed-version resume state.
- **FR-021**: Full-object integrity MUST be verified before an artifact becomes
  active, discoverable, reusable, or available to downstream consumers.
- **FR-022**: Revocation and policy-epoch behavior for already cached manifests
  MUST be explicit and testable; cryptographic content equality MUST NOT be
  confused with currently acceptable publisher provenance.

#### Persistence, lifecycle, and recovery

- **FR-023**: Bulk artifact payload and repository control metadata MUST have
  separate persistence contracts so either can evolve without changing public
  artifact semantics.
- **FR-024**: Payload persistence MUST support streaming range writes, bounded
  range reads, resumable partial state, atomic finalization, existence checks,
  and safe abort.
- **FR-025**: Metadata persistence MUST support manifests, leases, verified
  progress, replica receipts, catalog state, policy state, and garbage-
  collection ownership without requiring one durable metadata record per Data
  packet.
- **FR-026**: Metadata cardinality and routine metadata work MUST scale with
  artifacts, replicas, transfer sessions, chunks, and manifest pages rather
  than 4 KiB Data-packet count.
- **FR-027**: The lifecycle MUST distinguish at least absent, queued,
  receiving, verified, committed, active, failed, and expired states, with
  documented legal transitions. `RESERVED` is retained only as a legacy
  persisted-state migration value and is not entered by the new path.
- **FR-028**: Partial payload MUST remain outside the active namespace and MUST
  become visible only through an atomic state transition after required
  verification and durability.
- **FR-029**: Crash recovery MUST never expose an unverified object, lose a
  valid committed object, merge different immutable identities, or report
  durability without a retained replica receipt.
- **FR-030**: Task queues, internal transfer-session ownership, temporary
  payloads, and garbage collection MUST have bounded lifetime and ownership
  rules that prevent deletion of active, executing, finalizing, or committed
  artifacts. Advisory ACK capacity MUST NOT be counted as reserved storage.
- **FR-031**: The repository MUST support content deduplication without
  allowing a logical-name claim or less-trusted publisher to inherit another
  publisher's provenance.

#### Compatibility and migration

- **FR-032**: The existing exact-packet format MUST remain available for
  objects that require original wire-packet preservation or legacy behavior.
- **FR-033**: The scalable manifest format MUST have an explicit version and
  capability advertisement distinct from the exact-packet format.
- **FR-034**: Mixed-version deployments MUST fail explicitly when requested
  integrity, format, or durability cannot be met; silent downgrade is
  prohibited.
- **FR-035**: Migration MUST preserve committed legacy objects, define how new-
  format objects are indexed and garbage-collected, and provide a rollback
  path that does not reinterpret one format as another.
- **FR-036**: Existing NDNSF authorization, permissions, tokens, replay
  protection, provider permission, and NAC-ABE routing MUST remain enforced on
  repository control operations.

#### Observability and verification

- **FR-037**: Each transfer MUST expose a stable operation identity and
  machine-readable phase timings for discovery/ACK collection, planning,
  queue wait, transfer-session start, transfer, verification, persistence,
  replication, commit, and activation.
- **FR-038**: Metrics MUST distinguish logical payload bytes, wire bytes,
  retransmitted bytes, storage bytes read and written, metadata operations,
  asymmetric verifications, digest verifications, control operations, and
  achieved replica count.
- **FR-039**: Performance claims MUST compare repository goodput against a
  matched network ceiling and a matched raw NDN segmented-transfer ceiling.
- **FR-040**: The canonical experiment MUST compare legacy per-packet
  asymmetric verification, digest-only transfer, and signed-manifest transfer
  using identical artifact bytes, topology, packet geometry, concurrency,
  logging, and measurement windows.
- **FR-041**: The preflight matrix MUST cover at least 1 MiB, 64 MiB, 1 GiB,
  and 16 GiB artifacts; one and three replicas; and concurrency levels 1, 4,
  and 16, unless a documented resource gate makes a cell inadmissible.
- **FR-042**: Every admissible matrix cell MUST include a warmup followed by at
  least five measured repetitions and retain individual samples, failures,
  environment identity, exact commands, and derivation rules.
- **FR-043**: NDNSF network, security, recovery, and performance acceptance MUST
  run in MiniNDN by default. TigerCluster and large models MUST be used only
  after the local gate passes or when explicitly required.
- **FR-044**: Negative, slower, failed, or resource-inadmissible results MUST be
  retained and reported; acceptance thresholds MUST NOT be tuned after viewing
  the formal results.

### Key Entities

- **Artifact Reference**: Stable public identity containing the logical name,
  immutable content identity, format version, size, and trust context.
- **Root Manifest**: Publisher-authenticated root binding the complete
  artifact, manifest hierarchy, naming rules, algorithms, and policy context.
- **Manifest Page**: Bounded content-addressed node that authenticates child
  pages or chunks without requiring the root to list every Data packet.
- **Artifact Chunk**: Independently transferable and resumable range whose
  digest is transitively bound to the root manifest.
- **Transfer Session**: Idempotent publication or retrieval lifecycle with
  lease, progress, cancellation, timeout, and resume state.
- **Verified Progress Map**: Compact record of ranges or chunks already
  validated for one exact artifact identity.
- **Replica Receipt**: Repository-authenticated evidence that a named replica
  committed a specific artifact identity under a stated policy and epoch.
- **Payload Store**: Persistence authority for immutable bytes, partial ranges,
  atomic finalization, and bounded reads.
- **Metadata Store**: Persistence authority for manifests, lifecycle state,
  sessions, receipts, policy metadata, and garbage-collection ownership.
- **Capability Advertisement**: Versioned declaration of supported artifact
  formats, algorithms, limits, resume behavior, and durability features.

## Scope Boundaries

### In Scope

- Generic immutable large-artifact publication, replication, retrieval, resume,
  cancellation, deduplication, verification, and garbage-collection semantics.
- Scalable manifest hierarchy, publisher provenance, algorithm agility, and
  denial-of-service limits.
- Separation of payload and metadata persistence contracts.
- Simple synchronous, asynchronous, and advanced session API behavior.
- Exact-packet compatibility, format negotiation, migration, and rollback.
- MiniNDN security, failure-recovery, and matched-throughput validation.

### Out of Scope

- Model partitioning, ONNX dependency-graph planning, provider role assignment,
  GPU/CPU/RAM placement, model loading, KV-cache transfer, and inference
  scheduling.
- Qwen-specific names, formats, adapters, or execution behavior.
- Modification of the base NDNSF Request/ACK/Selection/Response security
  semantics.
- Mutable database records, POSIX-compatible remote filesystems, arbitrary
  in-place writes, or general transactional object mutation.
- Confidentiality of artifact payload beyond existing policy and encryption
  facilities; this feature's new cryptographic contract concerns provenance
  and integrity.
- Choosing a particular database engine before matched evidence identifies the
  persistence bottleneck.

## Assumptions

- Artifacts are immutable once their content identity is committed. A changed
  object is a new version with a new content identity.
- The existing NDNSF collaboration API remains the normative control-plane
  carrier, and NDN segmented delivery remains the bulk data plane.
- A validated signed root manifest plus transitively bound collision-resistant
  digests provides public verifiability without a shared HMAC key.
- Asymmetric signing and verification of a bounded root trust structure is not
  expected to dominate bulk-transfer cost; this assumption must still be
  measured.
- The initial implementation may retain an embedded metadata database when it
  meets measured concurrency and recovery requirements; database replacement
  is an evidence-driven planning decision, not a requirement.
- Spec 162 may consume this feature for model-shard publication only after
  Spec 164 passes its generic MiniNDN acceptance gate. Existing Spec 162
  measurements remain the legacy baseline.
- Formal TigerCluster execution is deferred until the generic repository path
  passes local correctness, security, recovery, and throughput gates.

## Success Criteria

### Measurable Outcomes

- **SC-001**: For every successful publication and retrieval test, reconstructed
  bytes match the declared full-artifact digest, and all injected content,
  manifest, name, truncation, extension, and mixed-version corruptions are
  rejected before activation.
- **SC-002**: On the frozen matched topology, digest-only repository goodput is
  at least 85% of raw NDN segmented-transfer goodput for artifact sizes of
  64 MiB and above.
- **SC-003**: For large-artifact cells of 64 MiB and above, signed-manifest
  goodput is no more than 10% below digest-only goodput under the same
  workload, with the difference reported using all measured repetitions
  rather than a best run. The 1 MiB preflight/small-object cells remain
  mandatory diagnostics but do not define this large-artifact throughput gate.

  **Post-third-campaign scope amendment (2026-07-30)**: The original wording
  omitted the lower size bound even though this feature targets large
  artifacts and SC-002 already uses 64 MiB. The first three frozen campaigns
  and their original verdicts remain immutable. This clarification is
  prospective: it MUST be implemented and test-locked before a fourth
  campaign is frozen, and only that later campaign may provide confirmatory
  evidence under the clarified criterion.
- **SC-004**: One artifact publication performs a number of control operations
  bounded by replica count and lifecycle phases, with no per-chunk or per-Data-
  packet NDNSF service invocation.
- **SC-005**: Peak transfer memory remains bounded by configured in-flight work
  and does not grow in proportion to artifact size.
- **SC-006**: Routine metadata record count is bounded by objects, replicas,
  sessions, chunks, and manifest pages and contains no durable record for every
  4 KiB Data packet in the scalable format.
- **SC-007**: For cold single-replica transfer, measured payload-store read
  amplification is at most 1.20 times logical artifact bytes and write
  amplification is at most 1.50 times logical artifact bytes, using explicitly
  documented byte-accounting boundaries.
- **SC-008**: Every supported interruption point either resumes without
  retransmitting verified chunks or safely restarts without exposing partial
  content; 100% of recovery tests finish in a valid non-active or valid active
  state.
- **SC-009**: A developer can publish and retrieve an artifact with one public
  operation per direction, and can perform progress reporting, cancellation,
  resume, and replication without accessing private runtime attributes.
- **SC-010**: Legacy exact-packet objects remain readable with unchanged trust
  semantics throughout upgrade and rollback tests, and unsupported new-format
  requests fail explicitly without silent downgrade.
- **SC-011**: Every admissible formal matrix cell completes one warmup and at
  least five measured repetitions, with complete sample-level evidence for
  goodput, tail latency, CPU, memory, storage I/O, retransmissions, control
  operations, and failures.
- **SC-012**: MiniNDN correctness, security, recovery, and performance gates
  pass before any TigerCluster or large-model acceptance claim is made.
