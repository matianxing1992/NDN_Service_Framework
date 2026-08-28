# Traceability: SVS PubSub Payload Interoperability

| Requirement / criterion | Design authority | Task | Acceptance evidence |
|---|---|---|---|
| FR-001, SC-005 ownership isolation | plan: Architecture and Ownership | T002, T003 | Git/path inspection; unchanged NDN-SVS tree |
| FR-002 public APIs | research Decision 2 | T002 | peer source and compile/type-check receipts |
| FR-003 corpus | plan: Payload Corpus | T001 | manifest and deterministic fixture tests |
| FR-004-FR-005 exact receipts | payload receipt contract | T001, T002 | name/length/SHA-256/segment assertions |
| FR-006 classified bounded failures | payload receipt contract | T002, T003 | stage/reason events and deadline receipt |
| FR-007 standalone stop gate | research Decision 3 | T002, T003 | standalone summary and MiniNDN admission receipt |
| FR-008 MiniNDN topology | quickstart and plan flow | T003 | topology, RIB, process, capture, cell summary |
| FR-009 evidence levels | payload receipt contract | T002, T003 | completion summary claim level |
| FR-010 sync-test separation | plan: Structure Decision | T001, T003 | existing regression result plus separate payload artifacts |
| SC-001 bilateral completeness | payload receipt contract | T001, T002 | 8/8 exact unique receipts |
| SC-002 segmented identity | data model and receipt contract | T001, T002 | exact digest and segment count greater than one |
| SC-003 loss cells | plan: Validation Flow | T003 | admitted 0%/5% summaries or NOT_ADMITTED receipt |
| SC-004 bounded diagnosis | data model state rules | T001-T003 | classified terminal summary within deadline |

Negative evidence completes diagnosis and stop-gate behavior but does not
satisfy SC-001-SC-003 or authorize a positive application-interoperability
claim.
