# Spec 136 R3 Traceability

This is a supplementary coverage map. Normative authority remains with
`spec.md`, `plan.md`, `tasks.md`, `contracts/`, and the active audit.

| Intent / requirement | Design / contract | Tasks | Evidence |
|---|---|---|---|
| One binary and matched controls (FR-001, FR-002, SC-001) | One-binary/two-mode design; fixed controls | T003, T004 | R6 manifest hashes, commands, per-peer summaries |
| One FIFO worker and Face ownership (FR-003, FR-004, FR-005, FR-006, SC-003) | Ownership table; bounded queue and ordered commit | T003, T004 | Worker/Face thread test, accounting counters |
| Real RSA and fetched validation path (FR-007, FR-008, FR-009, SC-002) | RSA setup; Fetch window and Sync batching controls | T003, T004b | Signature counters, valid/tamper preflight |
| Two-node bidirectional workload (FR-010, FR-011, FR-012) | Experimental unit and topology contract | T004, T005 | R6 routes, commands, attempted/delivered summaries |
| Once-only formal matrix (FR-013, SC-004) | Fixed ten-cell order and terminal contract | T004, T005 | R6 sealed manifest and ten immutable receipts |
| Complete metrics and bounded reporting (FR-015, SC-005, SC-006) | Measurement rules and claim contract | T004, T006 | Formal report and post-audit; FR-015 gap explicitly retained |
| Preserve Spec 135 (FR-016) | Frozen-boundary rule | T004-T006 | Before/after protection checks |
| Per-peer admission and fresh preflight (FR-014, FR-017, FR-018, SC-007) | Blocking admission gates | T004b | Caller threads, no-op pacer, admission receipts |
| Signer and drain feasibility (FR-019, FR-020, SC-008) | Split wait/service and complete-drain gates | T004b, T005 | Utilization, outstanding and abandoned counters |
| Unique subscription and load outcome (FR-021) | Mapping de-duplication and terminal interpretation | T003, T004b | Regression test, terminal provenance |

All 21 functional requirements, 8 success criteria, and 4 user stories have a
task and an evidence path. T004b, T005, and T006 are complete. The post-
implementation audit classifies FR-015 metric completeness and worker-specific
SC-003 failure injection as evidence gaps; they are not silently promoted to
PASS.
