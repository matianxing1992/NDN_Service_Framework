# Spec 177 Requirement Traceability

This matrix maps each requirement and success criterion to cohesive implementation tasks and ordered validation gates. Unchecked tasks are planned work and do not claim implementation or passing evidence.

## Validation Gates

- **G1**: C++/Python unit and contract tests
- **G2**: deterministic CPU integration with fake adapter
- **G3**: real MVCNN-style model on the generated fixture
- **G4**: real multi-process MiniNDN
- **G5**: registered controlled-dataset evaluation
- **G6**: final code-aware audit and release gate

| Requirement | Owning tasks | Gates |
| --- | --- | --- |
| FR-001 | T002, T004 | G1-G2 |
| FR-002 | T002, T004 | G1-G2 |
| FR-003 | T002-T004, T009 | G1-G2, G4 |
| FR-004 | T004, T009-T010 | G1-G2, G4 |
| FR-005 | T003-T004, T009-T010 | G1-G2, G4 |
| FR-006 | T003, T005 | G2-G3 |
| FR-007 | T002-T003, T005-T006 | G1-G3 |
| FR-008 | T006, T010 | G1-G4 |
| FR-009 | T002, T004, T006, T010 | G1-G4 |
| FR-010 | T004, T009-T010 | G2, G4 |
| FR-011 | T004, T009-T010 | G1-G4 |
| FR-012 | T003-T006, T010-T011 | G1-G5 |
| FR-013 | T001, T007 | G1, G3 |
| FR-014 | T001, T008, T011 | G1, G5 |
| FR-015 | T005, T008, T011 | G3, G5 |
| FR-016 | T001-T013 dependency order | G1-G6 |
| FR-017 | T003-T004, T009, T013 | G1-G2, G6 |
| SC-001 | T005-T007 | G3 |
| SC-002 | T002-T004, T006, T009-T010 | G1-G4 |
| SC-003 | T003, T005, T007 | G2-G3 |
| SC-004 | T004, T006, T010 | G4 |
| SC-005 | T008, T011 | G5 |
| SC-006 | T001, T007-T008, T010-T013 | G3-G6 |

## User Story Coverage

| Story | Tasks | Independent acceptance |
| --- | --- | --- |
| US1 joint recognition | T004-T005 | 2-6 views are verified and jointly pooled; view removal/replacement changes fusion evidence |
| US2 annotated outputs | T006 | every contributing view has one valid Provider-owned annotation |
| US3 no-camera demonstration | T007-T008 | generated fixture passes functional gate; controlled data alone supports quantitative claims |
| US4 security and delivery | T009-T010 | negative security/failure matrix and multi-process MiniNDN pass with one terminal owner |

## Current Evidence Boundary

As of 2026-08-29, T001-T013 are implemented on the `UAV-Experimental`
working tree.  The C++ multi-view unit gate (4/4), C++ CPU integration gate
(3/3), and Python contract/model/output/security/evaluation/MiniNDN contract
gates (18 passed) pass.  The six-image fixture and registered controlled
campaign verify bounded 1/2/4/6-view execution, exact output provenance, and
annotation completeness; the one-sample generated registration explicitly does
not support an accuracy claim.

The real MiniNDN matrix ran all five scenarios.  Nominal and
Provider-selection completed with six verified UAV Data packets, six
Provider-owned annotations, and one terminal result owner.  Unavailable-view,
late-view, and publication-failure reached explicit rejected terminal stages.
The retained matrix evidence is
`evidence/minindn-matrix-20260829.md`.  The controlled campaign evidence is
`evidence/controlled-evaluation.md`; its paired-bootstrap status is
`insufficient-samples` because the registration has one sample.

The full repository C++ gate still has two unrelated baseline failures
(`NativeTensorBundleCodec...` divide-by-zero and
`ProductionProviderContextUsesSvsSegments`), which are recorded rather than
silently attributed to Spec177.  The Spec177-specific release gate is PASS
with the scientific-claim limitation above.
