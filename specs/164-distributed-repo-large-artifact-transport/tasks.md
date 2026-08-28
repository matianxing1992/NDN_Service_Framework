# Tasks: DistributedRepo Large-Artifact Transport

**Input**: [spec.md](spec.md), [plan.md](plan.md),
[research.md](research.md), [data-model.md](data-model.md), contracts, and
[quickstart.md](quickstart.md)

**Task rule**: Each task closes one independently reviewable behavior with its
failing test, implementation, focused validation, and evidence update kept
together. Task count is not a quality metric.

## Phase 1: Setup and Frozen Baseline

**Purpose**: Preserve the current subject and create deterministic inputs
before changing repository behavior.

- [x] T001 Capture the exact current `put_file → put → store_object`, control-call, packet-signing, fetching, and persistence behavior as the immutable `exact-packet-v1` compatibility baseline; add deterministic artifact fixtures and machine-readable subject identity without tuning the current path in `NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`, `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `tests/python/`, and `specs/164-distributed-repo-large-artifact-transport/evidence/legacy-subject.json`

**Acceptance gate**: The baseline can be invoked reproducibly, reports its
source/build identity and operation counts, and retains current negative
behavior rather than silently improving it.

---

## Phase 2: Foundational Security and Ownership

**Purpose**: Establish shared identities, limits, and one persistence authority
before any user-story data path.

- [x] T002 [P] Implement and test canonical `ArtifactReference`, capability, algorithm, RootManifest, ManifestPage, ArtifactChunk, UploadLease, and ReplicaReceipt types with bounded decoding and stable error categories in `NDNSF-DistributedRepo/include/ndnsf-distributed-repo/`, `NDNSF-DistributedRepo/src/`, `NDNSF-DistributedRepo/pythonWrapper/src/`, and `tests/unit-tests/`
- [x] T003 [P] Implement and test the authoritative `PayloadStore`/`MetadataStore` facade, lifecycle journal, and backend ownership rule so deployed Python orchestration and native code cannot commit independent state in `NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`, `NDNSF-DistributedRepo/src/`, `NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`, and `tests/python/`
- [x] T004 Define and test operation identity, phase timing, byte-accounting, cryptographic-operation, control-count, metadata-count, and replica-receipt metrics used by every later gate in `NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`, `NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`, `tests/unit-tests/`, and `tests/python/`

**Acceptance gate**: Canonical identity and limits round-trip across native and
Python boundaries, malformed/unbounded input is rejected before allocation,
one backend facade owns lifecycle state, and metrics have stable semantics.

---

## Phase 3: Trusted Near-Line-Rate Artifact Transfer (User Story 1, P1)

**Goal**: Publish and retrieve one large immutable artifact through a signed
bounded root and digest-authenticated segmented data plane.

**Independent Test**: On one publisher, one repository, and one consumer,
publish a deterministic artifact, retrieve it, prove publisher trust and exact
content identity, inject corruption/substitution, and compare digest-only and
signed-manifest goodput with a matched raw segmented NDN transfer.

- [x] T005 [P] [US1] Implement the `artifact-manifest-v2` trust composition—canonical signed root, hierarchical content-addressed pages, derived names, page/chunk/full digests, algorithm capability negotiation, policy epoch, revocation evaluation, and all parser/substitution/downgrade negative cases—in `NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`, `NDNSF-DistributedRepo/src/ArtifactManifest.cpp`, `NDNSF-DistributedRepo/pythonWrapper/src/`, `tests/unit-tests/`, and `tests/python/`
- [x] T006 [P] [US1] Implement streaming content-addressed payload files and transactional artifact metadata with range writes/reads, verified progress, bounded memory, atomic finalization intent, and no durable row per small Data packet in `NDNSF-DistributedRepo/src/backends/`, `NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`, `tests/unit-tests/`, and `tests/python/`
- [x] T007 [US1] Implement one collaboration-based replica/lease control flow plus a bounded adaptive segmented NDN producer/fetcher with out-of-order delivery, retransmission, duplicate suppression, backpressure, and control-count independence in `NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`, `NDNSF-DistributedRepo/src/ArtifactTransfer.cpp`, `NDNSF-DistributedRepo/pythonWrapper/`, `pythonWrapper/src/ndnsf/_ndnsf.cpp`, and focused native/Python tests
- [x] T008 [US1] Close the single-replica end-to-end path from publication through full verification, atomic commit, authenticated receipt, activation, retrieval, and atomic consumer destination; add deterministic local and MiniNDN success/corruption evidence in `NDNSF-DistributedRepo/`, `Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py`, `tests/python/`, and `specs/164-distributed-repo-large-artifact-transport/evidence/us1/`

**Acceptance gate**: SC-001, SC-004, SC-005, and SC-006 hold for the single-
replica smoke; no public shared HMAC is required; every injected corruption is
rejected before activation.

---

## Phase 4: Resume and Crash-Safe Recovery (User Story 2, P2)

**Goal**: Resume verified work after interruption while partial content remains
invisible and garbage collection remains race-safe.

**Independent Test**: Stop publisher, repository, and consumer at each declared
lifecycle boundary; restart; verify exact final state, transferred ranges,
digest, visibility, receipts, and GC ownership.

- [x] T009 [P] [US2] Implement idempotent publication/retrieval sessions, lease renewal/expiry, monotonic verified progress, cancellation, and exact-identity resume so only missing chunks plus bounded recovery traffic are transferred in `NDNSF-DistributedRepo/include/ndnsf-distributed-repo/`, `NDNSF-DistributedRepo/src/`, `NDNSF-DistributedRepo/pythonWrapper/`, and `tests/python/`
- [x] T010 [P] [US2] Implement and failure-inject the finalization journal, payload rename, metadata commit, receipt, activation, startup reconciliation, temporary ownership, capacity accounting, and GC protocol across every crash point in `NDNSF-DistributedRepo/src/backends/`, `NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`, `tests/unit-tests/`, and `tests/python/`
- [x] T011 [US2] Validate publisher/repository/consumer interruption, lease expiry, low-space failure, changed-identity resume, concurrent same/different digest sessions, and three-replica partial commit in MiniNDN; retain per-case transferred-byte and final-state evidence in `Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py`, `tests/python/`, and `specs/164-distributed-repo-large-artifact-transport/evidence/us2/`

**Acceptance gate**: SC-008 holds at every supported interruption point, no
partial object is discoverable, no verified range is unnecessarily
retransmitted beyond declared recovery traffic, and achieved durability equals
retained distinct receipts.

---

## Phase 5: Simple and Advanced Public API (User Story 3, P3)

**Goal**: Applications publish and fetch without private fields while advanced
users retain explicit session control.

**Independent Test**: Two minimal applications use only public sync APIs, then
repeat with async progress, cancellation, resume, deduplication, and advanced
sessions.

- [x] T012 [US3] Deliver and contract-test the synchronous, asynchronous, and advanced-session artifact APIs, ArtifactReference/result/error bindings, progress monotonicity, cancellation, idempotency, replica results, and public control selection in `NDNSF-DistributedRepo/pythonWrapper/src/`, `NDNSF-DistributedRepo/pythonWrapper/py_repoclient/`, `NDNSF-DistributedRepo/pythonWrapper/pyproject.toml`, and `tests/python/`
- [x] T013 [US3] Replace private-field and manual chunk/control usage in maintained examples with minimal public publish/fetch applications, document all arguments/results/errors in synchronized `NDNSF-DistributedRepo/README.md` and `NDNSF-DistributedRepo/README_ch.md`, and prove the examples in local and MiniNDN smoke evidence under `specs/164-distributed-repo-large-artifact-transport/evidence/us3/`

**Acceptance gate**: SC-009 holds; no example or test accesses
`_client.control_mode`, packet batches, or replica-internal calls.

---

## Phase 6: Compatibility, Migration, and Rollback (User Story 4, P4)

**Goal**: Legacy exact-packet objects and new artifacts coexist without silent
downgrade, reinterpretation, or destructive automatic migration.

**Independent Test**: Create legacy and v2 objects before/after upgrade, use
capable/incapable/mixed replicas and consumers, then roll back new publication
and verify both formats' declared behavior.

- [x] T014 [P] [US4] Preserve and test `exact-packet-v1` behavior behind an explicit format/backend while adding capability negotiation and hard failure for unsupported v2 formats, algorithms, limits, or durability in `NDNSF-DistributedRepo/include/`, `NDNSF-DistributedRepo/src/`, `NDNSF-DistributedRepo/pythonWrapper/`, and legacy/new contract tests
- [x] T015 [US4] Implement and test schema-generation startup, catalog/GC format identity, mixed-version replica selection, non-destructive roll-forward, rollback that disables new writes without reinterpreting bytes, and operator-visible migration diagnostics in `NDNSF-DistributedRepo/src/backends/`, `NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`, `tests/python/`, and `specs/164-distributed-repo-large-artifact-transport/evidence/us4/`

**Acceptance gate**: SC-010 holds; legacy trust semantics remain unchanged,
incapable peers fail explicitly, and rollback preserves committed objects.

---

## Phase 7: Academically Defensible Performance Evidence (User Story 5, P5)

**Goal**: Attribute cost to network, raw NDN, repository control, trust,
transfer, and persistence using a frozen matched campaign.

**Independent Test**: Reproduce the declared matrix and derive every reported
ratio/distribution from retained sample-level evidence without post-result
tuning or omitted failures.

- [x] T016 [P] [US5] Implement the matched physical/network and raw segmented NDN ceilings plus repository phase/resource instrumentation, stable timeline sampling, immutable campaign manifest, and per-run schema in `Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py`, `Experiments/`, `NDNSF-DistributedRepo/`, and `specs/164-distributed-repo-large-artifact-transport/evidence/schemas/`
- [x] T017 [US5] Run and preserve the MiniNDN preflight, freeze admissible cells before formal outcomes, then execute one warmup and at least five measured repetitions for every admissible size/replica/concurrency/verification cell without tuning subjects or thresholds; write the unique canonical campaign under `results/` and register it in `specs/164-distributed-repo-large-artifact-transport/evidence/campaign-pointer.md`
- [x] T018 [US5] Derive paired repository/raw-NDN and signed-manifest/digest-only ratios, distributions, intervals, scaling, amplification, failures, and threshold verdicts from all retained samples; independently verify formulas and publish limitations and reproducibility commands in `Experiments/analyze_distributed_repo_artifact.py` and `specs/164-distributed-repo-large-artifact-transport/evidence/performance-report.md`

**Acceptance gate**: SC-002, SC-003, SC-007, SC-011, and SC-012 receive
evidence-backed PASS/FAIL/INCONCLUSIVE verdicts; every negative or inadmissible
cell remains visible.

---

## Phase 8: Cross-Cutting Audit and Documentation

**Purpose**: Close security, architecture, compatibility, and operator
documentation only after all implemented stories have evidence.

- [x] T019 [P] Run the Spec 164 code-aware audit against current CodeGraph source, threat model, trust composition, parser/resource bounds, replay/downgrade/deduplication/GC cases, migration/rollback, and all available evidence; fix only blocking findings within this feature and record residual risk in `specs/164-distributed-repo-large-artifact-transport/audit.md`
- [x] T020 [P] Synchronize final public architecture, API, security, recovery, compatibility, MiniNDN reproduction, and TigerCluster boundary guidance in `NDNSF-DistributedRepo/README.md`, `NDNSF-DistributedRepo/README_ch.md`, `docs/NDNSFDI/NDNSFDI-Design-ch.tex`, and relevant slides without presenting planned or failed behavior as deployed

**Acceptance gate**: Audit verdict permits the achieved implementation scope,
English/Chinese docs agree, and every deployment/performance claim links to
current evidence.

---

## Phase 9: Audit-Blocking Performance Remediation

**Purpose**: Resolve the negative and inconclusive findings retained by T019.
This phase creates a new frozen campaign; it never changes or overwrites the
first campaign.

- [x] T021 Replace per-consumer `openssl` process creation in the signed-manifest performance subject with an in-process ndn-cxx detached SHA-256 verifier, add corruption coverage, rebuild the Python extension, and retain a before/after phase-cost diagnosis in `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `pythonWrapper/ndnsf/`, `Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py`, `tests/python/`, and `specs/164-distributed-repo-large-artifact-transport/evidence/remediation/`
- [x] T022 Implement and contract-test a deployed `ArtifactApiBackend` adapter that drives advisory ACK offer collection and queued replica-task assignment through `ReplicaTaskCollaborationClient` and the public NDNSF `begin_collaboration → ACK_CLOSED → commit_plan` path, without ACK-time reservation or resource locking, while keeping segmented Data transfer in NDNSF-DistributedRepo; use it in the MiniNDN public publish/fetch experiment and measure bounded control operations in `NDNSF-DistributedRepo/pythonWrapper/py_repoclient/`, `Experiments/NDNSF_DistributedRepo_PublicApi_Minindn.py`, and `tests/python/`
- [x] T023 Replace the active performance subject's legacy ACK-reservation phase with advisory `ackCollection`/`planning`, bounded `queueWait`, and execution-time `sessionStart`; update the evidence schemas while retaining legacy result readability; add matched cold retrieval and separate payload-store reads/writes, metadata-store reads/writes, Data wire bytes, and Interest wire bytes according to the canonical contract; test lifecycle semantics, byte accounting, and cold-destination visibility in `Experiments/`, `NDNSF-DistributedRepo/`, `tests/python/`, and `specs/164-distributed-repo-large-artifact-transport/evidence/schemas/`
- [x] T024 Freeze and run a new MiniNDN remediation campaign with one retained warmup and at least five measured repetitions per admitted cell, preserve the original campaign, derive determinate SC-002/SC-003/SC-004/SC-007 verdicts, and publish a new immutable pointer and report under `results/` and `specs/164-distributed-repo-large-artifact-transport/evidence/remediation/`
- [x] T025 Re-run the code-aware security, lifecycle, compatibility, migration, performance, and evidence audit; synchronize changed results into both repository guides and both NDNSF-DI PDF sources, rebuild/audit the PDFs, and explicitly permit or block the authorized TigerCluster campaign in `specs/164-distributed-repo-large-artifact-transport/audit.md`

**Acceptance gate**: SC-002, SC-003, SC-004, and SC-007 are determinate and
pass on the replacement frozen MiniNDN evidence; T019's BLOCK findings are
closed without rewriting the original campaign; only then may TigerCluster be
used for external-validity evidence.

---

## Phase 10: High-Concurrency Stability Closure

**Purpose**: Close the measured SC-003 failure without altering either frozen
campaign, then re-open the TigerCluster acceptance gate.

- [x] T026 Diagnose the retained r1/c4, r1/c16, and r3/c4 failures by run ID and subject; distinguish producer, NFD/forwarding, consumer window/retry, process scheduling, persistence, and verification causes; publish a source-backed root-cause report and an FR/task/evidence traceability matrix without changing frozen evidence
- [x] T027 Implement the smallest justified concurrency/backpressure correction, add a failure-focused MiniNDN regression that reproduces the old failure and passes with the fix, and preserve every timeout/retry/resource bound
- [x] T028 Freeze and execute a third matched MiniNDN campaign with the same thresholds, one retained warmup and five measured repetitions per admitted cell; retain its three negative outcomes and SC-003 failure without replacement or reinterpretation
- [x] T029 Audit the third campaign's first-Interest failures and SC-003 statistical domain; publish the 11-fallacy scan and prospectively clarify the large-artifact gate without applying it retroactively

**Acceptance gate**: SC-002, SC-003, SC-004, SC-007, SC-011, and SC-012 are
all determinate `PASS`; the queued no-reservation control path remains
unchanged; both predecessor campaigns remain immutable; the audit explicitly
permits TigerCluster acceptance work.

---

## Phase 11: Confirmatory Large-Artifact Closure

**Purpose**: Correct the bounded first-Interest retry gap, predeclare the
large-artifact population for SC-003, and obtain new confirmatory evidence
without selecting or rewriting any prior outcome.

- [x] T030 Put the initial adaptive segmented Interest under the existing bounded retry budget with exact retransmission accounting; test the retry semantics and lock the SC-003 ≥64 MiB eligibility rule while continuing to report every 1 MiB diagnostic
- [x] T031 Freeze and execute a fourth matched MiniNDN campaign only after T029/T030 are complete; retain one warmup and at least five measured repetitions for every admitted cell, every failure, and all per-cell distributions; require the predeclared SC-003 completion, point-estimate, and bootstrap gates to pass
- [x] T032 Re-run the code-aware security/lifecycle/compatibility/performance audit, update the canonical campaign pointer, guides, slides, detailed PDF and PDF audit, and permit the authorized TigerCluster NDNSF-DI + Qwen campaign only if every Spec 164 gate is `PASS`
- [x] T033 Remove the legacy active capacity-reservation API and control round so ACK remains advisory and Selection directly creates a queued idempotent store assignment; retain the old table/state only as read-only migration input and record regression evidence in `evidence/remediation/t033-remove-capacity-reservation.md`

**Acceptance gate**: the third campaign remains immutable negative evidence;
the fourth campaign is independently verified; SC-002, SC-003, SC-004,
SC-007, SC-011, and SC-012 are all `PASS`; no small-object failure is hidden;
and the final audit explicitly permits TigerCluster work.

---

## Phase 12: Public Network Data-Plane Closure

**Purpose**: Correct the TigerCluster integration gap discovered after T032:
the public Artifact API control adapter must drive the real RepoNode segmented
network data plane for one whole artifact, rather than delegating to a test-only
or legacy per-chunk backend.

- [x] T034 Implement and contract-test a generic whole-artifact network backend: start a bounded-memory segmented producer, carry its immutable source descriptor in the queued Selection assignment, let each selected RepoNode fetch adaptively into its filesystem CAS, verify the signed root and full content digest, atomically commit and serve the artifact, and return a durable receipt through the existing `begin_collaboration → ACK_CLOSED → commit_plan` path without reservation or locks
- [x] T035 Integrate the public network backend into the MiniNDN public API experiment and NDNSF-DI Qwen artifact registration/fetch path; make cancellation/failure cleanup remove bootstrap tokens and selection keys while retaining terminal evidence; rerun the Spec 164 suites and a new immutable MiniNDN confirmation before rebuilding the TigerCluster runtime bundle

**Acceptance gate**: no production path uses the legacy per-chunk
`DistributedRepo.put_file()` loop for large artifacts; a public API call moves
and commits a real file through RepoNode over NDN; cancellation leaves no
private selection material; and new MiniNDN evidence passes before the
TigerCluster rerun.

## Dependencies

```text
T001
 └── T002, T003, T004
       ├── T005 ─┐
       ├── T006 ─┼── T007 → T008       (US1 MVP)
       │         │
       │         ├── T009, T010 → T011 (US2)
       │         ├── T012 → T013       (US3)
       │         └── T014 → T015       (US4)
       │
       └── T008, T011, T013, T015
                     └── T016 → T017 → T018 (US5)
                                      └── T019, T020
                                                └── T021
                                                     ├── T022
                                                     └── T023
                                                          └── T024 → T025
                                                                      └── T026 → T027 → T028 → T029
                                                                                          └── T030 → T031 → T032 → T033 → T034 → T035
```

- Phase 2 blocks all user stories.
- US1 is the MVP and blocks formal network campaigns.
- US2, US3, and US4 can proceed in parallel after their shared US1 primitives
  stabilize; each retains an independent acceptance gate.
- US5 requires the implemented subjects and frozen contracts from US1–US4.
- Cross-cutting audit and final docs use achieved evidence and therefore close
  last.
- T021--T025 are controlled remediation created by T019's blocking verdict.
  They preserve the first campaign as immutable negative evidence.
- T026--T029 are controlled closure created by T025's retained SC-003 failure.
  They preserve both earlier campaigns and do not weaken thresholds.

## Parallel Opportunities

- T002 and T003 can proceed independently; T004 can begin after their public
  identities and lifecycle phases are stable.
- T005 and T006 are parallel security/persistence subjects that converge at
  T007.
- After T008, T009/T010, T012, and T014 can be assigned independently.
- T019 and T020 can run in parallel after performance evidence, provided docs
  do not outrun the audit verdict.

## Mechanical Fragments Coalesced

The cohesion pass merged at least 20 mechanical fragments into their owning
behavioral tasks, including:

- manifest tests, codec implementation, negative cases, and evidence → T005;
- backend interfaces, streaming files, metadata transactions, and focused
  tests → T006;
- segmented fetcher, control integration, retransmission tests, and metrics →
  T007;
- recovery tests, journal implementation, startup reconciliation, and GC →
  T010;
- bindings, sync/async APIs, progress/cancel semantics, and API tests → T012;
- harness creation, run execution, and result retention remain separated only
  where the frozen-campaign dependency makes their outputs independently
  meaningful (T016–T018).

## Implementation Strategy

### MVP

Complete T001–T008. This delivers User Story 1: one trusted, scalable,
single-replica artifact publication/retrieval path and enough matched evidence
to decide whether the architecture preserves raw NDN capacity.

### Incremental Delivery

1. Add restart-safe resume and multi-replica recovery (US2).
2. Expose the simple/async/advanced public API (US3).
3. Qualify coexistence, migration, and rollback (US4).
4. Freeze and run the full MiniNDN performance campaign (US5).
5. Audit and update final public documentation.

TigerCluster and large Qwen artifacts remain outside implementation acceptance
until the MiniNDN gate closes.

## Format Validation

- All tasks use `- [ ] Tnnn` sequential checklist format.
- Story-phase tasks include `[USn]`.
- `[P]` appears only where tasks can proceed against different primary files
  or risks without an incomplete task dependency.
- Every task names concrete source, test, experiment, documentation, or
  evidence paths.
- Tests and evidence are included inside the behavioral task they validate
  rather than split into mechanical follow-up tasks.
