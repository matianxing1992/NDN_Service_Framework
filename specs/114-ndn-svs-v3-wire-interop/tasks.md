# Tasks: NDN-SVS V3 Wire Compatibility and Interoperability

**Input**: Design documents from `specs/114-ndn-svs-v3-wire-interop/`

**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`,
`contracts/`, `quickstart.md`

**Tests**: Required. Each behavioral task follows a local red-green-evidence
cycle: add the smallest failing test, implement the behavior, continue from the
current test, and record the focused result. The full suite and network matrix
run only after their prerequisite gates pass.

**Organization**: The previous 73 mechanical checklist items are consolidated
into 29 independently closable tasks. A task may include its directly coupled
test, implementation, and focused evidence, but does not combine unrelated
protocol behaviors. All original acceptance gates remain.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Safe to execute in parallel because it changes independent files and
  does not depend on an incomplete task in the same phase.
- **[Story]**: Maps the task to a user story in `spec.md`.
- Every task names exact source, test, evidence, or documentation paths.

## Phase 1: Baseline and Independent Authorities

**Purpose**: Freeze the Spec 113 base, reproduce the hybrid wire packet, and
bind standards/peer identities before protocol edits.

- [x] T001 Record `/home/tianxing/NDN/ndn-svs` branch/OID/status, Spec 113 ancestor OIDs, compiler/dependency and installed-library identities, current serial/parallel `/v=2 + raw StateVector` packet captures, and a verified local rollback ref in `specs/114-ndn-svs-v3-wire-interop/evidence/baseline.md`, without pushing or moving master (FR-002, FR-003, FR-013, FR-021, FR-023)
- [x] T002 [P] Pin the normative SVS V3 page revision, official C++ V2 baseline, NDNts source/package and `sync-interop` identities, then create lockfile-exact peer inputs in `../ndn-svs/tests/interop/ndnts/package.json` and `package-lock.json` and record their digests in `specs/114-ndn-svs-v3-wire-interop/evidence/baseline.md` (FR-020, FR-021)
- [x] T003 Validate Context Mode, CodeGraph freshness, Spec Kit/GSD prerequisites, target/control worktree ownership, disk/sudo/MiniNDN/Node/NFD-cleanup availability, and the pre-implementation traceability map; record all outcomes and any non-blocking warning in `specs/114-ndn-svs-v3-wire-interop/evidence/tool-gates.md` and `traceability.md` (FR-021, FR-024)

**Checkpoint**: The incompatibility is captured, independent authorities are
pinned, and the pre-feature state is recoverable.

---

## Phase 2: Shared Test and Protocol Foundation

**Purpose**: Establish independent wire oracles, one protocol profile, and
candidate tooling used by every story.

**Gate**: No production send/receive behavior changes before T004-T006 expose
the current hybrid behavior as a failing test.

- [x] T004 [P] Create independently authored positive V2/V3 and malformed wire fixtures under `../ndn-svs/tests/fixtures/svs-v3/`, covering canonical TLVs, multi-epoch entries, wrong version/name, missing/invalid signature, raw V3 StateVector, malformed Content/NNI, SeqNo zero, future BootstrapTime, duplicate/unknown TLVs, and truncated extensions; document provenance without using the production encoder (FR-008, FR-009, FR-010, FR-017, FR-019, FR-020; SC-001, SC-002)
- [x] T005 Add red constructor/default/source-compatibility tests in `../ndn-svs/tests/unit-tests/core.t.cpp` and `svspubsub.t.cpp`, then introduce `SvsProtocolVersion` plus one validated `SyncProtocolOptions` in `../ndn-svs/ndn-svs/core.hpp` with V3 default, explicit V2, optional bootstrap time, and profile-owned timer defaults (FR-001, FR-006, FR-011, FR-014, FR-019)
- [x] T006 Add `../ndn-svs/tests/unit-tests/v3-wire.t.cpp` and its `../ndn-svs/tests/wscript` target with red V3 envelope tests; create the typed V2/V3 codec interface/error boundary in `../ndn-svs/ndn-svs/sync-protocol.cpp/.hpp`; and add immutable manifest/run-once red tests plus the inspection skeleton in `tests/python/test_spec114_candidate_manifest.py`, `tests/python/test_spec114_svs_v3_tools.py`, and `Experiments/spec114_candidate_manifest.py` (FR-002, FR-003, FR-004, FR-005, FR-013, FR-019, FR-021, FR-024)

**Checkpoint**: Independent fixtures fail against the hybrid implementation,
and shared abstractions exist without creating a second state machine.

---

## Phase 3: User Story 1 - Standard V3 Packet Exchange (Priority: P1) MVP

**Goal**: Produce and consume the normative V3 envelope in serial and parallel
paths.

**Independent Test**: Fixed V3 packets decode, production packets match their
normative fields, and one independent packet advances exactly one remote range.

- [x] T007 [US1] Add failing exact encode tests in `../ndn-svs/tests/unit-tests/v3-wire.t.cpp`, then implement `/v=3`, signed State Vector Data Content, canonical tuple ordering, ApplicationParameters placement, and ParametersSha256Digest construction in `../ndn-svs/ndn-svs/sync-protocol.cpp`; record fixed-vector digests in `specs/114-ndn-svs-v3-wire-interop/evidence/fixed-vectors.md` (FR-002, FR-003, FR-004, FR-008, FR-019; SC-001)
- [x] T008 [US1] Add independent receive and route-isolation tests in `../ndn-svs/tests/unit-tests/core.t.cpp`, then implement exact V3 embedded-Data extraction/name/content checks, trailing-block separation, selected versioned registration, and serial receive routing through the shared merge logic in `../ndn-svs/ndn-svs/sync-protocol.cpp` and `core.cpp/.hpp` (FR-003, FR-005, FR-013, FR-015)
- [x] T009 [US1] Add timer, immediate-publication, batching-diagnostic, suppression-state, and zero-Sync-Ack tests in `../ndn-svs/tests/unit-tests/core.t.cpp`, then implement V3 1000 ms Interest lifetime, 30 s ±10% periodic timing, the 200 ms exponential suppression bound, explicit batching overrides, and Interest-only reconciliation in `../ndn-svs/ndn-svs/core.cpp/.hpp` (FR-006, FR-007, FR-025; SC-010)
- [x] T010 [US1] Add serial/parallel byte-equivalence, signer-thread/fencing, and bootstrap-aware publication/fetch-name tests in `../ndn-svs/tests/unit-tests/core.t.cpp`, then route parallel production through the same codec and correct only required naming in `../ndn-svs/ndn-svs/core.cpp/.hpp`, `svsync-base.cpp`, `svsync.hpp`, and `svsync-shared.hpp`; record the focused MVP result in `specs/114-ndn-svs-v3-wire-interop/evidence/fixed-vectors.md` (FR-012, FR-013, FR-020; SC-001, SC-007, SC-010)

**Checkpoint**: Core V3 packets are byte-correct and locally consumable; this
is not yet a security-, migration-, or merge-ready candidate.

---

## Phase 4: User Story 2 - Atomic Invalid-Packet Rejection (Priority: P1)

**Goal**: Invalid input cannot mutate state, escape callbacks, kill workers, or
poison the next valid packet.

**Independent Test**: Every malformed/security fixture fails atomically in both
processing modes and a following valid vector succeeds.

- [x] T011 [P] [US2] Add table-driven malformed-envelope and signature-policy tests in `../ndn-svs/tests/unit-tests/v3-wire.t.cpp` and `security-options.t.cpp`, then implement V3 embedded-Data signing/validation and observable unverified structural mode using existing SecurityOptions ownership in `../ndn-svs/ndn-svs/security-options.cpp/.hpp` and `core.cpp` (FR-004, FR-005, FR-020; SC-002)
- [x] T012 [P] [US2] Add SeqNo 0, `now+86400`, `now+86401`, malformed NNI, multi-entry future tuple, canonical ordering, re-bootstrap, and next-valid tests in `../ndn-svs/tests/unit-tests/version-vector.t.cpp`, then enforce positive complete tuples and atomic future-vector rejection in `../ndn-svs/ndn-svs/version-vector.cpp/.hpp` (FR-008, FR-009, FR-010; SC-002, SC-007)
- [x] T013 [US2] Add serial/parallel invalid-vector, asynchronous-validator lifetime, worker-survival, and shutdown tests in `../ndn-svs/tests/unit-tests/core.t.cpp`, then ensure validation precedes decode/merge and all declared codec/vector failures are caught at both event-loop boundaries in `../ndn-svs/ndn-svs/core.cpp/.hpp` (FR-004, FR-010, FR-013; SC-002)
- [x] T014 [US2] Add bootstrap injection and structured rejection-diagnostic tests in `../ndn-svs/tests/unit-tests/core.t.cpp`, then validate supplied bootstrap time before registration, expose the active epoch, normalize safe reason counters, and run the complete negative matrix once; record unchanged state digests and next-valid progress in `specs/114-ndn-svs-v3-wire-interop/evidence/focused-validation.md` (FR-010, FR-011, FR-015; SC-002, SC-007)

**Checkpoint**: Authoritative state changes only after the correct signed object
and complete vector pass validation.

---

## Phase 5: User Story 3 - Explicit V2/V3 Migration (Priority: P1)

**Goal**: Experimental defaults to complete V3 while explicit V2 remains
isolated, source-compatible, observable, and reversible.

**Independent Test**: Fixed V2 bytes remain stable, V3 is the default, and
mixed profiles exchange zero state with a clear diagnostic.

- [x] T015 [P] [US3] Add V2 fixed-vector/outer-signing and public-constructor propagation tests in `../ndn-svs/tests/unit-tests/v3-wire.t.cpp`, `security-options.t.cpp`, `core.t.cpp`, and `svspubsub.t.cpp`, then implement the isolated V2 codec and pass one `SyncProtocolOptions` through `../ndn-svs/ndn-svs/svsync-base.cpp/.hpp`, `svsync.hpp`, `svsync-shared.hpp`, and `svspubsub.cpp/.hpp` (FR-001, FR-006, FR-014, FR-019; SC-005)
- [x] T016 [US3] Add mixed-route/no-dual-publish/no-downgrade tests, then switch only Experimental's default to V3, retain explicit V2, log resolved profiles, and expose protocol/timer/bootstrap/extension identity through `../ndn-svs/ndn-svs/core.cpp/.hpp` and `Experiments/spec114_candidate_manifest.py`; record V2 byte stability and mixed isolation in `specs/114-ndn-svs-v3-wire-interop/evidence/focused-validation.md` (FR-005, FR-014, FR-015, FR-021; SC-005)
- [x] T017 [US3] Add NDNSF and GUI configuration tests, then propagate `NDNSF_SVS_PROTOCOL_VERSION=v2|v3` through `ndn-service-framework/ServiceUser.cpp` and `ServiceProvider.cpp`, apply `NDNSF_SVS_MAX_SUPPRESSION_MS` only when explicitly set, and update `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/gui.py` so V3/200 ms is the unset default while explicit V2, explicit 1 ms, and invalid-version behavior remain observable (FR-001, FR-014, FR-015, FR-022, FR-026; SC-011)
- [x] T018 [US3] Update V2/V3 selection, source-versus-ABI compatibility, validation status, bootstrap ownership, profile-owned defaults, explicit overrides, and rollback examples in `../ndn-svs/README.md`, `../ndn-svs/examples/core.cpp`, `README.md`, and `README_ch.md`, then rerun the focused V2/V3/configuration tests and record exact outcomes in `specs/114-ndn-svs-v3-wire-interop/evidence/focused-validation.md` (FR-004, FR-011, FR-014, FR-022, FR-026; SC-005, SC-011)

**Checkpoint**: Migration and rollback are explicit; no hybrid packet or silent
downgrade remains.

---

## Phase 6: User Story 4 - Standards-Safe Fork Extensions (Priority: P2)

**Goal**: Mapping/Repair remain useful without redefining or partially
poisoning core V3.

**Independent Test**: Standard-only, known, unknown, and malformed extension
cases preserve the same validated core vector; malformed known extensions
commit no extension state.

- [x] T019 [P] [US4] Add standard-only/known/unknown/duplicate/malformed trailing-block tests in `../ndn-svs/tests/unit-tests/v3-wire.t.cpp` and `svspubsub.t.cpp`, then replace the single extra-block assumption with a bounded trailing-block collection after the standard embedded Data in `../ndn-svs/ndn-svs/sync-protocol.cpp/.hpp` and `core.cpp/.hpp` (FR-016, FR-017; SC-006)
- [x] T020 [US4] Add prepare/commit and Spec 113 deduplication/repair tests, then emit and atomically parse MappingData/RepairData after the embedded Data with unknown-block skipping, duplicate-known rejection, and structured diagnostics in `../ndn-svs/ndn-svs/svspubsub.cpp/.hpp` (FR-016, FR-017, FR-023; SC-006)
- [x] T021 [US4] Add V3 whole-parameters LZMA rejection and explicit V2-retention tests, enforce the profile rule in `../ndn-svs/ndn-svs/core.cpp/.hpp` and `sync-protocol.cpp`, then run existing sparse-mapping, repair, duplicate-fetch, segmented-publication, transactional-async, and callback-lifetime regressions and record exact outcomes in `specs/114-ndn-svs-v3-wire-interop/evidence/focused-validation.md` (FR-018, FR-023; SC-006)

**Checkpoint**: The candidate is accurately described as core V3 plus optional
NDNSF extensions.

---

## Phase 7: User Story 5 - Independent and Network Evidence (Priority: P2)

**Goal**: Prove bidirectional interoperability and NDNSF consumer safety against
one immutable candidate.

**Independent Test**: The quickstart reproduces fixed, standalone, MiniNDN, and
consumer outcomes from committed identities.

- [x] T022 [P] [US5] Implement the deterministic C++ V2/V3 peer, HMAC test signing/verification, final-vector and JSONL event contract in `../ndn-svs/tests/interop/cpp/svs3-peer.cpp` and add its target to `../ndn-svs/tests/wscript` (FR-020, FR-021, FR-025)
- [x] T023 [P] [US5] Implement the lockfile-pinned NDNts peer with explicit V3 timers, matching HMAC test policy, bounded publication, final-vector and JSONL events in `../ndn-svs/tests/interop/ndnts/svs3-peer.mjs` (FR-004, FR-006, FR-020, FR-021)
- [x] T024 [US5] Implement cleanup and C++→NDNts, NDNts→C++, concurrent, explicit-V2, and declared-profile-mismatch cases in `../ndn-svs/tests/interop/run-svs3-interop.sh`; run the complete rebuilt `../ndn-svs/build/unit-tests` suite and standalone cases; record counts, hashes, vector/packet digests, exact sequence coverage, and zero Sync Ack in `evidence/full-build.md` and `evidence/standalone-interop.md` before MiniNDN (FR-015, FR-020, FR-021, FR-025; SC-001, SC-002, SC-005, SC-006, SC-007, SC-008, SC-010)
- [x] T025 [US5] Add red callback-range, duplicate/missing-sequence, final-vector, packet-sample, cleanup, and run-once tests in `tests/python/test_spec114_svs_v3_tools.py`, then implement the two-node topology, loss injection, peer commands, aggregation, Sync Ack detection, and immutable cell summaries in `Experiments/NDN_SVS_V3_Interop_Minindn.py`; freeze the clean candidate and six cell IDs in `results/spec114-svs-v3/<candidate>/candidate-manifest.json` only after T024 passes (FR-020, FR-021, FR-025)
- [x] T026 [US5] Execute exactly once for the frozen candidate the six formal cells `loss00-run01..03` and `loss05-run01..03`, each with 20 C++ plus 20 NDNts publications and the fixed 60-second convergence bound; preserve every result, including failures, and validate 6/6 identity, 120/120 unique 0%-loss remote sequences from callback ranges, zero duplicate coverage/Sync Ack/restart, 5%-loss convergence, equal final vectors, packet samples, and unchanged source/configuration in `specs/114-ndn-svs-v3-wire-interop/evidence/minindn-validation.md` (FR-021; SC-003, SC-004, SC-010)
- [x] T027 [US5] Install or link the committed NDN-SVS candidate, rebuild affected NDNSF C++/Python consumers, bind installed/built hashes, and run focused Spec 112/113 segmented Normal/Targeted sync/async, degraded timeout, burst liveness, explicit V2 rollback, default V3/200 ms, explicit 1 ms, GUI round-trip, and affected Targeted tests from `quickstart.md`; record exact pass/skip/failure counts in `specs/114-ndn-svs-v3-wire-interop/evidence/ndnsf-regression.md` (FR-022, FR-026; SC-008, SC-011)
- [x] T028 [US5] Organize NDN-SVS changes into reviewable protocol/codec, validation/safety, extension, and interop commits above `c34c04d`; verify diff scope/revert effects/test attribution; then complete `specs/114-ndn-svs-v3-wire-interop/traceability.md`, `completion-summary.md`, and `evidence/commit-review.md` with OIDs, candidate ID, test counts, packet digests, negative results, V2 disposition, and explicit no-merge/no-push status (FR-021, FR-023, FR-024; SC-009)

**Checkpoint**: A merge decision can be made from independent immutable
evidence; Spec 114 itself does not merge or push.

---

## Phase 8: Final Audit and Convergence

**Purpose**: Close only the work actually supported by evidence.

- [x] T029 Run strict Spec Kit structure/prerequisite/placeholder/coverage checks, code-aware post-implementation audit, and Spec Kit convergence; append and execute any newly discovered work before checking T029, and record a zero-Critical/High final verdict in `specs/114-ndn-svs-v3-wire-interop/evidence/final-audit.md` (FR-024; SC-009)

---

## Dependencies and Execution Order

```text
Phase 1 baseline
  -> Phase 2 fixed fixtures/shared foundation
     -> US1 standard V3 wire
        -> US2 validation/state safety
        -> US3 explicit migration
           -> US4 extension adaptation
              -> US5 independent/network/consumer evidence
                 -> final audit
```

- T004-T006 must expose the hybrid behavior before T007-T010 change it.
- US2 depends on US1's valid envelope; US3 default switching waits for US1/US2.
- US4 waits for the stable embedded-Data/trailing-block boundary.
- T022 and T023 may proceed in parallel only after the wire contract is stable.
- T024 is the full unit/standalone gate; T025 cannot freeze a candidate earlier.
- T026 is one matrix task, but its six named cells remain sequential and
  run-once under a single cleanup owner.
- T027 starts only after T026 preserves the complete candidate result.
- T028-T029 close only after all implementation and evidence tasks finish.

## Parallel Opportunities

- T002 can proceed while T001 records local evidence.
- T004's fixed fixtures and T005's public options tests touch independent files.
- T011 and T012 may draft independent security/vector tests after US1.
- T022 and T023 implement independent peers in separate languages/files.
- Formal MiniNDN cells are deliberately not parallelized.

## Implementation Strategy

### Wire MVP

1. Complete T001-T006.
2. Complete US1 T007-T010 and stop at its focused evidence checkpoint.

### Minimum safe candidate

1. Complete US2 T011-T014 and US3 T015-T018.
2. Run their complete focused negative/V2/V3 tests once.
3. Adapt extensions only after those gates pass.

### Formal candidate

1. Complete T019-T025 and freeze one manifest only after full unit/standalone success.
2. Execute the six-cell matrix once through T026.
3. Complete consumer regression, review, and final audit through T029.

## Completion Definition

- All 32 tasks are checked with linked evidence.
- Every FR-001..FR-026 and SC-001..SC-011 has task/test/evidence coverage.
- V3 vectors, both independent directions, six formal MiniNDN cells, V2
  regression, extension isolation, and NDNSF consumer gates have executed.
- No failed candidate/cell was overwritten or rerun under the same identity.
- Final audit/convergence reports zero Critical/High and appends no work.
- Experimental remains a clean reviewable delta above Spec 113; no remote ref,
  local master, or release was mutated.

## Phase 9: Convergence

- [x] T030 [US2] Defer V3 StateVector semantic decoding until the embedded Data signature/policy validation callback succeeds, add serial/parallel ordering and next-valid tests in `../ndn-svs/tests/unit-tests/core.t.cpp` and `v3-wire.t.cpp`, and preserve V2 immediate decoding in `../ndn-svs/ndn-svs/sync-protocol.cpp/.hpp` and `core.cpp` per FR-004, FR-010, and the receive-pipeline contract (partial)
- [x] T031 [US4] Add collection-level prepare/commit validation for all trailing MappingData/RepairData blocks and make serial/parallel extension delivery occur after the validated core merge through one collection callback in `../ndn-svs/ndn-svs/core.cpp/.hpp`, `svspubsub.cpp/.hpp`, and their unit tests per FR-013, FR-016, and FR-017 (partial)
- [x] T032 [US5] Rebuild and recommit the converged NDN-SVS tree, create a new immutable candidate, rerun the full unit/standalone and exactly-once six-cell MiniNDN matrix, reinstall/rebuild NDNSF, rerun the affected consumer/configuration gates, and supersede completion evidence without deleting prior candidates per FR-021-FR-024 and SC-003-SC-011 (partial)
