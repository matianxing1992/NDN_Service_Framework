# Traceability: Complete SVS V3 Review Commit

| Requirement | Design authority | Tasks | Success criteria |
|---|---|---|---|
| FR-001 | `contracts/ref-publication.md`, `RewriteBaseline`, `SafetyRef` | T001, T008, T011, T016 | SC-007, SC-009 |
| FR-002 | Remote fence in `contracts/ref-publication.md` | T001, T008, T011 | SC-005, SC-007 |
| FR-003 | Required owner in `contracts/commit-composition.md` | T002-T004, T011, T017, T020 | SC-001, SC-006 |
| FR-004 | Direct tests in `contracts/commit-composition.md` | T002-T005, T011, T013, T017-T018, T021 | SC-001, SC-002 |
| FR-005 | Required production behavior in `contracts/commit-composition.md` | T002-T005, T011 | SC-001, SC-002 |
| FR-006 | V2 isolation in `contracts/commit-composition.md` | T003, T005, T011 | SC-002 |
| FR-007 | Explicit exclusions in `contracts/commit-composition.md` | T002, T003, T006, T011, T019-T020 | SC-001, SC-003 |
| FR-008 | Independent harness boundary in Research Decision 2 | T002, T005-T006, T011-T013, T017, T021, T023-T025 | SC-001, SC-004, SC-010 |
| FR-009 | Machine-checkable composition invariant 4 | T002, T004, T011, T017, T020 | SC-001 |
| FR-010 | Experimental replay design in `plan.md` | T006-T007, T011, T019, T024 | SC-003, SC-004, SC-010 |
| FR-011 | `RewriteCandidate` tree preservation invariant | T006, T011, T024 | SC-003, SC-010 |
| FR-012 | `contracts/validation-evidence.md` Gate B | T003, T005, T011, T018, T021, T025 | SC-002 |
| FR-013 | `contracts/validation-evidence.md` Gate D | T007, T011, T014, T019, T021-T023, T025 | SC-004, SC-010 |
| FR-014 | Candidate-bound evidence rule | T005, T007, T011, T014, T018, T021-T023, T025 | SC-002, SC-004, SC-010 |
| FR-015 | Publication order in `contracts/ref-publication.md` | T008-T009, T011 | SC-007 |
| FR-016 | Explicit lease constraint in `contracts/ref-publication.md` | T002, T009, T011 | SC-005, SC-007 |
| FR-017 | Completion invariant `master == origin/master` | T009, T011 | SC-005 |
| FR-018 | Three-ref ancestry completion invariant | T009, T011, T020, T024 | SC-006, SC-010 |
| FR-019 | PR metadata decision and publication contract | T010-T011, T015, T019, T022, T025 | SC-008 |
| FR-020 | Rollback contract | T008-T011 | SC-007, SC-008 |
| FR-021 | Scope boundary in `plan.md` | T001, T003-T004, T006, T009-T011, T015-T017, T020, T022, T024 | SC-003, SC-005, SC-009, SC-010 |
| FR-022 | Isolated worktree architecture | T001, T003-T004, T011, T016 | SC-003, SC-009 |
| FR-023 | Publication authorization precondition | T008-T011, T015, T019-T020, T022, T024-T025 | SC-005, SC-008 |
| FR-024 | Evidence and Completion Semantics in `plan.md` | T001-T025 | SC-002-SC-010 |
| FR-025 | External interoperability ownership in `plan.md` | T023-T025 | SC-004, SC-010 |
| FR-026 | Nine-owner sequence in `contracts/history-consolidation.md` | T036-T038 | SC-011-SC-013 |
| FR-027 | Corrective-owner rule in `contracts/history-consolidation.md` | T036-T038 | SC-011-SC-012 |
| FR-028 | Separate publication/recovery state-machine owners in `contracts/history-consolidation.md` | T036-T038 | SC-011-SC-012 |
| FR-029 | Post-construction exact-OID validation in `plan.md` | T038 | SC-012 |
| FR-030 | Frozen-tree, synchronized-local-ref, and no-remote-mutation invariant | T036-T038 | SC-013 |
| FR-031 | Tracked NDN-SVS Boost 1.74 policy in `contracts/history-consolidation.md` | T038 | SC-011, SC-014 |
| FR-032 | Disposable `compileTMP` validation contract in `quickstart.md` | T038 | SC-012, SC-014 |
| FR-033 | Focused three-owner range in `spec.md` Phase 15 | T040-T043 | SC-015, SC-017 |
| FR-034 | Descendant continuation boundary in `plan.md` | T042-T043 | SC-015, SC-018 |
| FR-035 | Corrected SVS-PS V3 wire and naming contract in `spec.md` | T041, T043 | SC-017 |
| FR-036 | Six-owner continuation replay in `plan.md` | T042-T043 | SC-018 |
| FR-037 | Dirty-checkout preservation and immutable safety refs | T039, T043 | SC-018 |
| FR-038 | Code, direct tests, and fixtures only in NDN-SVS | T040-T041, T043 | SC-016-SC-017 |
| FR-039 | Signed V3 Content extension boundary and V2 isolation | T041-T043 | SC-017-SC-018 |
| FR-040 | Local-only rewrite with no remote, tag, or PR mutation | T039, T042-T043 | SC-018 |

## User Story Coverage

| Story | Requirements | Tasks | Independent evidence |
|---|---|---|---|
| US1 | FR-003-FR-009, FR-012, FR-014, FR-025-FR-035, FR-038-FR-039 | T003-T005, T017-T018, T020-T031, T035-T043 | `evidence/isolated-v3-validation.md`, `evidence/focused-v3-rewrite-20260807.md`, ownership manifest, exact-OID units, clean NDN-SVS scan, NDNSF-owned standalone interop |
| US2 | FR-007-FR-014, FR-024-FR-040 | T006-T007, T019-T031, T035-T043 | focused/continuation ancestry, exact-candidate unit/interop evidence, original dirty-checkout hashes |
| US3 | FR-001-FR-002, FR-015-FR-024, FR-037, FR-040 | T008-T010, T016, T019-T020, T022, T024-T025, T039, T043 | immutable local safety refs and publication deferral; remote and PR state remain unchanged until user authorization |

## Controlling Intent

The 2026-08-07 user override requires a local, unpushed three-commit PR branch
containing regex/name-only PubSub, V2 InterestSigner, and corrected SVS V3.
Mapping, parallelism, segmented publication, and Fetcher/Repair remain on a
six-commit descendant continuation branch. The dirty `Experimental` checkout,
remote refs, tags, and PR metadata must not move until the user inspects and
publishes the focused branch. FR-033-FR-040, SC-015-SC-018, and T039-T043 are
the controlling end-to-end chain for this rewrite.
