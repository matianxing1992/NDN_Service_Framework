# Tasks: NDN-SVS Experimental Convergence

**Input**: Design documents from
`specs/113-ndn-svs-experimental-convergence/`

**Prerequisites**: `spec.md`, `plan.md`, `research.md`, `data-model.md`,
`contracts/`, `quickstart.md`

**Tests**: Mandatory and test-first because this feature changes Git recovery,
distributed sequence ordering, rollback, thread ownership, and merge evidence.

**Scope rule**: No remote push; no NDNSF-DI/UAV/Repo/container change; no 5%
loss tuning. Preserve unrelated dirty root-repository changes.

## Phase 1: Setup And Immutable Baseline

**Purpose**: Bind the exact pre-migration state before any branch reference moves.

- [x] T001 Record local/remote refs, reflogs, remotes, merge base, branch tracking, and pinned `origin/master` in `specs/113-ndn-svs-experimental-convergence/evidence/pre-migration.md`
- [x] T002 Record `../ndn-svs` porcelain status, binary-safe tracked diff digest, changed paths, and the selected ignored `docs/high-loss-repair-design.md` digest in `specs/113-ndn-svs-experimental-convergence/evidence/pre-migration.md`
- [x] T003 Scan the proposed snapshot paths for credential/auth material and record exclusions in `specs/113-ndn-svs-experimental-convergence/evidence/pre-migration.md`
- [x] T004 Validate Context Mode, synchronized CodeGraph, Spec Kit prerequisites, and GSD health; record tool/fallback status in `specs/113-ndn-svs-experimental-convergence/evidence/tool-gates.md`
- [x] T005 [P] Create requirement-to-task-to-test/evidence mapping in `specs/113-ndn-svs-experimental-convergence/traceability.md`

---

## Phase 2: Foundational Recovery And Selective Replay

**Purpose**: Establish a permanent recovery point and a conflict-isolated convergence branch.

**Critical**: No reset, rebase, or branch-force operation occurs before T006-T007 pass.

- [x] T006 Create `backup/experimental-before-convergence-20260715` from current `../ndn-svs` Experimental and commit the complete tracked WIP plus explicitly selected ignored design artifacts as a clearly labeled snapshot
- [x] T007 Verify the backup snapshot parent, included path list, status/diff hashes, and recovery commands against T001-T003 in `specs/113-ndn-svs-experimental-convergence/evidence/backup-verification.md`
- [x] T008 Create `review/ndn-svs-convergence` from the backup snapshot and perform selective `--onto` replay after `0521665` onto the pinned `origin/master`, without moving the backup or remote refs
- [x] T009 Resolve replay conflicts in `../ndn-svs/ndn-svs/mapping-provider.cpp`, `security-options.cpp/.hpp`, `svspubsub.cpp/.hpp`, and tests by retaining reviewed remote implementations unless a Spec 113 requirement proves a needed delta
- [x] T010 Compare the reconstructed tree with the backup snapshot, classify every dropped/retained hunk, and record range-diff/cherry evidence in `specs/113-ndn-svs-experimental-convergence/evidence/replay-classification.md`
- [x] T011 Convert the replayed WIP into an uncommitted review surface on `review/ndn-svs-convergence` while retaining the immutable backup, then verify no content loss in `specs/113-ndn-svs-experimental-convergence/evidence/replay-classification.md`

**Checkpoint**: The backup is permanent; all remaining edits are based on pinned remote master and can be safely split.

---

## Phase 3: User Story 1 - Recoverable Original State (Priority: P1) MVP

**Goal**: Prove that the original Experimental history and worktree remain recoverable.

**Independent Test**: Reconstruct original HEAD plus WIP in a disposable worktree and compare manifest hashes without changing active branches.

- [x] T012 [US1] Create a disposable backup worktree, verify original HEAD ancestry and all snapshot/artifact hashes, then remove it and record the recovery drill in `specs/113-ndn-svs-experimental-convergence/evidence/backup-verification.md`
- [x] T013 [US1] Confirm the backup branch is excluded from every later rebase/reset/delete operation and document its permanent retention contract in `specs/113-ndn-svs-experimental-convergence/evidence/backup-verification.md`

**Checkpoint**: Destructive history work may continue because complete recovery has been executed, not merely described.

---

## Phase 4: User Story 2 - Clean Reviewed Baseline (Priority: P1)

**Goal**: Make the future Experimental delta contain no duplicate reviewed history or local-only workflow configuration.

**Independent Test**: Inspect the diff against pinned `origin/master`; every hunk maps to a retained concern and upstream InterestSigner remains authoritative.

- [x] T014 [US2] Classify old Experimental commits with `git cherry`/`range-diff` and prove reviewed-equivalent commits are absent from the new delta in `specs/113-ndn-svs-experimental-convergence/evidence/replay-classification.md`
- [x] T015 [US2] Remove agent/spec/docs/result ignore rules from the product diff in `../ndn-svs/.gitignore` and retain them only through local exclude/context configuration
- [x] T016 [US2] Verify `../ndn-svs/ndn-svs/security-options.cpp` and `.hpp` retain the reviewed ndn-cxx InterestSigner path and remove superseded custom timestamp code/tests from the delta
- [x] T017 [US2] Audit every changed file against mapping, recovery, publication, or build ownership and remove/justify any unrequested hunk in `specs/113-ndn-svs-experimental-convergence/evidence/scope-audit.md`

**Checkpoint**: The review surface is a true delta from remote master.

---

## Phase 5: User Story 3 - Reviewable Concern Commits (Priority: P1)

**Goal**: Stabilize and test mapping, recovery, publication, and build concerns before committing them independently.

**Independent Test**: Each concern passes its focused tests and can be staged without paths owned by another concern.

### Mapping And Duplicate Fetch

- [x] T018 [P] [US3] Add/retain sparse-range mapping and duplicate in-flight/backoff fetch tests in `../ndn-svs/tests/unit-tests/mapping-provider.t.cpp` and `../ndn-svs/tests/unit-tests/svspubsub.t.cpp`
- [x] T019 [US3] Stabilize partial mapping response and duplicate fetch suppression behavior in `../ndn-svs/ndn-svs/mapping-provider.cpp`, `svspubsub.cpp`, and `svspubsub.hpp`
- [x] T020 [US3] Run the focused mapping tests and record exact commands/results in `specs/113-ndn-svs-experimental-convergence/evidence/focused-validation.md`

### Bounded Publication Recovery

- [x] T021 [P] [US3] Add/retain adaptive lifetime, separate-window, redundant piggyback, selective repair, retry/backoff, and deduplication tests in `../ndn-svs/tests/unit-tests/svspubsub.t.cpp`
- [x] T022 [US3] Stabilize bounded fetch/recovery implementation and Repair TLVs in `../ndn-svs/ndn-svs/fetcher.cpp/.hpp`, `mapping-provider.hpp`, `svspubsub.cpp/.hpp`, `svsync-base.cpp/.hpp`, and `tlv.hpp`
- [x] T023 [US3] Run focused recovery tests, retain negative results, and record the outcome in `specs/113-ndn-svs-experimental-convergence/evidence/focused-validation.md`

### Local Build Baseline

- [x] T024 [US3] Restore the fork's Boost 1.71 configure gate without altering upstream-facing policy in `../ndn-svs/wscript`
- [x] T025 [US3] Configure and compile from the reconstructed sources on Boost 1.71 and record compiler/dependency/binary hashes in `specs/113-ndn-svs-experimental-convergence/evidence/build-validation.md`

---

## Phase 6: User Story 4 - Transactional Publication Correctness (Priority: P1)

**Goal**: Close the merge-audit correctness blockers before any final product commit is created.

**Independent Test**: Failure injection proves ordered retry/rollback, Face-thread ownership, enforced DataStore erase, exact wire boundaries, and subsequent provider health.

### Tests First

- [x] T026 [P] [US4] Add a multi-publication commit-stage failure/retry test proving the cursor does not advance, later publications remain blocked, stored state is reclaimed on shutdown, and recovery commits in order in `../ndn-svs/tests/unit-tests/svspubsub.t.cpp`
- [x] T027 [P] [US4] Add event-loop ownership, optional active-`Face::put` failure, and post-version-vector Sync-send containment tests in `../ndn-svs/tests/unit-tests/svspubsub.t.cpp` and `core.t.cpp`
- [x] T028 [P] [US4] Add DataStore rollback contract coverage for unsupported single-packet rejection, partial insert, and exact-name erase in `../ndn-svs/tests/unit-tests/svspubsub.t.cpp`
- [x] T029 [P] [US4] Replace predicate-only limits with actual signed outer Data constructions at limit-1, limit, and limit+1 in `../ndn-svs/tests/unit-tests/svspubsub.t.cpp`
- [x] T030 [P] [US4] Add Fetcher destruction tests covering late Data, Nack, timeout, validation-success/failure, and scheduled retry callbacks in `../ndn-svs/tests/unit-tests/svspubsub.t.cpp`

### Implementation

- [x] T031 [US4] Change prepared-publication commit bookkeeping so `m_nextAsyncCommitSeq` advances only after successful advertisement and failed head transactions retry without losing later state in `../ndn-svs/ndn-svs/svspubsub.cpp` and `.hpp`
- [x] T032 [US4] Move all Face emission/advertisement work to the Face event loop, make active first-packet emission non-fatal fallback, contain Sync-send exceptions after local version-vector commit, and align `publishAsync` documentation in `../ndn-svs/ndn-svs/svspubsub.cpp/.hpp`, `svsync-base.cpp/.hpp`, and `core.cpp/.hpp`
- [x] T033 [US4] Add a source-compatible rollback-capability query, reject every unsupported asynchronous transaction before insertion including the single-packet case, and update built-in/test stores in `../ndn-svs/ndn-svs/store.hpp`, `store-memory.hpp`, and `../ndn-svs/tests/unit-tests/svspubsub.t.cpp`
- [x] T034 [US4] Make raw Face callbacks and deferred validation/retry callbacks lifetime-safe for Data, Nack, timeout, and shutdown in `../ndn-svs/ndn-svs/fetcher.cpp` and `.hpp`
- [x] T035 [US4] Ensure segment fitting and receiver assembly satisfy actual final signed packet limits and deterministic cleanup in `../ndn-svs/ndn-svs/svspubsub.cpp` and `.hpp`
- [x] T036 [US4] Run all new T026-T030 tests, fixing the current failing test before moving to the next, and record results in `specs/113-ndn-svs-experimental-convergence/evidence/focused-validation.md`
- [x] T037 [US4] Run the complete rebuilt `../ndn-svs/build/unit-tests` suite and require 100% pass in `specs/113-ndn-svs-experimental-convergence/evidence/build-validation.md`

**Checkpoint**: Product behavior is merge-ready before history is finalized.

---

## Phase 7: Commit Reconstruction And Per-Commit Review

**Purpose**: Produce the final linear review sequence from the tested tree.

- [x] T038 Commit stable sparse mapping/duplicate-fetch behavior with its tests as one review concern in `../ndn-svs`
- [x] T039 Commit bounded publication recovery/repair with its tests as one independent review concern in `../ndn-svs`
- [x] T040 Commit transactional segmented publication, enforced rollback, and callback lifetime with its tests as one independent review concern in `../ndn-svs`
- [x] T041 Commit Boost 1.71 fork baseline as one independent build concern in `../ndn-svs`
- [x] T042 Inspect every final commit for one-purpose scope, subject/body clarity, revert effect, `git diff --check`, and absence of WIP/agent artifacts; record OIDs in `specs/113-ndn-svs-experimental-convergence/evidence/commit-review.md`
- [x] T043 Rebuild and rerun the complete unit suite from final committed HEAD to prove no staged/untracked product source was used in `specs/113-ndn-svs-experimental-convergence/evidence/commit-review.md`

---

## Phase 8: User Story 5 - Fresh Cross-Repository Evidence (Priority: P2)

**Goal**: Bind final committed sources to fresh NDNSF/MiniNDN evidence.

**Independent Test**: A new candidate passes focused cross-repository tests and six immutable MiniNDN cells with the Spec 112 acceptance counts.

- [x] T044 [US5] Install or link the final committed NDN-SVS library, rebuild affected NDNSF binaries/extensions, and record binary/source identities in `specs/113-ndn-svs-experimental-convergence/evidence/integration-validation.md`
- [x] T045 [US5] Run focused C++ Targeted, Python Targeted, Targeted-timeout, and segmented-response tests from `quickstart.md`; record skips/failures honestly in `specs/113-ndn-svs-experimental-convergence/evidence/integration-validation.md`
- [x] T046 [US5] Extend `Experiments/spec112_candidate_manifest.py` to bind final master/Experimental/backup topology and create a new immutable candidate at `results/spec112-segmented/<candidate>/candidate-manifest.json`
- [x] T047 [US5] Run the four 0% Normal/Targeted × synchronous/asynchronous boundary cells exactly once under the T046 candidate with `Experiments/NDNSF_Segmented_Response_Minindn.py`
- [x] T048 [US5] Run the same-epoch 80×8-KB plus 10×64-B plus 12×4-KB burst cell exactly once under the T046 candidate with `Experiments/NDNSF_Segmented_Response_Minindn.py`
- [x] T049 [US5] Run the degraded-provider Targeted timeout cell exactly once under the T046 candidate with `Experiments/NDNSF_Segmented_Response_Minindn.py`
- [x] T050 [US5] Validate the candidate manifest, 6/6 cell summaries, acceptance counts, provider liveness, packet limits, callbacks, and unchanged source identity in `specs/113-ndn-svs-experimental-convergence/evidence/minindn-validation.md`

---

## Phase 9: Final Local Topology And Closeout

**Purpose**: Move only local branch labels after all product/evidence gates pass.

- [x] T051 Verify the pinned `origin/master` and remote refs still match T001, then move local `master` to `origin/master` without checking out or modifying the dirty NDNSF root repository
- [x] T052 Move local `Experimental` to the validated convergence HEAD, retain the permanent backup and review branch, and document an explicit no-upstream disposition without changing remote Experimental
- [x] T053 Prove `master == origin/master`, `origin/master` is an ancestor of Experimental, backup resolves to the original snapshot, Experimental is clean, and no conflict markers remain in `specs/113-ndn-svs-experimental-convergence/evidence/final-topology.md`
- [x] T054 Update `specs/113-ndn-svs-experimental-convergence/traceability.md` and create `completion-summary.md` with exact commit IDs, test counts, candidate paths, evidence limits, and remote-safety result
- [x] T055 Run strict Spec Kit structure, prerequisites, placeholder/coverage checks, `git diff --check`, per-commit scope audit, and post-implementation code-aware audit; record the verdict in `specs/113-ndn-svs-experimental-convergence/evidence/final-audit.md`
- [x] T056 Run Spec Kit convergence; if it appends tasks, execute them in order and repeat implementation/audit/convergence until no task remains and no Critical/High finding exists

---

## Dependencies & Execution Order

```text
Setup T001-T005
  -> Recovery/replay T006-T011
      -> US1 recovery proof T012-T013
          -> US2 clean baseline T014-T017
              -> US3 concern stabilization T018-T025
                  -> US4 correctness T026-T037
                      -> commit reconstruction T038-T043
                          -> US5 fresh evidence T044-T050
                              -> topology/closeout T051-T056
```

- T018 and T021 initially touch distinct test sections but implementation in
  `svspubsub` remains sequential.
- T026-T030 are parallel test-authoring opportunities; their implementations
  T031-T035 are sequential because they share transaction/lifetime state.
- No final ref move occurs before fresh candidate validation.
- A failed focused test is repaired and its sequence continues; the complete
  suite is rerun once after all focused tests have been exercised.

## Implementation Strategy

1. Make recovery executable before changing history.
2. Reconstruct only the true delta from reviewed remote master.
3. Write failure tests, then correct transaction/lifetime semantics.
4. Build one-purpose commits from the passing tree.
5. Rebuild and generate fresh evidence from committed identities.
6. Move local labels last; never push.

## Completion Definition

- All T001-T056 and any appended convergence tasks are `[x]`.
- Permanent backup verified; local master equals unchanged origin/master.
- Experimental is clean, based on origin/master, and contains only reviewed
  concern commits.
- Unit, focused integration, and fresh MiniNDN candidate gates pass.
- Final audit has no Critical/High finding and convergence appends nothing.
