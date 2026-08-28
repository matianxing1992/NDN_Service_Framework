# Traceability

| Requirement | Design / contract | Task | Acceptance evidence |
|---|---|---|---|
| FR-001, FR-002, SC-001 | Execution Gate A | T002 | Three unique hosts/GPUs and successful named-Data probe |
| FR-003, FR-004, FR-009, SC-006 | Runtime/Image Gate B | T003, T004b, T004c | Frozen stage manifest, coherent runtime manifest, native-constructor smoke, single-node smoke |
| FR-005, FR-006, FR-007 | Execution Gate C | T004d | Job 174221 secured flow, three-role mapping, two completed dependency fetches, requester response |
| FR-008, SC-003, SC-005 | Data model and evidence contract | T004d, T005 | Attempt 015 timing markers, Data names, producer/consumer SHA-256 chain, node/GPU map, and post-hoc audit |
| FR-010, FR-011 | Exactly-once Slurm contract | T002-T004d | Unique source/submission digests, job IDs, preserved terminal states, and no Attempt 016 |
| FR-012 | Safety contract | T002-T005 | 58/58 checksum verification, secret-file scan, and ephemeral job-local identity record |
| FR-013, SC-004 | Scope and completion audit | T005 | `completion-summary.md`; exact token/shape result with no scaling claim |
| FR-014, FR-015 | Mixed-runtime rejection contract | T004a-T004c | Attempt 005 and constructor diagnostics preserved; clean runtime rebuild required before live rerun |
