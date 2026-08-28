# Spec 170 objective traceability (interim, 2026-08-19)

This is an interim traceability index for the final audit. It does **not**
claim T039 completion: every FR/SC/H row has not yet been proven against one
frozen candidate. A row marked `PASS (qualification)` is evidence for the
named exercised behavior only.

| Objective area | Requirements represented | Status | Evidence |
|---|---|---|---|
| Request/ACK/Selection/Response lifecycle | FR-013..FR-029; SC-005..SC-007 | PASS (qualification) | Full local integration; real CPU-ONNX production D2b |
| Python/native V3 contract and V2 separation | FR-001..FR-012, FR-066..FR-067 | PASS (covered subset) | 101 Python tests; exact current test command in `closure-report.md` |
| Device/queue/JIT/admission semantics | FR-046..FR-067; SC-014..SC-024 | PARTIAL | Focused unit groups and local Worker tests; no full Tiger matrix |
| DATA_V1 manifest, AEAD, replay, bounds, deadlines | FR-068; SC-025..SC-027 | PASS (named local cases) | 9 cross-Provider cases, 50 fault seeds, production SVS bridge cases |
| Protected artifact/key-grant lifecycle | FR-030..FR-034, FR-069; SC-008, SC-013 | PARTIAL | Named authorization and NAC-ABE negatives pass; full grant/revocation/zeroization lifecycle open |
| Hybrid redistribution and 3A/3B/3C oracle | FR-026..FR-027, FR-037..FR-045, FR-061..FR-063; SC-025..SC-030 | PARTIAL | Real MiniNDN CPU positive `[1,2,1]`/`[2,1,2]` paths plus local adapter; full mutation/transport corpus and remote equivalence open |
| Build/SIF/toolchain boundary | FR-070..FR-071; SC-010..SC-011a | PASS for r23 sealed source | `current-sif-r23-local-20260819.md`; not current dirty-tree evidence |
| Failure and negative coverage | H1..H10; applicable SC negative rows | PARTIAL | `security-failure-matrix.md`; complete T028/T037 matrix missing |
| Reproducible performance and optimum claim | SC-034 and T036 | BLOCK | `performance-closure-audit-20260819.md`; descriptive only, no hierarchical bootstrap/TOST/Holm |
| Freeze and final row-by-row closure | T029 and T039 | BLOCK | No frozen-candidate manifest, freeze report, or completed row-by-row proof |

## Interpretation rule

No paper, slide, or release note may summarize a `PARTIAL` or `BLOCK` row as
“complete,” “all failure modes covered,” or “globally optimal.” Any executable
or dependency change after r23 invalidates r23 as evidence and requires a new
candidate identity before Tiger execution.
