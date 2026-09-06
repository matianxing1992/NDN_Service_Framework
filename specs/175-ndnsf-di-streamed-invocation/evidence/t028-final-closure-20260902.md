# T028 final closure — 2026-09-02

## Verdict

`FUNCTIONAL_FAIL`; Spec175 is not qualified. The feature deliberately has one
real TigerCluster deployment target, and that target was consumed exactly once
by candidate r5 (Job `208200`). No evidence from another candidate or historical
matrix is combined with this result.

## Closure checks

- T020/G0 source, profile, workload, model, launcher, and SIF identity were
  bound to r5.
- T022/G-L passed the focused checks, host/CPU production-path smoke, and exact
  local-SIF smoke.
- T025/G-T reached Controller/repository/Provider readiness but failed in the
  User before ACK/Selection and model execution because the planning manifest
  `candidateDigest` disagreed with the exact-SIF runtime recomputation.
- The deployment produced no streamed token trace or terminal Response, so the
  functional acceptance condition is not met. Child status and cleanup evidence
  remain preserved with the T025 record.

## Follow-up boundary

The stale/inconsistent planning canonicalization is a local source/runtime
contract defect. It must be fixed and covered by a focused regression before a
new candidate can be considered. A repaired candidate and any later Tiger
deployment require an explicit follow-up Spec; they are not a retry or an
extension of Spec175.

