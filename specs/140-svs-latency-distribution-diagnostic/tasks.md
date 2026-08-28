# Tasks: SVS Latency Distribution Diagnostic

- [x] T001 Audit the frozen Spec 136 boundary, source evidence path, raw-sample
  necessity, two-cell matrix, statistical definitions, and task cohesion;
  record `evidence/pre-implementation-audit.md`.

- [x] T002 [US1] Add delivery sample retention and mean/p50/p95/p99 fields to
  the RSA benchmark without changing RSA, publication, Sync, Fetch, or worker
  behavior; add exact known-sample tests.

- [x] T003 [US1] Implement the independent Spec 140 build manifest, MiniNDN
  runner, and analyzer; reject legacy or mismatched evidence and verify
  combined percentiles from concatenated peer samples.

- [x] T004 [US1] Build the new binary and run focused unit/integration checks,
  recording source, binary, library, linkage, command, and schema identities.

- [x] T005 [US2] Run exactly two fresh 400 pps 10/60/10 MiniNDN cells once,
  retaining any failure without automatic retry.

- [x] T006 [US2] Analyze raw samples, produce the mean/p50/p95/p99 comparison
  and plot, update the Dr. Wang email from this new evidence only, perform a
  post-implementation audit, and freeze Spec 140.

```text
T001 -> T002 -> T003 -> T004 -> T005 -> T006
```
