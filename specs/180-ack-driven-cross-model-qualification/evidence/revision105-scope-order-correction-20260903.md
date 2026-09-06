# Spec180 scope and ordering correction — revision 105

**Date:** 2026-09-03  
**Status:** `BLOCKED_AT_G0`  
**Subject:** current Spec180 source/candidate; not the frozen Spec175 baseline

## Diagnosis

Spec180 has not produced a failed NDNSF-DI protocol result. It has not reached
the first live request because the fail-closed runner correctly stops before
NFD/SVS when the candidate input is not admissible. Two causes must be kept
separate:

1. **Input closure:** no single current candidate binds a role-correct
   `DetectShard0/1` package, registered catalogue signature, case policy,
   Provider offer-key map, and canonical Y-A ONNX file with verified digests.
2. **Execution order:** the original open tasks combined the minimum atomic
   Y-A path with the shared-role and negative matrix. Boundary tests and audit
   notes therefore accumulated while no terminal Response existed. QWEN-F was
   also scheduled after the SIF boundary, which would have invalidated a sealed
   image if implemented there.

## Corrected finite queue

```text
G0 trusted YOLO input closure
  -> G1 one atomic FullModel Y-A terminal Response
  -> G2 reuse the same driver for Y-B and fixed Y-N controls
  -> T013 release tooling and separate QWEN-F ONNX entrypoint
  -> T014 one design-code convergence PASS
  -> T015--T020 one frozen local/SIF/Tiger route
```

Only the first open gate is active. `implemented` and `wired` checks remain
useful diagnostics, but they cannot be promoted to `executed`; `executed`
cannot be promoted to `qualified` without the case oracle and cleanup record.

## Recovery rule

When G0 is missing, record `WAITING_EXTERNAL_INPUT` once with the owner and
stop. Do not rename a stale manifest, generate a substitute signing key, rerun
Spec175, rebuild SIF, submit Tiger, or create another runner. After G0 is
supplied, run only the one Y-A request. The shared-role, negative, QWEN-F,
release, SIF, and Tiger work follows the queue above and consumes the frozen
candidate; any behavior-affecting change returns to its owning pre-T014 task.

This record explains the delay and narrows the work; it does not claim a live
NDNSF-DI result or a performance conclusion.
