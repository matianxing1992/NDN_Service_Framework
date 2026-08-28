# Traceability: UAV Video True End-to-End Latency

| Requirement / criterion | Design owner | Tasks | Closing evidence |
|---|---|---|---|
| FR-001, FR-002, FR-003, FR-004, FR-005, FR-006; SC-001, SC-002, SC-003 | Source identity and latency evidence contract | T001, T004, T005 | deterministic oracle tests, stage CSV, corrected baseline |
| FR-007; SC-007 | APP/GUI bounded media queues | T002, T008, T009 | queue/drop unit tests and cell accounting |
| FR-008; SC-004 | Provider codec readiness and Ground Station decoder lifecycle | T006, T009 | startup candidates and five matched pairs |
| FR-009, FR-010 | APP-owned backend capability and rollback | T002, T003, T006, T007, T008, T009 | capability report, explicit backend/fallback status |
| FR-011; SC-011 | Existing Stream/security/FEC invariants | T004, T007, T009 | focused protocol/security/FEC and MiniNDN regressions |
| FR-012, FR-013, FR-014; SC-005, SC-006, SC-007, SC-008, SC-009, SC-010 | Experiment controller and analyzer | T005, T006, T007, T008, T009 | frozen cell summaries, five matched pairs, trace controls |
| FR-015 | Versioned additive UAV metadata compatibility | T002, T004, T009 | old/new decode cases and rollback regression |

## Cross-Spec Ownership And Evidence Reuse

| Concern | Authoritative spec | Rule |
|---|---|---|
| Mapping continuity, latest join, sequence-domain separation, invalid FIFO rejection | Spec 121 | Completed prerequisite; preserve its implementation and findings. |
| Acquisition identity, codec PTS/output association, true capture-to-widget/presentation | Spec 122 | Must pass T001-T005 before optimization evidence is admissible. |
| Candidate selection | Spec 122 T006-T008 | Exploratory only; may select one frozen candidate. |
| Runtime-default acceptance | Spec 122 T009 | Uses only new counterbalanced confirmatory identities; Spec 121 and exploratory cells are excluded. |

## Source Intent Mapping

| User intent | Story / requirement | Task outcome |
|---|---|---|
| Determine whether 87 ms decoder startup and 19 ms GUI time are normal | US1, FR-003–FR-005 | T001/T005 separate startup, cadence, widget, and unavailable presentation |
| Analyze every stage before optimizing | US1/US2, FR-003–FR-006, FR-012–FR-014 | T004/T005 create exact stage evidence and rank bottlenecks |
| Use mature live-video techniques only when they help | US2/US3, FR-007–FR-010 | T006–T008 probe persistence, key-frame readiness, timestamp backend, bounded mailbox, and direct rendering independently |
| Fix the real latency without weakening NDNSF Streaming | FR-011, SC-008/SC-011 | T007/T009 retain exact-name prefetch, security, FEC, and Interest/resource gates |
