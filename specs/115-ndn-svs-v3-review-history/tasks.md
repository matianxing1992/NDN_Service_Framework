# Tasks: Complete SVS V3 Review Commit

**Input**: Design documents from
`specs/115-ndn-svs-v3-review-history/`

**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`,
`contracts/`, `quickstart.md`

**Tests**: Required. Each task includes the tests, inspections, and evidence
needed to close its behavioral outcome. Tests and evidence are not separate
mechanical checklist items.

**Organization**: Historical tasks preserve earlier rewrite evidence. The
current unchecked phase implements the 2026-08-07 focused PR override. Task
count is not a completeness target.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Safe to execute in parallel because it does not mutate the same ref,
  worktree, candidate manifest, or evidence authority.
- **[Story]**: Maps the task to a user story in `spec.md`.
- Every task names its source, tooling, evidence, and acceptance surface.

## Phase 1: Immutable Baseline and Rewrite Authority

**Purpose**: Fence current local, remote, and PR identities before constructing
any replacement history.

- [x] T001 Refresh `origin` and `upstream`, capture the exact local `master`, `origin/master`, `Experimental`, upstream base, PR #36 head/review state, tree IDs, target worktree cleanliness, and active worktree owners; create uniquely named immutable local safety refs without moving live branches and record the complete baseline and abort conditions in `specs/115-ndn-svs-v3-review-history/evidence/baseline-and-backups.md` (FR-001, FR-002, FR-021, FR-022; SC-003)
- [x] T002 [P] Build a test-first commit-ownership and ref-verification tool in `Experiments/spec115_v3_history_rewrite.py` with focused tests in `tests/python/test_spec115_v3_history_rewrite.py`; map old `1b1ff2c`, `76bb042`, `dd092ae`, `53dd158`, fork-extension, reliability, harness, and review-tail hunks to their permitted owners, reject duplicate/unowned official-V3 groups and unsafe force commands, and write the accepted manifest to `specs/115-ndn-svs-v3-review-history/evidence/commit-ownership.md` (FR-003-FR-009, FR-016, FR-024; SC-001, SC-009)

**Checkpoint**: Old identities are recoverable and every change group has one
declared semantic owner before history surgery begins.

---

## Phase 2: User Story 1 - One Complete V3 Review Commit (Priority: P1) MVP

**Goal**: Replace the hybrid V3 commit with one independently correct official
V3 commit and a reviewable PR tail.

**Independent Test**: Check out the replacement V3 OID alone, build it, run its
focused wire/state/security tests, and confirm no descendant owns an official
V3 repair.

- [x] T003 [US1] In a uniquely named isolated NDN-SVS rewrite worktree, reconstruct the commit currently represented by `1b1ff2c` as one `svs: implement standards-compliant V3 sync protocol` commit by combining the old BootstrapTime/multi-epoch behavior with the official Core/codec/validation/timing corrections from `76bb042`, `dd092ae`, and the official-V3 portions of `53dd158`; preserve original author attribution, include generic V3 PubSub/MappingProvider integration and directly coupled tests but exclude Mapping/Repair extension semantics, then build and execute the fixed-vector, malformed-input, validation-before-decode, atomicity, timer, zero-Sync-Ack, and serial/parallel gates at that exact commit and record OID/tree/commands/results in `specs/115-ndn-svs-v3-review-history/evidence/isolated-v3-validation.md` (target paths: `../ndn-svs/examples/chat.cpp`, `ndn-svs/common.hpp`, `core.cpp`, `core.hpp`, `mapping-provider.cpp`, `mapping-provider.hpp`, `sync-protocol.cpp`, `sync-protocol.hpp`, `version-vector.cpp`, `version-vector.hpp`, `svspubsub.cpp`, `svspubsub.hpp`, `svsync-base.cpp`, `svsync-base.hpp`, `svsync.hpp`, `svsync-shared.hpp`, `tlv.hpp`, `tests/unit-tests/core.t.cpp`, `mapping-provider.t.cpp`, `version-vector.t.cpp`, `v3-wire.t.cpp`, `security-options.t.cpp`, `svspubsub.t.cpp`, and `tests/wscript`) (FR-003, FR-004, FR-005, FR-006, FR-012; SC-001, SC-002)
- [x] T004 [US1] Replay the remaining PR #36 commits after the replacement V3 commit, remove or fold V3-specific portions of the old aggregate test/fix commits into the new owner while preserving non-V3 behavior, prove with `Experiments/spec115_v3_history_rewrite.py` that no descendant exists solely to repair official V3 behavior, run per-commit diff-check/build gates, and record the final candidate master sequence plus any explicitly deferred non-V3 review comment in `specs/115-ndn-svs-v3-review-history/evidence/commit-ownership.md` (FR-004, FR-009, FR-021, FR-022; SC-001, SC-009)
- [x] T005 [US1] Validate the isolated replacement V3 commit with independent fixtures and C++/NDNts peers from `../ndn-svs/tests/fixtures/svs-v3/` and `../ndn-svs/tests/interop/` without placing the harness implementation inside the official V3 commit; confirm normative `/v=3`, embedded signed Data, parameters digest, bidirectional sequence coverage, invalid-packet rejection, and explicit V2 isolation, then bind packet digests and results to the replacement OID in `specs/115-ndn-svs-v3-review-history/evidence/isolated-v3-validation.md` (FR-006, FR-008, FR-012, FR-014; SC-002)

**Checkpoint**: The replacement V3 commit is independently standards-compliant
and the rewritten PR tail contains no V3 repair-only descendant.

---

## Phase 3: User Story 2 - Preserve Experimental Behavior (Priority: P1)

**Goal**: Replay separately owned work on the rewritten PR head without losing
or duplicating the validated final tree.

**Independent Test**: The candidate Experimental tree equals the frozen
pre-rewrite `53dd158` tree and all new-OID candidate gates pass.

- [x] T006 [US2] Reconstruct `Experimental` on the candidate master by replaying the Spec 113 reliability/Boost commits, one separately owned bounded Mapping/Repair extension commit containing the extension-atomicity portions of `ee2ea87` and `53dd158`, and one independent interop/harness/docs commit containing `3636b32` plus its final script correction; drop Core V3 changes already owned by master, audit duplicated/lost hunks, require zero unexplained tree difference from the frozen old Experimental tree, and record old/new commit and tree mappings in `specs/115-ndn-svs-v3-review-history/evidence/experimental-replay.md` (FR-007, FR-008, FR-010, FR-011, FR-021; SC-003, SC-006)
- [x] T007 [US2] Generate a new candidate manifest for the rewritten Experimental OID and run the complete NDN-SVS unit suite, standalone C++/NDNts interop, immutable Spec 114 MiniNDN matrix, and NDNSF Spec 112 consumer regressions using the canonical commands and no stale OID-bound receipt; continue from focused failures rather than restarting the matrix after each repair, then update `specs/115-ndn-svs-v3-review-history/evidence/full-validation.md` and the affected Spec 114 evidence with exact commands, counts, identities, and negative outcomes (FR-013, FR-014, FR-024; SC-004, SC-009)

**Checkpoint**: Both the isolated review commit and complete Experimental
candidate are validated under their rewritten identities.

---

## Phase 4: User Story 3 - Fenced Publication and Recovery (Priority: P2)

**Goal**: Publish the verified candidate so all required refs contain the
complete V3 commit, while retaining deterministic rollback.

**Independent Test**: A fresh fetch proves master equality, three-ref V3
containment, PR head identity, and remote backup identity.

- [x] T008 [US3] After an explicit publication authorization and a final refresh proving `origin/master` still equals the T001 expected OID, create a uniquely named remote safety branch at that exact old OID, verify it independently with a remote-ref query, prepare the candidate-to-backup lease-protected rollback command, and record the verified remote receipt in `specs/115-ndn-svs-v3-review-history/evidence/baseline-and-backups.md`; abort without retry on any fence, authorization, validation, or backup mismatch (FR-001, FR-002, FR-015, FR-020, FR-023; SC-007)
- [x] T009 [US3] Move local `master` and `Experimental` only to their fully validated candidate OIDs, update `origin/master` with an explicit `refs/heads/master:<expected-old-oid>` force-with-lease and no other remote ref mutation, fetch again, and use `Experiments/spec115_v3_history_rewrite.py` plus direct Git queries to prove `master == origin/master`, replacement-V3 ancestry from `master`, `origin/master`, and `Experimental`, unchanged upstream/tags/releases, and exact remote backup retention; record the commands and identities in `specs/115-ndn-svs-v3-review-history/evidence/publication-receipt.md` (FR-016, FR-017, FR-018, FR-021, FR-022, FR-023, FR-024; SC-005, SC-006, SC-007)
- [x] T010 [US3] Verify PR #36 now points to the published master OID, update its review description with the replacement V3 OID, standards-correction summary, independent interop evidence, separately owned extension boundary, and exact rollback reference, then inspect the public commit list from a fresh remote query and record the PR/branch/rollback receipt in `specs/115-ndn-svs-v3-review-history/evidence/publication-receipt.md` without claiming upstream merge or release (FR-019, FR-020, FR-023, FR-024; SC-008)

**Checkpoint**: The complete V3 commit is visible from all required refs and PR
#36, and the old remote state remains recoverable.

---

## Phase 5: Completion Audit

**Purpose**: Prove that history, source behavior, evidence, and external state
all satisfy the same feature contract.

- [x] T011 Run strict structure, full post-implementation semantic/code/remote audit, task-cohesion scan, fresh-fetch containment checks, evidence identity verification, and rollback-recipe review; resolve any actionable gap through focused validation, then write `specs/115-ndn-svs-v3-review-history/audit-report.md` and `completion-summary.md` with the final old/new OIDs, tree equality, test counts, PR state, backup retention, limitations, and explicit statement that upstream branches/tags/releases were not changed (FR-001-FR-024; SC-001-SC-009)

## Dependencies & Execution Order

```text
T001 ─┬─> T003 -> T004 -> T005 -> T006 -> T007 -> T008 -> T009 -> T010 -> T011
      └─> T002 ────────────────┘
```

- T001 and T002 establish independent identity and ownership authorities.
- T003-T005 close the independently reviewable V3 commit before Experimental
  is replayed.
- T006 requires the final candidate master; T007 requires the final candidate
  Experimental.
- T008-T010 are external-state tasks and cannot begin before full local
  validation and explicit authorization.
- T011 depends on fresh remote and PR receipts, not checked boxes alone.

## Parallel Opportunities

- T002 can develop and test the ownership verifier while T001 records the
  baseline, provided it does not mutate target refs.
- Expensive validation commands inside T007 may run concurrently only when
  they use distinct immutable candidate directories and do not share MiniNDN
  launcher/cleanup ownership.
- No history-construction or remote-publication task is marked parallel because
  they mutate shared refs and evidence authorities.

## Implementation Strategy

### Reviewable MVP

T001-T005 produce a complete, independently validated V3 commit and clean local
candidate master without changing live branches or the remote. This is the
first safe review checkpoint.

### Preservation Gate

T006-T007 reconstruct and fully validate Experimental. Failure here rejects the
candidate and leaves local/remote master unchanged.

### Publication Gate

T008-T010 perform the only external mutation, protected by explicit
authorization, an exact remote fence, and a verified backup. T011 closes the
feature only after fresh remote observation.

## Task-Cohesion Review

- Mechanically fragmented groups detected: 0.
- Coalescing opportunities remaining: 0.
- Tests, implementation, command execution, and evidence are combined when
  they close one behavior; tasks are split only at real ref/risk/acceptance
  boundaries.

## Phase 6: Convergence After Upstream Review

**Purpose**: Correct the default NFD registration behavior and replace the
route-injected interoperability claim with evidence from unmodified peer
registration before requesting another PR #36 review.

- [x] T012 [US1] Restore automatic cross-implementation reachability by first adding a failing registration/interop case, then separating the versioned local Interest filter from group-prefix NFD registration in `../ndn-svs/ndn-svs/core.cpp` and `../ndn-svs/ndn-svs/core.hpp`, removing the manual `nfdc route add` compensation from `../ndn-svs/tests/interop/run-svs3-interop.sh`, and proving C++→NDNts, NDNts→C++, concurrent V3, explicit V2, mismatch isolation, registration failure, and lifecycle cleanup without route injection per FR-008, SC-002, and SC-004
- [x] T013 [US1] Remove downstream-only review artifacts from the upstream candidate by replacing every tracked `Spec 114`/`spec114` fixture and test identity with neutral SVS V3 test names, regenerating and checking deterministic fixtures, and replacing `NDNSF_SVS_*` environment-variable coupling with typed `SVSPubSubOptions` limits plus unchanged defaults and focused option-boundary tests in `../ndn-svs/tests/fixtures/svs-v3/`, `../ndn-svs/tests/unit-tests/`, `../ndn-svs/ndn-svs/svspubsub.cpp`, and `../ndn-svs/ndn-svs/svspubsub.hpp` per FR-004 and FR-008
- [x] T014 [US2] Validate the complete local reviewer-fix candidate with `git diff --check`, fixture regeneration/no-diff, the full ASan unit suite, and the five-case no-injection C++/pinned-NDNts matrix; preserve exact OIDs, commands, counts, negative outcomes, automatic FIB observations, and the invalidated earlier route-injected claim in `specs/115-ndn-svs-v3-review-history/evidence/full-validation.md` and update `audit-report.md` so no stale evidence is counted per FR-013, FR-014, FR-024, SC-002, and SC-004
- [x] T015 [US3] Form one local reviewable follow-up commit above `master@2b052c9`, verify its parent/tree/diff and every-commit unit status, and prepare a concise replacement PR description plus resolution replies for the three outdated threads and the active fixture-name thread in `specs/115-ndn-svs-v3-review-history/evidence/publication-receipt.md`; do not push, edit PR #36, resolve threads, or mutate any remote until the user separately authorizes publication per FR-019, FR-021, FR-023, and FR-024

**Convergence dependency**:

```text
T012 -> T013 -> T014 -> T015
```

T012 owns the correctness blocker. T013 closes bounded upstream-review hygiene
without changing protocol semantics. T014 is the candidate acceptance gate.
T015 creates only local review material and preserves the remote authorization
boundary.

## Phase 7: Review-Fix Ownership Fold

**Purpose**: Place each validated review correction in the commit that first
introduced the behavior, then rebuild both local branch lines without a
repair-only PR tail.

- [x] T016 Create immutable local safety refs for public `master@2b052c9`, `Experimental@321c314`, reviewer candidate `7eeb63c`, and corrected harness `2bb8b37`; construct the rewrite only in a new isolated worktree and verify all source worktrees remain clean and recoverable (FR-001, FR-021, FR-022)
- [x] T017 Split `7eeb63c` by semantic ownership: fold generic typed `SVSPubSubOptions`, removal of `NDNSF_SVS_*`, and its behavior test into the commit currently represented by `7433913`; fold group-prefix registration, version-specific local filtering, bounded registration failure, lifecycle tests, neutral V3 fixture identities, and regenerated fixtures into the commit currently represented by `aa746fa`; replay `004d2bb` and `2b052c9` without retaining a repair-only descendant (FR-003, FR-004, FR-008, FR-009)
- [x] T018 Validate the rewritten master sequence at every new commit with diff checks and the complete unit suite available at that commit; validate the rewritten V3 owner independently with deterministic fixtures and registration tests, and record the old/new OID and tree mapping in the ownership evidence (FR-004, FR-012, FR-014, FR-024)
- [x] T019 Replay the six Experimental-only commits onto the rewritten master, apply the no-route-injection harness correction as Experimental-only work, run the final ASan unit suite and five-case C++/NDNts matrix, then update audit/completion/publication evidence while leaving `origin/master`, PR #36, comments, and review threads unchanged pending authorization (FR-007, FR-010, FR-013, FR-019, FR-023, FR-024)

**Ownership-fold dependency**:

```text
T016 -> T017 -> T018 -> T019
```

## Phase 8: Final Review Narrative and Three-Implementation Proof

**Purpose**: Make the rewritten history understandable commit-by-commit and
bind fresh standalone plus MiniNDN C++/NDNts evidence to the final message-only
OIDs before any remote publication.

- [x] T020 Create new safety refs for the validated `master@c62f4b5` and `Experimental@b1c3f49`, then rewrite only commit metadata so every master and Experimental subject/body states its owned behavior, boundary, and reason; require identical old/new tree IDs, one complete V3 owner shared by both local branches, no repair-only descendant, and no remote mutation (FR-003, FR-007, FR-009, FR-018, FR-021-FR-024)
- [x] T021 Revalidate the final message-only history with ownership-manifest checks, per-commit tree/test binding, final ASan units, deterministic fixtures, banned-string scans, and the five-case no-route-injection standalone C++/pinned-NDNts matrix; record exact new OIDs, counts, and digests in Spec 115 evidence (FR-004, FR-008, FR-012-FR-014, FR-024; SC-001-SC-002, SC-004, SC-009)
- [x] T022 Freeze a new manifest for the exact final Experimental checkout and execute once the six formal MiniNDN C++ NDN-SVS/NDNts cells—three at 0% loss and three at 5% loss, 20 publications per peer, fixed 60-second bound—preserving failures and proving bidirectional coverage, equal final vectors, zero duplicate/reject/restart/Sync-Ack outcomes, then rerun strict audit and update completion status without pushing origin or editing PR #36 (FR-013-FR-014, FR-019, FR-021, FR-023-FR-024; SC-003-SC-004, SC-008-SC-009)

**Final-validation dependency**:

```text
T020 -> T021 -> T022
```

## Phase 9: External Interoperability Ownership Correction

**Purpose**: Prove the cross-implementation claim with an actual TypeScript
peer while keeping all executable interop examples and Node dependencies in
NDNSF rather than the NDN-SVS review history.

- [x] T023 Move the external C++ SVS peer, pinned NDNts dependencies, standalone runner, and peer logic into `examples/interop/ndn-svs-v3/` in NDNSF; convert the NDNts peer from JavaScript ESM to an actual Node 22-compatible `.ts` TypeScript source, update the MiniNDN launcher and candidate manifest to hash and launch only NDNSF-owned peer assets, and add focused ownership/language/path tests (FR-008, FR-013-FR-014, FR-024-FR-025; SC-004, SC-010)
- [x] T024 Create a new immutable safety ref for `Experimental@7fd50f9`, move the local Experimental branch to the last separately owned code/unit-test commit before the two interop-harness commits, and prove that `master` and `Experimental` still share `eb0754d`, their production/unit trees are preserved, and neither branch tracks `tests/interop`, external Node dependencies, or NDNSF orchestration files; do not modify any remote ref (FR-007-FR-011, FR-018, FR-021-FR-025; SC-001, SC-003, SC-010)
- [x] T025 Build the NDNSF-owned C++ peer against the exact clean NDN-SVS Experimental tree, install the pinned NDNts dependencies under NDNSF, run multiple standalone TypeScript/C++ cases and a fresh run-once MiniNDN 0%/5% matrix from NDNSF paths, then update exact OIDs, manifests, digests, validation evidence, completion status, and strict post-implementation audit while preserving all failures and leaving origin/PR unchanged (FR-012-FR-014, FR-019, FR-023-FR-025; SC-002, SC-004, SC-008-SC-010)

**Ownership-correction dependency**:

```text
T023 -> T024 -> T025
```

## Phase 10: Experimental Commit Reviewability

**Purpose**: Preserve the accepted Experimental implementation while making
each local-only commit independently understandable, testable, and reversible.

- [x] T026 Freeze `master@e42996d`, `Experimental@70e682f`,
  `origin/master@2b052c9`, and the exact Experimental tree in an immutable
  safety ref and evidence record; audit the five existing commits into the
  seven semantic ownership units defined by FR-026-FR-028, and prepare an
  isolated rewrite worktree without changing a live or remote ref.
- [x] T027 Construct the seven-commit candidate in dependency order, repairing
  a commit before advancing whenever configure, build, or the complete unit
  suite fails; bind every command and result to its exact OID and preserve the
  old/new commit and tree mapping in evidence (FR-026-FR-029; SC-011-SC-012).
- [x] T028 Verify final tree identity, branch ancestry, semantic diff
  boundaries, `git diff --check`, and unchanged master/remote/tag state; only
  then move local Experimental to the validated head, rerun the final complete
  unit suite and strict Spec Kit audit, and update completion/rollback evidence
  without pushing or editing PR #36 (FR-030; SC-013).

**Reviewability dependency**:

```text
T026 -> T027 -> T028
```

## Phase 11: Full Local Commit Consolidation

**Purpose**: Replace the mechanically separated 17-commit local range with a
smaller behavior-owned history in which later bug fixes live in their owning
mechanisms, while preserving the accepted final source tree and all remote
state.

- [x] T029 Freeze `master == Experimental == 8335643f`, `origin/master ==
  2b052c9`, `upstream/master == a937247`, the final tree ID, and all remote/tag
  identities in immutable local safety refs and evidence; map every one of the
  17 source commits into the at-most-12 ownership units defined by
  `contracts/history-consolidation.md`, reject unowned or duplicated changes,
  and prepare a unique isolated rewrite worktree without moving a live or
  remote ref (FR-021-FR-030; SC-009, SC-011, SC-013).
- [x] T030 Construct the complete candidate history in dependency order, fold
  every corrective-only change into its mechanism owner, preserve the official
  V3 and Boost fork-policy boundaries, produce clear subject/body messages,
  prove candidate-head tree identity and per-commit semantic path boundaries,
  and record the old-to-new OID/owner map before any test campaign or live-ref
  movement (FR-003-FR-011, FR-021-FR-030; SC-001, SC-003, SC-011, SC-013).
- [x] T031 Walk every surviving exact OID only after construction, run `git
  diff --check`, configure/build, and the complete available unit suite, repair
  a failure in its owner and resume from that OID, then rerun final ownership,
  tree, branch, strict-structure, and post-implementation semantic audits; move
  local `master` and `Experimental` together only after all gates pass, update
  exact evidence/completion/rollback records, and leave origin/PR/upstream/tags
  untouched (FR-012-FR-014, FR-021-FR-030; SC-001-SC-004, SC-009-SC-013).

**Consolidation dependency**:

```text
T029 -> T030 -> T031
```

## Phase 12: Convergence

- [x] T032 Rebuild the exact clean `Experimental@0c09d65` candidate with
  AddressSanitizer, the complete unit suite, and the NDNSF-owned C++ peer;
  install the pinned NDNts dependencies, execute the five-case standalone
  TypeScript/C++ matrix, freeze a new content-addressed manifest, and run the
  six immutable formal MiniNDN 0%/5% cells once with 20 publications per peer
  and the fixed 60-second bound, preserving failures and recording exact OIDs,
  hashes, counts, final-vector equality, and zero duplicate/reject/restart/
  Sync-Ack evidence in Spec 115 (FR-012-FR-014, FR-024; SC-002-SC-004,
  SC-009-SC-010, SC-012; partial).
- [ ] T033 Refresh and fence the current remote `origin/master` (expected
  `0c09d65` after the first publication), publish a uniquely named remote safety
  branch at that exact OID and verify it independently,
  validate the exact force-with-lease command, then update only remote master to
  the fully validated local candidate; fetch again and record local/remote/
  Experimental/V3 ancestry, unchanged upstream/tags/releases, backup identity,
  and the exact lease-protected rollback command (FR-001-FR-002,
  FR-015-FR-018, FR-020-FR-024; SC-005-SC-007, SC-013; missing).
- [ ] T034 Monitor every GitHub Actions check for the exact published head,
  repair any failure in its semantic owner and repeat affected local/network/
  fenced-publication gates until all required checks are green; then update PR
  #36 description and actionable review threads with the final nine-commit narrative,
  complete V3 owner, final interop evidence, extension boundary, safety ref and
  rollback receipt, perform a fresh remote/PR query plus strict post-implementation
  Spec Kit audit, and update completion evidence without claiming upstream merge
  or release (FR-003-FR-009, FR-013-FR-024; SC-001-SC-013; partial).

**Convergence dependency**:

```text
T032 -> T033 -> T034
```

T033 was completed once for candidate `0c09d65`, but exact-head CI exposed a
missing direct `<atomic>` include. The correction was folded into owner 11 and
produced validated local candidate `6b1fc52`. Because the user then prohibited
further pushes until a new explicit request, T033 is reopened for the corrected
candidate and T034 remains open. The earlier publication and safety receipt are
preserved as historical evidence, not counted as current-candidate completion.

## Phase 13: NDN-SVS Boost Policy and Commit-Boundary Correction

- [x] T035 Replace the 12-commit local candidate with an 11-commit review
  history that removes the committed Boost 1.71 build-policy owner, keeps Boost
  1.74 in every surviving NDN-SVS commit, moves `setPeriodicSyncTime()` from the
  piggyback/fetch owner into the parallel-Sync owner, applies the reviewed
  subject/body messages, and validates every exact OID through a newly created
  and deleted `compileTMP` branch carrying the temporary 1.71 threshold; then
  prove final-tree equality with the pre-boundary 1.74 candidate, move local
  `master` and `Experimental` together, update ownership/validation/audit
  evidence, and leave origin/PR/upstream/tags plus all non-NDN-SVS Boost policy
  untouched (FR-026-FR-032; SC-011-SC-014).

**Corrected convergence dependency**:

```text
T032 -> T035 -> T033 -> T034
```

## Phase 14: Nine-Owner State-Machine Correction

**Purpose**: Replace the locally validated 11-commit history with the corrected
nine-owner review history after code inspection proved that segmented
publication and segmented recovery are distinct state machines.

- [x] T036 Freeze the current local `master`/`Experimental`, `origin/master`,
  `upstream/master`, tags, final tree, and all worktrees in immutable local
  safety refs; update the ownership contract and strict pre-implementation
  audit for exactly nine owners, then create a unique isolated rewrite
  worktree without moving any live or remote ref (FR-021-FR-032;
  SC-009, SC-011, SC-013-SC-014).
- [x] T037 Construct the nine commits in dependency order: fold piggyback
  bounds and piggyback tests into the piggyback owner, fold the async test into
  the async-production owner, clarify the V2-only InterestSigner owner, keep
  official V3 isolated, split producer-side failure-atomic segmented
  publication from receiver-side bounded fetch/Repair recovery, and fold the
  old extension-validation fix into the recovery owner; prove exact final-tree
  equality and record every old-to-new hunk/test owner before moving live refs
  (FR-003-FR-014, FR-025-FR-032; SC-001-SC-003, SC-010-SC-014).
- [x] T038 Walk the nine exact OIDs in order, creating and deleting
  `compileTMP` for each commit, temporarily lowering only that branch to Boost
  1.71, then running `git diff --check`, configure/build, and the complete unit
  suite. Repair a failure in its semantic owner and resume from that OID. After
  all nine pass, verify Boost 1.74 in every reviewed commit, zero remaining
  `compileTMP`, exact final-tree equality, and unchanged remote/upstream/tags;
  only then move local `master` and `Experimental` together and update
  ownership, validation, audit, and completion evidence without pushing or
  editing PR #36 (FR-012-FR-014, FR-021-FR-032; SC-001-SC-004,
  SC-009-SC-014).

**Nine-owner dependency**:

```text
T036 -> T037 -> T038 -> T033 -> T034
```

## Phase 15: Focused Three-Commit V3 PR Boundary

**Purpose**: Reorder the validated behavior owners so that one local branch
contains only regex/name-only PubSub, V2 InterestSigner, and corrected SVS V3,
while all other work remains on a descendant continuation branch.

- [x] T039 Freeze the current local/remote/worktree identities and dirty-state
  hashes, create uniquely named immutable local safety refs, and create a new
  isolated sibling worktree rooted at refreshed `upstream/master`; record the
  baseline only under Spec 115 and do not move the dirty `Experimental`
  checkout, any live branch, remote, tag, or PR (FR-037-FR-040; SC-018).
- [x] T040 Reconstruct the first two independently buildable commits from the
  existing behavior owners: regex/name-only PubSub and V2 ndn-cxx
  `InterestSigner`; include their directly coupled unit tests, exclude every
  planning/evidence/harness file, and validate each exact OID with diff checks
  and the disposable `compileTMP` full-unit procedure (FR-033, FR-038;
  SC-015-SC-017).
- [x] T041 Rebuild the complete V3 owner on only those two parents, fold the
  current normative SVS-PS V3 outer-Data, Mapping-query, and signature-coverage
  corrections into their introduction points, remove obsolete unsigned
  trailing-extension semantics, include directly coupled deterministic and
  malformed-wire tests, and validate the exact commit in isolation plus the
  NDNSF-owned C++/NDNts interop harness (FR-033-FR-035, FR-038-FR-039;
  SC-015-SC-017).
- [x] T042 Create a dedicated local focused-PR branch at the corrected V3 OID,
  prove its three-commit order and excluded-path set, then replay the six
  remaining behavior owners onto a separate continuation branch, adapting
  MappingData to the signed SVS-PS V3 content contract and attributing every
  intentional final-tree difference in Spec 115 evidence (FR-034, FR-036,
  FR-039-FR-040; SC-015-SC-018).
- [x] T043 Run strict Spec Kit structure/post-implementation audit, exact-OID
  diff/unit/wire checks, final ownership and branch-topology verification, and
  dirty-checkout/remote/tag identity checks; update only NDNSF evidence and
  leave both local branches unpushed for the user to inspect and publish later
  (FR-033-FR-040; SC-015-SC-018).

**Focused rewrite dependency**:

```text
T039 -> T040 -> T041 -> T042 -> T043
```
