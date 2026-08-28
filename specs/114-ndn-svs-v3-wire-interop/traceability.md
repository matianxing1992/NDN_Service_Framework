# Spec 114 Final Traceability

**Status**: Final-audit candidate as of 2026-07-16. All FR-001..FR-026 and
SC-001..SC-011 retain an implementation task and executed evidence. T030-T032
close the post-implementation validation-order, extension-atomicity, and
candidate-supersession findings; T029 is checked only after the final audit.

## Functional Requirements

| Requirement | Tasks | Test/evidence |
|---|---|---|
| FR-001 explicit profile/default V3 | T005, T015-T017 | public construction and NDNSF mode tests |
| FR-002 V3 names | T001, T006-T007 | fixed vectors and captured packet names |
| FR-003 signed Data envelope | T001, T006-T008 | fixed Data-wrapped packets and receive tests |
| FR-004 signer/validator target | T006-T007, T011, T018, T023, T030 | validation-before-semantic-decode tests and HMAC interop |
| FR-005 exact V3 route/envelope | T006, T008, T011, T016 | wrong-version/name negative vectors |
| FR-006 V3 timers/suppression | T005, T009, T015, T023 | timer bounds, profile defaults, captured lifetime |
| FR-007 immediate publication/batching | T009 | scheduler and batching diagnostics |
| FR-008 TLVs/canonical order | T004, T007, T012 | independent exact vectors |
| FR-009 positive encoded sequence | T004, T012 | SeqNo-zero rejection vectors |
| FR-010 atomic future-vector rejection | T004, T012-T014 | boundaries, state digest, next-valid progress |
| FR-011 bootstrap reuse/current fallback | T005, T014, T018 | construction/restart tests |
| FR-012 bootstrap-aware Data names | T010 | multi-epoch publish/fetch tests |
| FR-013 serial/parallel equivalence | T001, T006, T008, T010, T013, T031 | byte/state/error/extension-order parity tests |
| FR-014 explicit isolated V2 | T005, T015-T018 | fixed V2 bytes and rollback tests |
| FR-015 no downgrade/observable profiles | T008, T014, T016-T017, T024 | injection and harness diagnostics |
| FR-016 extension profile after Data | T019-T020 | packet order and core-only tests |
| FR-017 malformed/unknown extension safety | T004, T019-T020, T031 | block and collection prepare/commit tests |
| FR-018 no whole-envelope V3 LZMA | T021 | configuration rejection and V2 retention |
| FR-019 independent packet tests | T004-T007, T015 | fixed bytes independent of codec |
| FR-020 bidirectional/negative/interop | T002, T004, T011, T022-T025 | C++/NDNts both directions and vectors |
| FR-021 candidate-bound MiniNDN evidence | T001-T003, T006, T016, T022-T026, T028, T032 | final manifest and six immutable cells plus preserved invalid setup attempts |
| FR-022 affected NDNSF regression | T017-T018, T027 | installed identity and Spec 112/113 tests |
| FR-023 preserve Spec 113 concerns | T001, T020-T021, T028 | ancestry, reliability regressions, commit review |
| FR-024 audit/convergence closure | T003, T006, T028-T032 | strict scan, audit fixes, superseding evidence, final audit |
| FR-025 no Sync Ack | T009-T010, T022, T024-T026 | counters and packet samples |
| FR-026 consumer profile defaults | T017-T018, T027 | C++ construction and GUI environment tests |

## Success Criteria

| Criterion | Tasks | Evidence |
|---|---|---|
| SC-001 normative vectors 100% | T004, T007, T010, T024 | fixed-vector and full-suite evidence |
| SC-002 negative vectors atomic | T004, T011-T014, T024 | state digest and next-valid progress |
| SC-003 120/120 unique 0%-loss sequences | T025-T026 | three immutable 0% cells |
| SC-004 three 5%-loss runs converge | T025-T026 | three immutable 5% cells |
| SC-005 V2 stable/mixed isolated | T015-T016, T018, T024 | V2 bytes and profile diagnosis |
| SC-006 extension isolation | T019-T021, T024 | core/extension digest comparison |
| SC-007 bootstrap/re-bootstrap names | T010, T012, T014, T024 | multi-epoch names and retrieval |
| SC-008 full NDN-SVS and NDNSF pass | T024, T027 | full-build and consumer evidence |
| SC-009 complete traceability/audit | T028-T029 | completion map and final audit |
| SC-010 zero Sync Ack | T009-T010, T024, T026 | emission counters and captures |
| SC-011 NDNSF default propagation | T017-T018, T027 | user/provider and GUI tests |

## Source Intent Chain

```text
reported C++ Experimental vs V3 incompatibility
  -> US1/US2/US3
  -> FR-001..FR-015, FR-019..FR-020, FR-025..FR-026
  -> fixed vectors + correct signed object + explicit version + no Ack
  -> standalone and MiniNDN independent-peer evidence

preserve Spec 113 recovery while becoming V3 compliant
  -> US4/US5
  -> FR-016..FR-018, FR-021..FR-024
  -> extension profile + candidate-bound NDNSF regression
  -> reviewable no-push completion evidence
```

## Executed evidence index

| Evidence | Actual outcome | Covers |
|---|---|---|
| `evidence/baseline.md` | hybrid `/v=2 + raw StateVector` reproduced; rollback ref retained | FR-002, FR-003, FR-013, FR-021, FR-023 |
| `evidence/fixed-vectors.md` | independent V2/V3 fixtures byte-match; malformed set rejected | FR-002-FR-005, FR-008-FR-010, FR-019; SC-001, SC-002 |
| `evidence/focused-validation.md` | 67/67 NDN-SVS units; validation ordering, collection atomicity, profile, extension, timer gates pass | FR-001, FR-004-FR-018, FR-023, FR-025; SC-002, SC-005-SC-007, SC-010 |
| `evidence/standalone-interop.md` | 5/5 C++/NDNts cases, including V2 and mismatch; zero Sync Ack | FR-004, FR-006, FR-015, FR-020, FR-025; SC-001, SC-005, SC-010 |
| `evidence/minindn-validation.md` | 6/6 immutable cells; 120/120 sequences at each loss class; equal final vectors | FR-020, FR-021, FR-025; SC-003, SC-004, SC-010 |
| `evidence/ndnsf-regression.md` | 8/8 consumer cells; 130 responses plus two expected exactly-once timeouts; focused tests pass | FR-001, FR-014, FR-015, FR-022, FR-026; SC-008, SC-011 |
| `evidence/commit-review.md` | five reviewable commits, reverse-order rollback, no merge/push | FR-021, FR-023, FR-024; SC-009 |
| `evidence/final-audit.md` | final strict and code-aware verdict | FR-024; SC-009 |
