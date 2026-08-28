# Tasks: SVS PubSub Payload Interoperability

**Input**: Design documents from `specs/117-svs-pubsub-payload-interop/`

**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`,
`contracts/payload-receipts.md`, `quickstart.md`

**Tests**: Required. Test-first checks, implementation, focused execution, and
evidence for one behavior remain together in the same task.

**Organization**: Three cohesive tasks cover the corpus/receipt authority, the
standalone bilateral gate, and conditionally admitted MiniNDN evidence. A
larger task count would mechanically split the same acceptance outcomes.

## Phase 1: Shared Corpus and Acceptance Authority

**Purpose**: Establish one independent byte-level oracle before either peer is
extended.

- [x] T001 Build the deterministic four-case payload corpus, manifest validator, receipt classifier, and test-first positive/negative contract cases in `examples/interop/ndn-svs-v3/payload_corpus.py` and `tests/python/test_spec117_svs_pubsub_interop.py`; prove stable names, lengths, SHA-256 values, embedded zero/non-UTF-8 bytes, mandatory segmented size, duplicate/missing/mismatch rejection, and unchanged existing StateVector interop artifacts (FR-003-FR-005, FR-009-FR-010; SC-001-SC-002, SC-004)

**Checkpoint**: Expected bytes and acceptance logic exist independently of both
runtime implementations.

---

## Phase 2: User Story 1 - Bilateral Standalone Payload Gate (Priority: P1) MVP

**Goal**: Execute both public PubSub implementations and identify whether they
retrieve and decode each other's application payloads.

**Independent Test**: One bounded two-peer run produces eight exact payload
receipts or a classified direction/case/stage incompatibility.

- [x] T002 [US1] Extend the NDNSF-owned C++ and real TypeScript peers with isolated payload mode using public `SVSPubSub` and `SvPublisher`/`SvSubscriber` APIs, add the pinned direct NDNts DataStore dependency and a bounded standalone orchestrator, then compile/type-check and execute the four-case corpus in both directions; write peer JSONL, packet/log evidence, and `summary.json` under a unique result directory, accepting only exact name/length/SHA-256 and multi-segment receipts and preserving any native Mapping/fetch/decapsulation incompatibility without changing `/home/tianxing/NDN/ndn-svs` or adding an adapter in `examples/interop/ndn-svs-v3/cpp/svs3-peer.cpp`, `examples/interop/ndn-svs-v3/ndnts/svs3-peer.ts`, `examples/interop/ndn-svs-v3/ndnts/package.json`, `examples/interop/ndn-svs-v3/run-payload-standalone.py`, and `examples/interop/ndn-svs-v3/README.md` (FR-001-FR-007; SC-001-SC-002, SC-004-SC-005)

**Checkpoint**: Application-data compatibility is either measured-compatible
or measured-negative at an exact native protocol boundary. Only the former
admits Phase 3 network execution.

---

## Phase 3: User Story 2 - MiniNDN Network Gate (Priority: P2)

**Goal**: Prove the accepted standalone path over separate NFD hosts and packet
loss, or mechanically preserve why the network gate was not admitted.

**Independent Test**: Fresh 0% and 5% cells contain complete bilateral exact
receipts, registration evidence, and packet captures; a failed standalone gate
instead produces a `NOT_ADMITTED` receipt and starts no MiniNDN process.

- [x] T003 [US2] Implement the conditional MiniNDN launcher and focused producer-registration/stop-gate tests, bind immutable peer and corpus identities, and—only after T002 is measured-compatible—run one 0% and one 5% cell with separate hosts, normal local producer registration, only the explicit inter-host sync-group topology route, bounded deadlines, and packet captures; otherwise prove MiniNDN was not launched, preserve the standalone blocker, run strict post-implementation structure/code/evidence audit, and record the exact achieved claim level and next owner in `Experiments/NDN_SVS_PubSub_Interop_Minindn.py`, `tests/python/test_spec117_svs_pubsub_interop.py`, and `specs/117-svs-pubsub-payload-interop/completion-summary.md` (FR-006-FR-010; SC-003-SC-005)

## Dependencies & Execution Order

```text
T001 -> T002 -> T003
```

- T001 is the independent oracle used by both peers and both runners.
- T002 must finish before T003 can decide whether MiniNDN is admissible.
- A measured-negative T002 result is not retried or translated; T003 closes
  the stop receipt and audit without starting the matrix.

## Parallel Opportunities

None. All three tasks share the corpus identity and acceptance authority, and
T002's verdict controls T003. Parallel execution would weaken evidence
ownership rather than shorten a meaningful critical path.

## Implementation Strategy

1. Build and test the byte-level oracle.
2. Run the cheapest real bilateral gate.
3. Spend MiniNDN time only when native standalone interoperability passes.
4. Preserve a negative result as the input to a separately owned protocol
   repair; do not patch a test-only compatibility layer.

## Task-Cohesion Review

- Mechanically fragmented groups detected: 0.
- Coalescing opportunities remaining: 0.
- Each task combines its tests, implementation, execution, and evidence because
  those steps close one behavioral acceptance gate.
