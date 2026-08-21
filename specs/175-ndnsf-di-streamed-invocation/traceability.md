# Planned Traceability: NDNSF-DI Streamed Invocation

**Status**: planning baseline; no implementation or experiment pass is claimed.

| Source intent | Requirements | Design authority | Tasks | Acceptance evidence |
|---|---|---|---|---|
| Preserve unary YOLO-style one-shot calls | FR-001, FR-045 | `contracts/api-contract.md` | T004, T008, T020 | G1 unary/Targeted regressions, I14, M01 |
| Add local-LLM-like incremental output without a per-token request | FR-002, FR-003, FR-004, FR-005, FR-006, FR-021, FR-022, FR-023, FR-024 | `plan.md` Decisions 1/7; API and generation contracts | T008..T015 | five-event API case, I01-I03, M02-M04 |
| Keep one complete final result | FR-007, FR-016, FR-017, FR-018 | wire completion contract | T003..T006, T008, T010 | End/Response mutation matrix and exact transcript oracle |
| Use deterministic exact NDN event names | FR-009, FR-010, FR-011, FR-012, FR-013, FR-014, FR-015 | `contracts/wire-protocol-v1.md` | T003, T005, T006, T016 | exact name vectors, Interest/Data lineage, loss/reorder cases |
| Avoid predictive mapping/FEC for request events | FR-010, FR-019 | `research.md` Decisions 2/6 | T003, T006, T022 | no mapping/FEC wire in packet lineage |
| Prefill once and reuse exact local KV | FR-021, FR-022, FR-031, FR-032 | generation contract Sections 4/9 | T011, T013, T014 | one prefill count; KV field mutation matrix; ORT oracle |
| One Provider per complete role; retain PreSplitFirst | FR-023, FR-024, FR-025, FR-026 | generation contract Section 1 | T014, T022, T026 | plan/Selection role map and 1/2/4 CPU + 3-role Tiger lineage |
| Support EOS/stop/max/deadline/cancel correctly | FR-027, FR-028, FR-029, FR-036 | generation contract Sections 2/3/7/8 | T011, T012, T018 | early-EOS/stop/max transition and Unicode stop tests |
| Deployed ONNX-only runtime | FR-030 | plan Technical Context; validation G3/G5/G6 | T012, T013, T021, T025, T026 | import/dependency closure and actual ORT provider evidence |
| Default no ambiguous re-execution; optional bounded replacement | FR-033, FR-034, FR-035, FR-036, FR-037, FR-038 | research Decision 8; generation Section 10 | T011, T017..T019 | I12 opt-in success, I13 default failure, stale-attempt negatives |
| Preserve authorization and event confidentiality | FR-039, FR-040, FR-041 | wire Sections 6-8 | T003, T005, T007, T017 | signer/key/token/policy/replay/AEAD mutation matrix |
| Bound queues/resources, contain callbacks, and use backpressure | FR-008, FR-020, FR-042, FR-043, FR-044 | data model states/metrics | T004..T006, T018 | callback-failure and capacity-1 slow-consumer cases; no-drop/high-water evidence |
| Reuse verified-delivery owners and emit honest evidence | FR-046, FR-047, FR-048, FR-055, FR-056, FR-057 | plan Architecture 9; experiment plan | T001, T010, T015, T020..T028 | manifests with implemented/wired/executed/measured labels |
| Unit and CPU integration before network | FR-049, FR-050 | validation G1/G2 | T002, T003..T020 | complete unit binary and I01-I14 process manifest |
| Real CPU MiniNDN before Tiger | FR-051, FR-052, FR-053 | validation G3/G4 | T021..T023 | exact SIF preflight and 27/27 M01-M09 |
| Final Tiger CUDA functional and 20 token/s qualification | FR-053, FR-054, FR-055, FR-056 | validation G5-G7; experiment plan | T024..T027 | G5/G6 exact functional, G7 unfiltered 30-unit verdict |

## Success criteria map

| Criteria | Closing task/gate |
|---|---|
| SC-001..SC-002 ordered delivery and terminal closure | T003..T010, G1/G2 |
| SC-003 deterministic 1/2/4 role oracle | T002, T014, T022, G2/G4 |
| SC-004 one placement/prefill | T011, T014, T015, G2/G4/G6 |
| SC-005 fault/recovery exactness | T016..T019, I04-I13, M05-M09 |
| SC-006 unary/Targeted compatibility | T008, T020, G1/I14/M01 |
| SC-007 CPU/no-runtime-Transformers | T012, T021..T023, G3/G4 |
| SC-008 one promoted SIF | T021..T024, G3/G4/Tiger preflight |
| SC-009 Tiger functional stream | T025..T026, G5/G6 |
| SC-010 performance verdict | T027, G7 |
| SC-011 repetitions | T025..T027, G5-G7 manifests |
| SC-012 high-impact decisions fixed; omitted low-level choices documented/tested | T001 contract gate and T028 closure |

## Evidence-level rule

Before execution every row is `proposed`. Future closure updates each row only
to one of `implemented`, `wired`, `executed`, `measured`, or
`performance-qualified`, with an exact path and hash. A checked task without its
required run remains at most `implemented` or `wired`.
