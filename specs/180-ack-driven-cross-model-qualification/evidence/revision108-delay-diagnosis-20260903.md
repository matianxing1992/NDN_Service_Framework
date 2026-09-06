# Spec180 delay diagnosis — revision 108

**Date:** 2026-09-03  
**Status:** `BLOCKED_AT_G0`  
**Subject:** current Spec180 source/candidate; Spec175 remains a frozen local baseline

## Finding

No real NDNSF-DI experiment has failed. No current YOLO candidate has reached
the first live MiniNDN request. The maintained runner correctly stops before
NFD/SVS when G0 inputs are absent or inadmissible.

## Verified causes

1. Temporary packages are either stale (`DetectHead0/1`) or lack the
   registered catalogue signature; the trust root must not be replaced.
2. No immutable candidate currently binds the matching Provider offer-key map,
   canonical Y-A ONNX file, and all digests.
3. The earlier task graph allowed audit/release/negative-case work to proceed
   before one atomic Y-A request, producing `implemented`/`wired` evidence but
   no `executed` terminal Response.
4. QWEN-F production objects are still required before T013/T014 and cannot
   be used to bypass the missing YOLO result.

## Evidence boundary

The direct Y-A probe currently reports
`SPEC180_CASE_RESULT status=WAITING_EXTERNAL_INPUT` with exit code 78 before
network startup. The Spec180 contract gate reports
`contractReady=true`, `qualificationReady=false`; structural audits for both
Spec175 and Spec180 pass. These are readiness/classification results, not a
protocol measurement.

## Binding recovery order

```text
G0 owner-supplied signed role-correct YOLO candidate and bound inputs
 -> G1 one real Y-A terminal Response
 -> G2 reuse the same driver for Y-B/Y-N
 -> T013 QWEN-F/release closure
 -> T014 convergence PASS
 -> T015--T020 one frozen local/SIF/Tiger route
```

Until G0 closes, record the owner and stop once. Do not generate a new trust
root, edit a stale manifest, rerun Spec175, rebuild SIF, or submit Tiger.
