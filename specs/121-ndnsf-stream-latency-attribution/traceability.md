# Traceability: NDNSF Stream Latency Attribution and Continuity

| Requirement / criterion | Owner | Tasks | Evidence or disposition |
|---|---|---|---|
| FR-001, FR-002, FR-009; SC-001, SC-003 | Core Mapping-frontier continuity | T001, T002, T005 | focused Stream tests and corrected 60-second baseline |
| FR-003, FR-004; SC-003, SC-004 | UAV latest-join and sequence-domain correctness | T001, T003, T004 | UAV protocol-state and collision fixtures |
| FR-005, FR-006, FR-007, FR-008; SC-004, SC-005, SC-006, SC-007 | Honest stage attribution | T001, T004, T005 | analyzer fixtures and withdrawn FIFO aggregate |
| FR-010, FR-013, FR-014; SC-010 | Security, observability, and compatibility | T002, T003, T004, T006, T007 | focused regressions and unchanged Stream wire/API |
| FR-011, FR-012; SC-008, SC-009 | Performance experiment governance | T005, T006, T007 | one exploratory pair preserved; fresh exact-identity acceptance transferred to Spec 122 T009 |

## Cross-Spec Boundary

- Spec 121 is authoritative for Stream continuity, safe latest join, sequence-domain separation, and rejection of false H.264 FIFO correlation.
- Spec 122 is authoritative for acquisition identity, codec PTS/output binding, capture-to-widget/presentation timing, counterbalanced confirmatory cells, and runtime-default promotion.
- Spec 121 result directories are prior/exploratory evidence and MUST NOT be counted as Spec 122 confirmatory cells.
