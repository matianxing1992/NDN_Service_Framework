# Traceability: Serial Sync-Production Offload Proof

| Intent / Requirement | Design owner | Planned task | Acceptance evidence |
|---|---|---|---|
| Same latest source and same binary (FR-001, FR-002, SC-001) | Build/provenance design | T003 | `source-manifest.json`, binary SHA-256, runtime diff |
| Only Face vs one-worker production changes (FR-003, FR-004, FR-005) | Runtime treatment contract | T002, T003 | treatment unit tests, startup records |
| Pure two-process MiniNDN PubSub (FR-006, FR-007, FR-008) | Fixed network/workload | T003 | topology/routes/process admission |
| Direct stage and Face measurements (FR-009, FR-010, FR-017) | Measurement design | T002 | profiler tests, `stage-breakdown.csv` |
| Full publication and traffic accounting (FR-011, FR-019) | Event/analysis contract | T002, T003, T006 | conservation tests, `traffic-breakdown.csv` |
| One signer, no fallback, no unexplained work (FR-012, SC-002) | Signer-seriality proof | T002, T004-T006 | preflight and per-cell admission |
| Pacer/instrumentation validity (FR-013) | Preflight design | T003, T004 | preflight receipts |
| Deterministic non-formal rate selection (FR-014, SC-003) | Pilot contract | T004 | `pilot-cells.csv`, `rate-selection.json` |
| Exactly six once-only formal cells (FR-015, SC-004) | Formal execution contract | T004, T005 | sealed manifest, six receipts |
| Run-level paired analysis (FR-016, SC-005) | Analysis contract | T003, T006 | `run-metrics.csv`, `paired-contrasts.csv` |
| Pre-registered conclusion and no overclaim (FR-018, SC-006, SC-007, SC-008, SC-009) | Outcome decision table | T006 | `conclusion.json`, final report |
| Isolated Boost-1.71 build and protected prior Specs (FR-020, FR-021, SC-010) | Protection/rollback contract | T001, T003, T006 | linkage and closure verification |
| Safety and audit gates before formal work (FR-022) | Safety/failure/shutdown design | T001, T002, T003, T004 | audit, tests, smoke, preflight |

## Story Coverage

| Story | Tasks | Independent closeout |
|---|---|---|
| US1 Isolate execution location | T002, T003 | Same-binary runtime diff passes |
| US2 Prove seriality and accounting | T002, T004 | One-signer/fallback/conservation preflight passes |
| US3 Measure Face and user-visible effect | T004-T006 | One frozen rate, six cells, paired report |
| US4 Preserve evidence | T001, T003, T005, T006 | Hash and receipt closure passes |
