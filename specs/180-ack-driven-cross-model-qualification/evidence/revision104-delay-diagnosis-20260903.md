# Spec180 delay diagnosis — revision 104

**Date:** 2026-09-03  
**Status:** `BLOCKED_BEFORE_EXECUTION`  
**Subject:** current Spec180 source/candidate, not the frozen Spec175 baseline

## Why the real experiment has not completed

Spec180 has not failed in a live NDNSF-DI exchange; it has not been allowed to
start one. The runner is fail-closed and stops before NFD/SVS whenever the
candidate identity or required runtime inputs are not admissible. The delay is
therefore an execution-order and input-closure problem:

1. **No admissible YOLO candidate.** The temporary packages either advertise
   obsolete `DetectHead0/1` roles or use the current `DetectShard0/1` roles
   without the registered catalogue signature. Renaming a manifest in place
   would break its graph/catalogue digest binding.
2. **No closed runtime input set.** A runnable subject must bind the signed
   package, case policy, Provider offer-signing-key map, canonical Y-A ONNX
   path, and all digests in one candidate identity. Partial or ambient paths
   are rejected before network startup.
3. **Vertical execution was ordered too late.** Boundary tests, publication
   seams, and audit refinements accumulated before one atomic Y-A request had a
   trusted candidate. They prove `implemented` or `wired`, not `executed`.
4. **QWEN-F was outside the early release boundary.** The separate
   Qwen3.6-27B ONNX entrypoint must exist before T014 and SIF sealing; otherwise
   a later addition would mutate the candidate and invalidate earlier evidence.

## What is actually proven

- `spec180_contract_gate.py`: `contractReady=true`,
  `qualificationReady=false`.
- The release/dispatcher and full Spec180 Python checks pass (**178 passed,
  22 existing warnings**). They verify rejection and launch wiring only.
- The host release renderer now rejects unsigned, wrong-model, malformed,
  CPU-fallback-enabled, or non-CUDA QWEN-F manifests before scheduler calls.
- The maintained YOLO runner has Controller/Repository/Provider/User barriers,
  signed catalogue publication/readback, encrypted input-reference publication,
  request/attempt-bound lifecycle logging, and owned cleanup seams.
- No current Y-A run has produced a live ACK closure, Selection, Provider
  execution, terminal Response, or qualification manifest.
- Spec175 remains a sealed `LOCAL_FUNCTIONAL_PASS` for its named CPU/MiniNDN
  source identity. Its current-tree dirty gate is an evidence-mixing guard,
  not unfinished Spec175 work.

## Binding recovery order

```text
G0 fresh signed YOLO package + policy + Provider offer keys + canonical ONNX
  -> G1 one real Y-A terminal Response
  -> G2 reuse the driver for Y-B and fixed Y-N controls
  -> finish T013, including the separate QWEN-F ONNX entrypoint
  -> T014 one design-code convergence PASS
  -> T015--T020 one frozen local/SIF/Tiger qualification route
```

If G0 is missing, the orchestration record is `WAITING_EXTERNAL_INPUT` with the
owner and the runner stops before NFD/SVS. A runner-level detail such as
`ENVIRONMENT_MISSING` or `CANDIDATE_*` is diagnostic information for that same
pre-start state, not a protocol failure. Do not edit a stale manifest, rerun
Spec175, rebuild SIF, or submit Tiger. A real terminal Response is the first
evidence that can advance the queue.
