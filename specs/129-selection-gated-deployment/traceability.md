# Traceability: Spec 129 R1

R0 evidence is migration context only. T001--T010 implementation has focused
unit/integration coverage. The single formal T011 campaign is
`results/spec129-r1-20260721_183058`: 12/12 frozen cells accepted, each invoked
once, with no selective rerun and unchanged Spec 128 hashes.

## Source Intent Coverage

| Source decision | Requirement path | Task path |
|---|---|---|
| Only `DIReservationSelectionV1` positive ACK means reserved capacity | FR-002..FR-006, FR-027 | T001, T002, T009 |
| One ACK timeout; late positive ACK gets negative decision | FR-007..FR-010 | T003 |
| Selection separately targets every positive-ACK Provider | FR-010..FR-014 | T003, T005 |
| Exact assignment receiver-confidential | FR-015..FR-017 | T001, T004 |
| Independent `SelectionGatedInputV1`: non-reserving ACK key offer, encrypted REQUEST input, selected authorized recipient grant, Targeted incompatibility preflight | FR-027, FR-033 | T001, T004, T009, T010, T011 |
| No complete READY barrier; direct dependencies trigger stages | FR-018..FR-021 | T006 |
| Lease plus randomized bounded retry, probabilistic liveness | FR-022 | T007 |
| Most resource policy remains in NDNSF-DI | FR-026 | T001..T009 |

## Functional Requirement Coverage

| Requirement | Story | Tasks | Planned evidence |
|---|---|---|---|
| FR-001 | US1 | T001, T002 | intent bounds and request tests |
| FR-002 | US1 | T002 | capability-gated reserve-before-positive, authorization-first, and zero-negative tests |
| FR-003 | US1 | T001, T002 | lease codec/signature/binding |
| FR-004 | US1 | T002 | duplicate concurrency and no-extension tests |
| FR-005 | US1 | T002 | requester/service/global quota tests |
| FR-006 | US1/US2 | T002, T005 | pre-expiry commit, bounded committed lease, all release causes and orphan horizon |
| FR-007 | US2 | T003 | timeout close state machine |
| FR-008 | US2 | T003 | validation-complete eligibility race |
| FR-009 | US2 | T001, T003 | tombstone retention/late ACK |
| FR-010 | US2 | T003, T005 | one decision per positive reservation |
| FR-011 | US2 | T001, T003 | enum/version strict decode |
| FR-012 | US2 | T001, T003 | exact binding and signature negatives |
| FR-013 | US2 | T003, T005 | immutable first decision, duplicate/reorder/conflict/old-attempt safety |
| FR-014 | US2 | T003, T005 | receipt/retry/lost-decision expiry |
| FR-015 | US2 | T001, T004 | plan/assignment commitment |
| FR-016 | US2 | T001, T004 | recipient hybrid crypto/AAD negatives |
| FR-017 | US2 | T004 | minimum projection disclosure scan |
| FR-018 | US3 | T002, T005, T006 | prepare and hard-pin lifecycle |
| FR-019 | US3 | T006 | source/direct-dependency eligibility |
| FR-020 | US3 | T001, T006 | stage evidence replay/binding tests |
| FR-021 | US3 | T001, T006 | abort and partial-result rejection |
| FR-022 | US3 | T007 | release receipt-or-expiry before full-jitter retry and exhaustion metrics |
| FR-023 | US4 | T008 | zero unsolicited transition traffic |
| FR-024 | US4 | T004, T008 | status signature/encryption/binding |
| FR-025 | US4 | T008 | polling/cursor/terminal-stop tests |
| FR-026 | US5 | T001..T009 | ownership/import/static audit |
| FR-027 | US5 | T001, T002, T003, T004, T009, T011 | ordinary positive-ACK/no-reservation and generic security compatibility gates |
| FR-028 | US5 | T001, T004, T008, T009 | C++/Python malformed/parity tests |
| FR-029 | US3/US5 | T001, T003, T005, T006, T009 | R0 authority migration/deletion scans |
| FR-030 | US5 | T010, T011 | deterministic and frozen matrix coverage |
| FR-031 | US5 | T010, T011 | single-writer exact-once attribution |
| FR-032 | US5 | T010, T011 | Spec 128 before/after hashes |
| FR-033 | US2/US5 | T001, T004, T010, T011 | four combinations, input-only no-DI codec, key offer/grant, plaintext/key-lifetime and recipient negatives |

## Success Criteria Coverage

| Criterion | Tasks | Planned evidence |
|---|---|---|
| SC-001 | T002, T010, T011 | ACK-to-reservation cardinality |
| SC-002 | T002, T003, T005, T010, T011 | terminal release ledger/orphan scan |
| SC-003 | T003, T010, T011 | eligible/late decision counts |
| SC-004 | T001, T003..T006, T008, T011 | full negative security/binding suite |
| SC-005 | T004, T010, T011 | cross-recipient and plaintext assignment scan |
| SC-006 | T006, T010, T011 | stage eligibility/overlap/zero activation |
| SC-007 | T005, T006, T010, T011 | failure/abort/release/final-result evidence |
| SC-008 | T007, T010, T011 | collision/backoff/exhaustion bounds |
| SC-009 | T008, T010, T011 | pull counts/status scan/cursor results |
| SC-010 | T009, T011 | existing generic/security regressions |
| SC-011 | T001, T008, T009, T011 | C++/Python parity matrix |
| SC-012 | T010, T011 | twelve-cell aggregate and hash guards |
| SC-013 | T001, T004, T010, T011 | input plaintext/key absence and authorized-role decrypt |

## Fresh Matrix Mapping

The exact twelve frozen IDs in `plan.md` and
`Experiments/run_spec129_selection_gated_deployment_matrix.py` cover FR-002,
FR-006..FR-022 and FR-030..FR-033.
Deterministic suites cover exhaustive malformed/security/compatibility cases;
the live matrix demonstrates the actual NDNSF/SVS/MiniNDN path. Cell results
must be linked here only after execution and may not be selectively replaced.

Formal result: `results/spec129-r1-20260721_183058/campaign-summary.json`.
The aggregate records 12 requests, 36 ACKs, 33 reservations, 11 selected and 22
not-selected decisions, 33 receipts, 33 releases, six retries, one exhaustion,
two status snapshots, 15 ms dependency overlap, zero duplicate execution, and
zero packet plaintext matches.
