# Spec186 closure handoff — in progress

**Date:** 2026-09-13
**Branch:** `SPEC184Experiments`
**Checkpoint:** `9b433338` — current-source r6 identity and r55 G3 host receipt
**Source baseline:** `575b43cc93bbed29932303caf3d09974f1585af7`
**Current source commit:** `6d143d3f0f7a7c627af2c1ef6810d79c0738b52d`
**Current source seal:** `sha256:f4676a0f937c903caebc8374893171890d0d6d0639be42a3ced1b101d7512a00`
**Qualification state:** `IN_PROGRESS`
**Convergence verdict:** `PASS (implementation); BLOCK (runtime qualification)`

This is the current handoff. The earlier `closure-handoff-20260912.md` remains
an immutable snapshot. Component evidence and offline candidate receipts are
not promoted to protocol or GPU qualification.

| State | Current scope | Evidence |
| --- | --- | --- |
| Implemented | eight strict profiles, explicit 1.5.3 Apptainer path/version pins, deterministic manifests, zero-side-effect pre-dispatch, lifecycle/cleanup adapters, and explicit dependency-prefix forwarding for native Python binding builds | `tests/test_spec180_native_build.py` (81 passed), `Experiments/TigerCluster/tests` (80 passed), `scripts/spec180_native_build.py`, `evidence/runtime-version-policy-20260913.md`, `evidence/static-wiring-audit-20260913.md` |
| Wired | all eight profiles bind the current-source r6 application bundle; local and remote tree digest is `04c2dd64b4f070cbd909a87f75a0372a0e3d4dae45c7e712641369cf76531d73` | `evidence/application-bundle-r6-20260913.md`; r5 remains immutable local history |
| Executed | local Apptainer 1.5.3 SIF probe passed; current-source r55 produced a validated G3 host M01 manifest for the four-Provider tiny-ONNX stream; the separate YOLO host gate still needs its own Y-A/Y-B/Y-N runs | `evidence/minindn-stream-collaboration-r53-20260913.md`, `evidence/host-m01-r55-g3-20260913.md`, `evidence/host-m01-r12-route-boundary-20260913.md` |
| Measured | r55 measured two tokens (`[4,5]`) with six retries and zero duplicates for the tiny-ONNX regression only; no accepted Spec186 YOLO `[1,50,6]` oracle, Qwen3 tuple, Tiger GPU result or two-node reuse result | T006.a/c and T007–T012 remain open or waiting |

## Current boundaries

- Local `apptainer` resolves only to `/usr/local/bin/apptainer` 1.5.3. Tiger
  compute `srun` reports `/usr/bin/apptainer` 1.5.3-1.el9. The Tiger login
  node is a control-plane metadata boundary; its observed 1.3.4 package is not
  used for SIF execution and is never a runtime fallback.
- The r6 application package is staged read-only in project storage. The exact
  source-sealed base SIF is still absent, so all eight pre-dispatch gates fail
  closed before scheduler side effects.
- The earlier current-host M01 failure was `REPO_SERVICE_ROUTE_NOT_READY`
  after three bounded `/NDNSF/DistributedRepo/Object/v1/STATUS` timeouts.
  r55 now has a PASS route snapshot and G3 manifest; this is separate from the
  repaired NAC-ABE class-layout mismatch and still does not qualify YOLO.
- The r48/r49/r51 application failure was a Core lifecycle bug: non-terminal
  collaboration roles were forced through stream publisher initialization
  without a Provider-specific grant. r53 is the first clean regression after
  the repair and is not a YOLO qualification result.
- Qwen3-0.6B model, tokenizer and stage manifest are still unavailable; no
  Qwen2.5 or fixture artifact is substituted.

## Recovery order

1. Supply or build the exact source-sealed base SIF with Apptainer 1.5.3 and
   verify the r6 base-plus-app composition in the same runtime.
2. Use the validated host route as the local precondition, then rerun fresh
   local YOLO Y-A/Y-B/Y-N with terminal, oracle and cleanup receipts.
3. Supply the Qwen3-0.6B model tuple and run the CPU cold/follow-up case.
4. Execute Tiger single-node and two-node YOLO, then conditional Qwen and
   independent reuse allocation; reconcile every result by candidate digest.

Until those gates produce measured receipts, Spec186 remains a reproducible
implementation and dispatch boundary rather than a completed qualification.
