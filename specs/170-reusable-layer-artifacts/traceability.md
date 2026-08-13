# Pre-Implementation Traceability: Spec 170

**Purpose**: Make every source intent, hypothesis, normative requirement,
implementation task, blocking gate, and expected evidence discoverable before
implementation. This is the design-time matrix. T039 produces the final
evidence-time matrix without changing these mappings.

## Source Intent to User Story

| Source intent | Story/requirement ownership | Explicit exclusion |
|---|---|---|
| Requester publishes reusable layers; Provider assembles its role | US1, US2; FR-001..FR-025 | Request-specific global role shards are not V3 canonical artifacts |
| Provider may expose zero, one, or multiple GPUs | US4; FR-046..FR-060 | NDNSF-DI does not allocate cluster GPUs |
| One role may span GPUs/Providers; not every pipeline stage must be sharded | US3; FR-026..FR-027, FR-037..FR-044, FR-056..FR-062 | `M_i` is participant count, not equal slicing of every tensor |
| Strategy is replaceable but cannot weaken protocol | US5; FR-016..FR-021, FR-039, FR-063..FR-064 | Network-supplied/adversarial in-process plugins are unsupported |
| ACK separates reuse-only execution willingness from new preparation willingness | US2/US5; FR-020, FR-023, FR-028, FR-053, FR-057, FR-065 | Only three valid signed tuples; negative ACK is never selected; V3 ACK never queues or locks resources |
| Layers and prepared ONNX roles remain locally reusable with meaningful identity | US1/US2/US5; FR-019..FR-021, FR-029 | One bounded assembled bundle handles inline/large ONNX; request IDs and filenames never define equality |
| Protected models must remain reusable and revocable | US6; FR-030..FR-032, FR-069 | Plaintext keys are absent from ACK/strategy/Repo public metadata |
| MiniNDN must catch logic failures before TigerCluster | US4/US6; FR-067, FR-070..FR-071 | Simulated GPUs are not CUDA evidence; they are real lifecycle/network evidence |

## Hypothesis Traceability

| Hypothesis | FR/NFR basis | Success criteria | Implementation tasks | Blocking gate and evidence |
|---|---|---|---|---|
| H1 Resource truth | FR-046, FR-047, FR-048, FR-049, FR-050, FR-052, FR-053, FR-054, FR-058, FR-059, FR-060, FR-067 | SC-014, SC-015, SC-017, SC-019, SC-024, SC-029 | T003, T005, T012, T013, T024, T027 | Gate A/C; D0/D1/D2a/D2b; `gate-c-sif.md`, `tiger-d*.md` |
| H2 CPU and one-GPU compatibility | FR-013, FR-023, FR-025, FR-046..FR-050 | SC-005, SC-014, SC-017, SC-019 | T010, T012, T026, T030, T031 | BASE, Gate B, D0/D1 |
| H3 Multiple independent devices | FR-028, FR-041, FR-051..FR-055, FR-057, FR-065 | SC-007, SC-015, SC-018, SC-020, SC-029 | T005, T011, T013, T026, T032 | BASE, Gate B concurrency, D2a |
| H4 One role across devices/Providers | FR-026, FR-027, FR-037, FR-044, FR-050..FR-059, FR-068 | SC-016, SC-020, SC-021, SC-023, SC-031 | T014-T018, T032, T033 | independent 3A/3B; D2a/D2b |
| H5 Heterogeneous hybrid correctness | FR-027, FR-038, FR-056, FR-061, FR-062, FR-068 | SC-005, SC-025, SC-026, SC-035 | T019, T020, T021, T034 | independent 3C and D2h |
| H6 Data-driven liveness | FR-014, FR-023, FR-024, FR-025, FR-037, FR-065 | SC-006, SC-009 | T005, T006, T021, T023, T026 | Gate B scheduler-reaction trace; 3C/D2h |
| H7 Exact reuse | FR-001..FR-012, FR-018..FR-021, FR-029, FR-040..FR-043 | SC-001..SC-004, SC-010, SC-012, SC-018, SC-030, SC-034 | T007-T011, T026, T035, T036 | US1/US2, Gate B cold/warm, reuse matrix |
| H8 Failure observability/security | FR-030..FR-034, FR-042..FR-045, FR-053, FR-057, FR-069 | SC-008, SC-009, SC-013, SC-017, SC-020, SC-024, SC-031 | T022, T023, T028, T037 | SECURITY, pre-freeze mutation, frozen negative evidence |
| H9 Exact-SIF parity/freeze | FR-046, FR-054, FR-067, FR-070 | SC-011, SC-019, SC-032 | T024, T027, T029-T034 | Gate C, freeze report, D gates |
| H10 Strategy containment | FR-016, FR-017, FR-039, FR-063, FR-064, FR-066 | SC-027, SC-028, SC-030, SC-033 | T002, T004, T010, T025, T028 | F1, Gate A, pre-freeze closure |

## Functional Requirement to Task/Gate Mapping

| Requirements | Implemented/validated by | Primary blocking evidence |
|---|---|---|
| FR-001, FR-002, FR-003, FR-004, FR-005 | T007, T008 | US1 canonical identity tests; Gate A |
| FR-006, FR-007, FR-008, FR-009 | T007, T008 | namespace/layer/tensor index tests; Gate A |
| FR-010, FR-011, FR-012 | T007, T008 | idempotent root-last publication and duplicate-byte evidence |
| FR-013, FR-014, FR-015 | T005, T006, T010 | normal default integration; Gate B lifecycle trace |
| FR-016, FR-017 | T004, T011 | sealer mutation suite; reuse strategy matrix |
| FR-018, FR-019, FR-020, FR-021 | T009, T011 | Provider assembly/residency/reuse tests |
| FR-022 | T002, T010 | explicit V2/V3 dispatch and no-fallback tests |
| FR-023, FR-024, FR-025 | T005, T006, T009, T021, T023, T026 | queue/preparation/admission/data-driven Gate B trace |
| FR-026, FR-027 | T014-T021 | independent 3A/3B/3C oracle and mutation gates |
| FR-028 | T005, T013, T026 | ACK no-reservation and queue/JIT admission evidence |
| FR-029 | T011, T026, T035, T036 | exact cache retention/eviction and cold/warm rows |
| FR-030, FR-031, FR-032 | T007, T009, T022, T028 | origin/transformation, KeyGrant, bounds, zeroization negatives |
| FR-033, FR-034 | T023, T028, T037 | lifecycle/failure evidence matrix |
| FR-035, FR-036 | T003, T004, T007-T009, T014, T019 | adapter/capability predicate contract tests |
| FR-037, FR-038 | T016, T018, T021, T023 | group readiness, state identity, scheduler trace |
| FR-039 | T004, T014, T019 | strategy/sealer and adapter-certified recipe tests |
| FR-040, FR-041, FR-042, FR-043 | T003, T007, T009, T011 | normalization, loaded identity, bounded proof, single-flight tests |
| FR-044 | T016, T018 | local and `NDNSF_DATA_V1` group failure tests |
| FR-045 | T007, T009, T011, T022 | base/overlay/mutable/protection identity tests |
| FR-046, FR-047, FR-048, FR-049 | T003, T012, T024, T027, T030-T032 | offer/runtime/SIF/D0-D2 evidence |
| FR-050, FR-051, FR-052 | T013, T015, T017, T019 | binding/bundle/per-device feasibility tests |
| FR-053, FR-054, FR-055 | T003, T005, T013, T015, T017 | revalidation, scoped handle, local scheduler tests |
| FR-056, FR-057 | T014, T015, T017 | rank/bundle and atomic complete-vector admission tests |
| FR-058, FR-059, FR-060 | T003, T013, T015, T024 | sharing/failure domain, phase vector, profile/snapshot tests |
| FR-061, FR-062 | T014, T019, T020, T021 | 120-vector/per-tensor/hybrid oracle tests |
| FR-063, FR-064 | T004, T025, T028 | custom strategy containment and deterministic sealing |
| FR-065 | T005, T023 | queue/JIT state and fencing-token tests |
| FR-066 | T010, T038 | public default-path integration test and synchronized public docs |
| FR-067 | T003, T024, T027, T038 | native executable/build/SIF offer parity and deployment docs |
| FR-068 | T018, T021, T033, T037 | `NDNSF_DATA_V1` contract/fault/runtime evidence |
| FR-069 | T022, T028, T037 | protected runtime state/zeroization evidence |
| FR-070 | T001, T024-T029, T039 | freeze hash and invalid-candidate evidence |
| FR-071 | T006, T026 | real three-Provider concurrent MiniNDN gate |

## Success Criterion to Gate/Evidence Mapping

| Criteria | Closing tasks | Required evidence |
|---|---|---|
| SC-001, SC-002, SC-004, SC-012 | T007, T008, T025 | canonical identity/publication rows in Gate A |
| SC-003, SC-010, SC-018, SC-034 | T011, T026, T035, T036 | 15 cold/75 warm rows per configuration and exact-hit counters |
| SC-005 | T016, T018, T021, T026, T031-T034 | complete oracle outputs; independent topology labels |
| SC-006 | T021, T023, T026, T034 | eligible/start wake sequence and elapsed threshold |
| SC-007 | T006, T026 | three Provider processes and three concurrent V3 requests |
| SC-008, SC-009 | T022, T023, T028, T037 | deterministic mutation/failure-class matrix |
| SC-011 | T024, T027 | exact-SIF parity and installed native path |
| SC-013 | T022, T028, T037 | KeyGrant expiry/revocation/epoch/zeroization negatives |
| SC-014, SC-019, SC-024 | T003, T012, T024, T027, T030-T032 | allocation→probe→offer→Selection equality |
| SC-015 | T013, T032 | per-device feasibility/concurrency evidence |
| SC-016 | T014-T016, T032 | adapter-certified local device-set execution |
| SC-017 | T003, T005, T015, T028, T037 | lost/stale device fail-closed rows |
| SC-020 | T005, T013, T015, T017, T028 | queue/JIT complete-vector admission races |
| SC-021, SC-026 | T014, T019, T025 | exact rank/tensor coverage mutation corpus |
| SC-022, SC-023 | T003, T013, T015, T025 | sharing/failure-domain and phase-envelope rejection |
| SC-025 | T019-T021, T034 | `[1,2,1]`/`[2,1,2]` oracle and trace invariants |
| SC-027, SC-028 | T004, T025, T028 | sealer mutation and custom-strategy boundary evidence |
| SC-029 | T003, T005, T027 | Python/native ACK parity and reservation-book negative |
| SC-030 | T010, T026, T038 | normal Application integration instrumentation and public docs |
| SC-031 | T018, T028, T033, T037 | capability/manifest/segment fault corpus |
| SC-032 | T001, T024-T029, T039 | one freeze cut and post-freeze hash verification |
| SC-033 | T002, T010, T025, T035 | explicit V2 telemetry and no-fallback matrix |
| SC-035 | T019-T021, T034 | exact rank-to-Provider/device mapping and summed envelopes |

## Evidence Status Rules

Every final row records one of `PROPOSED`, `IMPLEMENTED`, `WIRED`, `EXECUTED`,
`MEASURED`, `PASS`, `BLOCK`, or `INVALID_CANDIDATE`. A later label requires the
evidence for every earlier integration level. Missing hardware may yield `BLOCK`,
but a local semantic or wiring failure must return to its pre-freeze task rather
than being relabelled as unavailable hardware.
