# Research: NDN-SVS V3 Wire Compatibility and Interoperability

## Decision 1: Treat the 2025-01-14 V3 page as normative

**Decision**: Implement and test against the published
[State Vector Sync V3 specification](https://named-data.github.io/StateVectorSync/Specification.html).

**Rationale**: It explicitly defines `/v=3`, signed State Vector Data in
ApplicationParameters, TLV 201/202/210/212/214, one-second lifetime, 200 ms
suppression, bootstrap boundaries, and state-machine behavior.

**Alternatives considered**:

- Current official C++ master: rejected as the normative V3 source because its
  core still emits `/v=2`, raw StateVector parameters, 1 ms lifetime, and the
  older suppression default.
- Copying any one external implementation: rejected because observable defaults
  and extension support differ even among independently tested implementations.

## Decision 2: Use pinned NDNts as the first independent peer

**Decision**: Pin an exact `@ndn/svs` package/source identity and enable
`svs3: true`; explicitly set all V3 timers in the fixture.

**Rationale**: NDNts separates V2 and V3 and its V3 source constructs `/v=3`,
embeds signed Data, checks the embedded name, validates that Data, and decodes
its Content. Its official `sync-interop` guide demonstrates V3 against NDNd and
uses `openUplinks()` to attach to NFD.

**Alternatives considered**:

- NDNd as the only peer: viable but adds a Go build/runtime to the MVP. Retain
  it as a later second oracle if NDNts and the fixed vectors disagree.
- Current C++ master: suitable only for V2 regression, not independent V3.
- A second copy of the new C++ codec: rejected because it cannot detect shared
  encoder/decoder mistakes.

## Decision 3: Preserve V2 explicitly and make V3 the Experimental default

**Decision**: One selected `SvsProtocolVersion` controls the whole protocol
profile. V3 is the default after Spec 114; V2 requires an explicit selection.

**Rationale**: Existing fork users need a rollback path, but automatic fallback
or hybrid acceptance makes trust and state ownership ambiguous. Versioned names
already provide clean route isolation.

**Alternatives considered**:

- Delete V2 immediately: rejected because upstream C++ peers still use it.
- Keep V2 default indefinitely: rejected because it would leave the feature's
  primary standards outcome opt-in and allow the current hybrid to survive.
- Auto-detect/dual-publish: rejected for state duplication, downgrade, and
  observability risks.

## Decision 4: Share merge logic, not wire framing

**Decision**: Separate V2 and V3 envelope codecs, then feed both into the same
validated state-vector comparison/merge/state-machine implementation.

**Rationale**: Names, signed objects, timers, and validation targets differ by
version; vector comparison semantics are reusable. This prevents the current
partial migration while avoiding two authoritative state machines.

**Alternatives considered**:

- Conditional statements scattered through `core.cpp`: rejected as likely to
  recreate hybrid combinations and serial/parallel drift.
- Two complete Core classes: rejected because comparison, suppression, workers,
  statistics, and lifecycle would diverge.

## Decision 5: Validate the embedded Data before worker decoding

**Decision**: Parse the V3 envelope structurally, validate the embedded Data on
the Face event loop, then pass only validated Content to serial or parallel
decode/merge.

**Rationale**: The State Vector Data is the V3 signed object. Decoding or merging
before its asynchronous validation would allow untrusted state into the worker
and complicate rollback.

**Alternatives considered**:

- Continue validating the outer Interest: rejected as the wrong signed object.
- Validate inside generic workers: rejected because validators and Face-related
  callbacks have event-loop/lifetime ownership.

## Decision 6: Make invalid-vector rejection atomic

**Decision**: Normalize structural and semantic decode failures into caught,
observable outcomes before any state change. Reject sequence zero; reject the
whole vector when any bootstrap exceeds `now + 86400s`.

**Rationale**: Current future-time detection throws `VersionVector::Error`, but
Core catches only `ndn::tlv::Error`. The specification requires ignoring the
entire vector, not partial merge or process loss.

**Alternatives considered**:

- Clamp future times or skip one tuple: rejected because that changes the
  authoritative vector and contradicts the whole-vector rule.
- Catch `std::exception` around the entire callback: insufficient alone because
  it hides ownership and could leave partial work.

## Decision 7: Keep bootstrap persistence application-owned

**Decision**: Add an optional initial bootstrap time and reuse the existing
getter; do not add protocol-library file storage.

**Rationale**: Applications have different persistence and identity lifetimes.
Injection satisfies the specification's reuse recommendation without a second
source of truth.

**Alternatives considered**:

- Hidden file under `$HOME`: rejected for containers, multi-instance collision,
  permissions, and unclear cleanup.
- Always current time: valid only after state loss and does not attempt reuse.

## Decision 8: Define Mapping/Repair as a fork extension profile

**Decision**: Standard Data comes first; private blocks trail it and are owned by
SVSPubSub. Unknown blocks are ignorable. Whole-envelope LZMA is unavailable in
V3 without a future negotiated profile.

**Rationale**: The linked core V3 page does not define RepairData or LZMA. The
[SVS-PS page](https://named-data.github.io/StateVectorSync/PubSubSpec.html)
predates the V3 core update and does not define the fork's repair protocol.

**Alternatives considered**:

- Put extensions in State Vector Data Content: rejected because it changes the
  normative content grammar.
- Call private TLVs core V3: rejected as an unverifiable standards claim.
- Delete the extensions: rejected because Spec 113 evidence shows they provide
  required segmented-publication recovery behavior.

## Decision 9: Candidate evidence is run-once and cross-repository

**Decision**: Require fixed vectors, full NDN-SVS tests, standalone bidirectional
interop, six MiniNDN cells, rebuilt NDNSF focused regressions, audit, and
convergence against one source manifest.

**Rationale**: Existing tests pass while constructing `/v=2` themselves. Spec
113 also demonstrated that stale installed headers can invalidate a candidate.

**Alternatives considered**:

- Homogeneous C++ unit tests only: rejected as unable to prove interoperability.
- Rerun failed cells until they pass: rejected because it destroys evidence
  integrity.
- Build only the target library without NDNSF: rejected because installed ABI
  and consumer behavior are part of the real deployment path.

## Source And Code Reality Snapshot

- Normative core spec: `https://named-data.github.io/StateVectorSync/Specification.html`
- Independent source: `https://github.com/yoursunny/NDNts/blob/main/pkg/svs/src/sync.ts`
- Interop guide: `https://github.com/yoursunny/NDNts/tree/main/integ/sync-interop`
- Target baseline: `/home/tianxing/NDN/ndn-svs`,
  `Experimental@c34c04d766836bba1567a70bae846dfbd9d25b66`
- Current confirmed hybrid sender: `ndn-svs/core.cpp` uses `appendVersion(2)`,
  raw StateVector ApplicationParameters, and `1_ms`.
- Current V3 tuple encoder: `ndn-svs/version-vector.cpp` implements nested
  BootstrapTime/SeqNo entries.
- Current test blind spot: `tests/unit-tests/core.t.cpp::makeSyncInterest`
  constructs `/v=2` and raw StateVector parameters.
