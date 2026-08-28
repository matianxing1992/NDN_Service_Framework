# Spec 134 Traceability

| Intent / requirement | Design | Task | Evidence |
|---|---|---|---|
| Audit NDN-SVS itself (`FR-001`, `FR-002`, `FR-003`, `FR-004`, `SC-001`) | Source-reality matrix | T001 | `evidence/threading-contract-audit.md` |
| Preserve negative evidence; stop unjustified repair (`FR-002`, `FR-004`, `FR-005`) | Evidence classes and rollback | T002 | Existing diagnostic receipts plus audit labels |
| Two nodes/processes; one I/O thread each (`FR-006`, `FR-007`, `FR-008`, `FR-009`, `FR-012`, `SC-002`, `SC-003`) | Per-peer execution and absolute release contract | T003 | Source-contract tests and driver self-test |
| Once-only correctness qualification (`FR-010`, `FR-011`, `FR-012`, `FR-013`, `FR-014`, `FR-016`, `SC-004`, `SC-005`) | Qualification receipt contract | T004 | New immutable terminal receipt |
| Correct Spec 133 handoff (`FR-015`, `SC-006`) | Phase 3 gate | T005, T006 | Updated Spec 133 tasks and final audit |
