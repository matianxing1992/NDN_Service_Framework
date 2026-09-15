# Spec186 closure handoff — 2026-09-14

## Boundary

Spec186 remains `IN_PROGRESS`. The direct target is still fresh MiniNDN YOLO
followed by the same base SIF plus read-only application bundle on TigerCluster.
The accepted promotion order is local Apptainer 1.5.3 build and exact-SIF CPU
smoke, immutable upload of the sealed SHA, then Tiger compute Apptainer 1.5.3
same-SHA verification and execution. Login-node Apptainer 1.3.4 is metadata-only.

## State by evidence scope

| Scope | State | Evidence |
| --- | --- | --- |
| Implemented | `PASS` | Spec186 profile, lifecycle, pre-dispatch gate, collector and SIF preflight are maintained under `Experiments/TigerCluster/`. |
| Wired | `PASS` | `design-code-convergence.md`, r6 app bundle and current profile/runtime receipts. |
| Compute build/import | `PASS` at bounded historical scope | `tiger-r38-build-boundary-20260914.md`; final SIF SHA `e890b5de…c168dd`, native import passed on compute 1.5.3. The r38 image remains immutable and predates the replay import fix. |
| Replay import boundary | `FIXED_LOCALLY` | `tiger-r38-import-shadow-fix-20260915.md`; job `212356` exposed source-package shadowing, and the runner plus regression test now select installed native bindings safely. |
| Local SIF promotion | `OPEN` | No local exact-SIF smoke, immutable upload and same-SHA receive receipt; r38 is an explicitly recorded compute-build fallback. |
| MiniNDN YOLO | `WAITING_EXTERNAL_INPUT` / `BLOCKED_AFTER_BOUNDARY` | No fresh Y-A/Y-B/Y-N process run has produced terminal, oracle and cleanup evidence. |
| Tiger single-node GPU | `BLOCKED_AFTER_BOUNDARY` | r38 stopped before MiniNDN at `CANONICAL_CATALOGUE_VERIFY_FAILED`; the raw import cause is fixed locally, but no rebuilt candidate, observed CUDA role or numerical result exists. |
| Tiger two-node/reuse | `NOT_STARTED` | Must wait for T006.d, T007 and T009. |
| Qwen3-0.6B CPU | `WAITING_EXTERNAL_INPUT` | Compatible model/backend tuple remains absent. |

## Next gates

1. Rebuild and reseal a new candidate containing the replay import fix; preserve
   r38 and verify the new exact entrypoint import before any protocol run.
2. Complete T006.d local-first promotion, or record a new local helper/filesystem
   failure and bind any compute-built replacement to a new source seal.
3. Re-run MiniNDN Y-A/Y-B/Y-N, then Tiger single-node, only after the exact
   candidate, app, case and image identities pass the pre-dispatch gate.

No runtime PASS is inferred from the r38 build, import, READY, CUDA visibility or
direct adapter diagnostics.
