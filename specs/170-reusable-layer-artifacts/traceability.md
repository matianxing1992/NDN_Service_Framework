# Pre-Implementation Traceability: Corrected Spec 170

**Purpose**: Bind each controlling design decision to implementation tasks and
evidence. Historical rows based on same-Provider multi-role execution, a role
spanning Providers, Provider-local tensor ranks, cross-Provider NCCL/socket, or
`LayerReuseFirstStrategy` as topology owner are superseded and cannot close a
criterion.

## Source Intent and Ownership

| Intent | Requirements | Required implementation/evidence |
|---|---|---|
| Reusable placement-independent canonical ONNX objects | FR-001..FR-012, FR-040, FR-043, FR-073..FR-075 | T007-T010; canonical identity/publication and provider-assembly tests |
| ACK-driven request-scoped topology | FR-013..FR-022, FR-063..FR-066 | T003-T005; default-path and sealer mutation evidence |
| One execution role per stage/rank; one-to-one role/Provider ownership | FR-017, FR-027, FR-050..FR-057, FR-061..FR-062 | T003-T006; ownership negative matrix |
| Consumer-pull NDN cross-Provider dataflow | FR-025..FR-027, FR-033..FR-039, FR-044, FR-068 | T011-T018; exact Interest/Data trace and fault matrix |
| ONNX Runtime-only deployment | FR-035, FR-067, FR-072..FR-075 | T008-T009, T020-T021; static/import/runtime gates |
| Truthful 0/1/N device visibility with one role/device selected per Attempt | FR-046..FR-060 | T006, T015, T020-T025; offer/Selection/allocation evidence |
| Protected, revocable artifacts and dataflow | FR-030..FR-034, FR-038, FR-045, FR-069 | T014, T021; security/replay/zeroization matrix |
| Freeze before remote execution | FR-070..FR-071 | T015-T022; immutable local closure and candidate record |

## Hypothesis Traceability

| Hypothesis | Closing tasks | Blocking evidence |
|---|---|---|
| H1 Canonical identities survive placement changes | T007-T010 | Two different ACK snapshots, identical canonical IDs/bytes, distinct assembly specs |
| H2 Default planning is ACK-driven PreSplitFirst | T003-T005 | Unmodified public call, immutable ACK_CLOSED input, no V2 fallback |
| H3 Role ownership is one-to-one | T003-T006, T015 | Positive four-role/four-Provider plan plus same-Provider/cross-Provider-role negatives |
| H4 Providers assemble canonical ONNX locally | T007-T010 | CPU/single-device ONNX oracle and exact warm reuse |
| H5 Tensor traffic is NDN consumer-pull | T011-T017 | Manifest/segment Interest/Data names and zero undeclared channels |
| H6 Heterogeneous hybrid execution is correct | T013, T017-T018 | `[1,2,1]` four-Provider and `[2,1,2]` five-Provider oracle/fault rows |
| H7 Security and failure boundaries fail closed | T014-T016, T021 | Replay/corruption/cancel/no-progress/zeroization matrix |
| H8 Exact SIF matches local candidate | T020-T022 | ABI/native closure, ONNX-only scan, exact hash/freeze records |
| H9 Tiger deployment preserves local semantics | T023-T026 | D0/D1/two-Provider tensor rows; hybrid GPU `BLOCK` if resources absent |

## Functional Requirement Mapping

| Requirements | Tasks | Primary gate |
|---|---|---|
| FR-001..FR-012 | T007 | Canonical object unit/integration gate |
| FR-013..FR-022 | T003-T005, T010 | Placement/default/reuse gate |
| FR-023..FR-029 | T005-T006, T008, T010, T013 | Selection, assembly, admission, execution gate |
| FR-030..FR-045 | T007-T014 | Integrity, security, dataflow, failure gate |
| FR-046..FR-060 | T003, T005-T006, T015, T020-T025 | Device truth and ownership gate |
| FR-061..FR-068 | T003-T005, T011-T018 | Tensor/hybrid NDN dataflow gate |
| FR-069..FR-071 | T014-T022 | Security, local closure, freeze gate |
| FR-072..FR-075 | T007-T009, T020-T021 | ONNX-only application-SIF gate |

## Success Criterion Mapping

| Criteria | Tasks | Evidence |
|---|---|---|
| SC-001..SC-004, SC-010, SC-012, SC-018, SC-034 | T007-T010, T019 | Canonical/reuse cold-warm rows |
| SC-005..SC-009, SC-025..SC-031, SC-035 | T003-T006, T011-T018 | Oracle, ownership, dataflow, lifecycle/fault traces |
| SC-013 | T014, T021 | Grant/revocation/zeroization negatives |
| SC-014..SC-024 | T006, T015, T020-T025 | Device/offer/binding/admission evidence |
| SC-011, SC-032, SC-036 | T020-T022 | Exact-SIF and freeze evidence |
| SC-033 | T004-T005 | Explicit V2 separation/no fallback |
| SC-037..SC-038 | T007-T009, T017-T021 | ACK-driven assembly and offline conversion parity |

## Final Closure Rule

T028 may report PASS only when every requirement/criterion above points to an
immutable passing row from the corrected candidate. Missing GPU resources may
leave a GPU-scale row `BLOCK`; they never justify remapping several roles onto
one Provider or substituting Provider-local NCCL for NDN dataflow.

**Explicit FR coverage index**: FR-001, FR-002, FR-003, FR-004, FR-005,
FR-006, FR-007, FR-008, FR-009, FR-010, FR-011, FR-012, FR-013, FR-014,
FR-015, FR-016, FR-017, FR-018, FR-019, FR-020, FR-021, FR-022, FR-023,
FR-024, FR-025, FR-026, FR-027, FR-028, FR-029, FR-030, FR-031, FR-032,
FR-033, FR-034, FR-035, FR-036, FR-037, FR-038, FR-039, FR-040, FR-041,
FR-042, FR-043, FR-044, FR-045, FR-046, FR-047, FR-048, FR-049, FR-050,
FR-051, FR-052, FR-053, FR-054, FR-055, FR-056, FR-057, FR-058, FR-059,
FR-060, FR-061, FR-062, FR-063, FR-064, FR-065, FR-066, FR-067, FR-068,
FR-069, FR-070, FR-071, FR-072, FR-073, FR-074, FR-075.

**Explicit SC coverage index**: SC-001, SC-002, SC-003, SC-004, SC-005,
SC-006, SC-007, SC-008, SC-009, SC-010, SC-011, SC-012, SC-013, SC-014,
SC-015, SC-016, SC-017, SC-018, SC-019, SC-020, SC-021, SC-022, SC-023,
SC-024, SC-025, SC-026, SC-027, SC-028, SC-029, SC-030, SC-031, SC-032,
SC-033, SC-034, SC-035, SC-036, SC-037, SC-038.
