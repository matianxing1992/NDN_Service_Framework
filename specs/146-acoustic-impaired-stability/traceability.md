# Traceability: Acoustic Loss/Reorder Stability

| Requirement | Task(s) | Test or measured evidence |
|---|---|---|
| FR-001 | T001, T005, T008 | Pre/post Spec 144 SHA-256 checks; distinct Spec 146 runner and output root |
| FR-002 | T002 | `LiveStreamReorderedRepairDoesNotPreemptInFlightSignedSource` |
| FR-003 | T003 | `LiveStreamAuthenticatedRepairRecoversAfterSourceTimeout` and finite retry cases |
| FR-004 | T002, T003 | Exact-once signed/FEC callback assertions and zero formal late arrivals |
| FR-005 | T003 | Direct `m_recoveryGroupBySource` lookup and focused recovery tests |
| FR-006 | T003 | Recovery-group retirement paths and bounded continuous tests |
| FR-007 | T004 | `LiveStreamAdaptiveSchedulingPrioritizesSourcesAcrossGroups` and `LiveStreamCompletedGroupsDoNotConsumeUnresolvedHorizon` |
| FR-008 | T002-T004 | Stream 63/63 and encrypted-permission/validator 15/15 focused suites |
| FR-009 | T008 | CodeGraph review and branch/string neutrality scan |
| FR-010 | T005, T008 | Analyzer source/group/invocation unit separation and conservation tests |
| FR-011 | T005, T008 | `campaign-cells.csv` contains every required field for all 16 cells |
| FR-012 | T005-T007 | Formal manifest freezes the inherited acoustic topology, profiles, and 60-second window |
| FR-013 | T007, T008 | Treatment verdicts: 1/1, 5/5, 5/5, and 4/5 |
| FR-014 | T008 | Public API symbol diff/scan; no Spec 146 convenience API change |
| FR-015 | T008 | Closure report retains the open payload-confidentiality limitation |
| FR-016 | T004 | `StreamNameResolverCommitsSequentialSuccessorsIncrementally` plus fork/reuse tests |
| FR-017 | T005-T007 | Every formal command explicitly sets `NDNSF_STREAM_PACKET_TIMELINE_TRACE=0` |
| SC-001 | T002 | Focused reorder/processing ownership tests pass |
| SC-002 | T003 | One/two-loss exact recovery and over-capacity termination tests pass |
| SC-003 | T004, T006-T008 | Bounded preflight and formal latency; no monotonic multi-second failure |
| SC-004 | T007, T008 | Fifteen accepted cells; retained `combined-r03` failure; all treatment thresholds pass |
| SC-005 | T007, T008 | Future-hit and Mapping novelty 100%; nonproductive ratio at most 2.251% |
| SC-006 | T005-T008 | 16 terminal single-invocation rows, frozen hashes, exact conservation |
| SC-007 | T008 | Post-implementation CodeGraph and neutrality audit PASS |
| SC-008 | T004 | Twenty incremental successors admitted within declared cache bounds |
