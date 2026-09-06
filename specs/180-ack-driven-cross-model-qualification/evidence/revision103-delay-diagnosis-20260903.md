# Spec180 delay diagnosis — revision 103

**Date:** 2026-09-03  
**Status:** `BLOCKED_BEFORE_EXECUTION`  
**Scope:** current Spec180 source and candidate-input boundary only

## Finding

Spec180 has not produced a real NDNSF-DI result because the first runnable
candidate has not crossed the candidate-input gate. This is not evidence that
the NDNSF-DI protocol failed or that MiniNDN is too slow. The maintained
runner is fail-closed and returns before starting NFD/SVS when required
inputs are absent or unverifiable.

The delay has four verified causes:

1. **Candidate identity is not closed.** Temporary YOLO packages either use
   obsolete `DetectHead0/1` role names or use the current `DetectShard0/1`
   names without the registered catalogue signature. An in-place rename would
   break the package/graph/catalogue digest binding.
2. **Runtime inputs are not closed.** No single candidate currently binds the
   Provider offer-signing key map, its digests, and the canonical Y-A ONNX
   path together with the package and case policy.
3. **The first live vertical slice was delayed by ordering.** Boundary tests,
   publication checks, and audit refinements accumulated before one atomic
   Y-A request was made runnable. They are useful `implemented`/`wired`
   evidence, but cannot create `executed` evidence.
4. **Cross-model release was ordered too late.** The production QWEN-F
   Qwen3.6-27B ONNX entrypoint was left outside the candidate-sealing
   boundary. A SIF built before that entrypoint existed could not qualify both
   models without mutation, so it is correctly blocked.

## Verified state

- `scripts/spec180_contract_gate.py` returns `status=PASS`,
  `contractReady=true`, and `qualificationReady=false`.
- The focused dispatcher/release/contract regression set passes: **31
  tests**. This verifies the fail-closed boundary only; it does not start a
  network or qualify a model.
- The full Spec180 Python collection after the release-boundary correction
  passes: **178 tests**, with 22 existing exporter/runtime warnings.
- The release renderer now rejects an unsigned or structurally invalid
  QWEN-F manifest before invoking its scheduler adapter; this is T013
  implementation evidence, not QWEN-F execution evidence.
- The YOLO runner and lifecycle path are implemented/wired, but no live
  Controller/Repository/Provider/User Y-A trace has reached terminal
  Response.
- With the required environment unset, the runner returns
  `ENVIRONMENT_MISSING` before network startup.
- Spec175 remains a sealed `LOCAL_FUNCTIONAL_PASS` for its named source
  identity. Its current-tree `DIRTY_INPUT_TREE` result is an evidence-mixing
  guard, not an unfinished Spec175 task.

## Binding recovery order

```text
G0 fresh signed YOLO package + policy + Provider offer keys + canonical ONNX
  -> G1 one real Y-A terminal Response
  -> G2 reuse driver for Y-B and fixed Y-N controls
  -> T013 finish release tooling and QWEN-F ONNX entrypoint
  -> T014 one design-code convergence PASS
  -> T015--T020 local, SIF, and Tiger qualification
```

If G0 is missing, record `WAITING_EXTERNAL_INPUT` with its owner and stop.
Do not edit a stale manifest, rebuild SIF, submit Tiger, or create another
preflight/audit loop. A real terminal Response, not a focused seam test, is
the first evidence that can advance the queue.
