# Spec 113 Traceability

Status vocabulary: `planned`, `implemented`, `executed-pass`, `executed-fail`.
No row may be promoted from implemented to executed without the named evidence.

## Functional Requirements

| Requirement | Tasks | Closing test/evidence | Final status |
|---|---|---|---|
| FR-001 immutable recovery point | T001-T007, T012 | `evidence/pre-migration.md`, backup recovery drill | executed-pass |
| FR-002 no stash-only recovery | T006-T013 | permanent backup branch and snapshot commit | executed-pass |
| FR-003 local master equals pinned remote master | T051, T053 | final topology OID equality | executed-pass |
| FR-004 Experimental is clean true delta | T008-T017, T038-T043, T052-T053 | range-diff, commit review, ancestry | executed-pass |
| FR-005 no remote mutation | T001, T008, T051-T053 | before/after remote ref comparison | executed-pass |
| FR-006 permanent old-state backup | T006-T013, T052-T053 | backup ref and disposable recovery worktree | executed-pass |
| FR-007 distinct concern ownership | T017-T043 | per-concern focused tests and commits | executed-pass |
| FR-008 exclude local workflow rules | T003, T015, T017, T042 | scope audit and final commit path lists | executed-pass |
| FR-009 retain reviewed remote implementations | T009-T010, T014, T016 | replay classification and InterestSigner inspection | executed-pass |
| FR-010 failed commit cannot advance | T026, T031, T036-T037 | multi-publication commit failure/retry test | executed-pass |
| FR-011 async contract/thread ownership agree | T027, T032, T036, T044-T045 | thread-owner/Face fallback and integration tests | executed-pass |
| FR-012 enforce DataStore rollback | T028, T033, T036-T037 | partial-insert/exact-erase test and full suite | executed-pass |
| FR-013 actual final packet boundaries | T029, T035-T037 | signed outer limit-1/limit/limit+1 test | executed-pass |
| FR-014 Boost 1.71 local build | T024-T025, T043 | clean configure/build and artifact hashes | executed-pass |
| FR-015 complete rebuilt NDN-SVS tests | T025, T036-T037, T043 | complete unit-suite result | executed-pass |
| FR-016 cross-repository NDNSF behavior | T044-T045 | focused C++/Python tests | executed-pass |
| FR-017 final candidate identity | T046, T050 | immutable manifest and post-cell identity check | executed-pass |
| FR-018 final cells run once | T046-T050 | unique cell directories and summaries | executed-pass |
| FR-019 audit and convergence closure | T054-T056 | final audit and no appended work | executed-pass |
| FR-020 clean branch/no conflicts/upstream disposition | T042-T043, T052-T053 | status, conflict search, branch tracking report | executed-pass |

## Success Criteria

| Criterion | Tasks | Acceptance evidence | Final status |
|---|---|---|---|
| SC-001 100% backup accounting | T001-T013 | manifest/recovery hash comparison | executed-pass |
| SC-002 final local/remote-safe topology | T051-T053 | exact OIDs and remote comparison | executed-pass |
| SC-003 zero duplicate reviewed commits | T008-T010, T014, T042 | cherry/range-diff classification | executed-pass |
| SC-004 one concern per commit | T038-T043 | commit review matrix | executed-pass |
| SC-005 zero gap/leak after commit failure | T026, T031-T037 | failure/retry/shutdown assertions | executed-pass |
| SC-006 actual signed boundary proof | T029, T035-T037 | observed final outer wire sizes | executed-pass |
| SC-007 Boost build and full unit pass | T024-T025, T037, T043 | build log and total pass count | executed-pass |
| SC-008 fresh MiniNDN candidate passes | T044-T050 | six immutable cells and aggregate | executed-pass |
| SC-009 no Critical/High or remaining task | T054-T056 | final audit/convergence | executed-pass |

## Source Intent To Evidence Chain

```text
User-approved final topology
  -> US1/US2 and FR-001..FR-009
  -> branch-topology/review-commit contracts
  -> T001..T017, T038..T043, T051..T053
  -> backup/replay/commit/topology evidence

Merge-readiness audit findings
  -> US3/US4 and FR-010..FR-016
  -> publication-transaction contract
  -> T018..T045
  -> focused/full/cross-repository tests

Source identity changes after rebase
  -> US5 and FR-017..FR-020
  -> validation-evidence contract
  -> T046..T056
  -> immutable candidate, final audit, convergence
```
