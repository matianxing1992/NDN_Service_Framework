# Spec186 closure handoff — in progress

**Date:** 2026-09-13
**Branch:** `SPEC184Experiments`
**Checkpoint:** `84cddcbb`
**Source baseline:** `575b43cc93bbed29932303caf3d09974f1585af7`
**Qualification state:** `IN_PROGRESS`
**Convergence verdict:** `PASS (implementation); BLOCK (runtime qualification)`

This is the current handoff. The earlier `closure-handoff-20260912.md` remains
an immutable snapshot. Component evidence and offline candidate receipts are
not promoted to protocol or GPU qualification.

| State | Current scope | Evidence |
| --- | --- | --- |
| Implemented | eight strict profiles, explicit 1.5.3 Apptainer path/version pins, deterministic manifests, zero-side-effect pre-dispatch, lifecycle/cleanup adapters, and explicit dependency-prefix forwarding for native Python binding builds | `tests/test_spec180_native_build.py` (81 passed), `Experiments/TigerCluster/tests` (76 passed), `scripts/spec180_native_build.py`, `evidence/runtime-version-policy-20260913.md` |
| Wired | all eight profiles bind the corrected r5 application bundle; local and remote tree digest is `687610de859155449c51ec2ba4bb7b57c77614cbf0a53f106bb65152f8c07129` | `evidence/application-bundle-r5-20260913.md`, `evidence/pre-dispatch-r5-20260913.md` |
| Executed | local Apptainer 1.5.3 SIF probe passed; a root MiniNDN M01 retry reached topology/NFD/controller/repository startup but stopped at the repository route barrier | `evidence/host-m01-r12-route-boundary-20260913.md` |
| Measured | no accepted Spec186 YOLO protocol result, `[1,50,6]` oracle, Qwen3 tuple, Tiger GPU result or two-node reuse result | T006.a/c and T007–T012 remain open or waiting |

## Current boundaries

- Local `apptainer` resolves only to `/usr/local/bin/apptainer` 1.5.3. Tiger
  compute `srun` reports `/usr/bin/apptainer` 1.5.3-1.el9. The Tiger login
  node's 1.3.4 is metadata-only and is not used for SIF execution.
- The r5 application package is staged read-only in project storage. The exact
  source-sealed base SIF is still absent, so all eight pre-dispatch gates fail
  closed before scheduler side effects.
- The current host M01 failure is `REPO_SERVICE_ROUTE_NOT_READY` after three
  bounded `/NDNSF/DistributedRepo/Object/v1/STATUS` timeouts. It is separate
  from the repaired NAC-ABE class-layout mismatch and does not qualify YOLO.
- Qwen3-0.6B model, tokenizer and stage manifest are still unavailable; no
  Qwen2.5 or fixture artifact is substituted.

## Recovery order

1. Supply or build the exact source-sealed base SIF with Apptainer 1.5.3 and
   verify the r5 base-plus-app composition in the same runtime.
2. Repair or supply the repository route/readiness prerequisite, then rerun
   fresh local YOLO Y-A/Y-B/Y-N with terminal, oracle and cleanup receipts.
3. Supply the Qwen3-0.6B model tuple and run the CPU cold/follow-up case.
4. Execute Tiger single-node and two-node YOLO, then conditional Qwen and
   independent reuse allocation; reconcile every result by candidate digest.

Until those gates produce measured receipts, Spec186 remains a reproducible
implementation and dispatch boundary rather than a completed qualification.
