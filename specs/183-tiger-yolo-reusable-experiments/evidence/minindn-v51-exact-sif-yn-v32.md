# Exact-SIF MiniNDN Y-N matrix: v51 / v32 layered application

**Run:** `minindn-local-20260909-v51-yn48`
**Date:** 2026-09-09
**Scope:** registered local exact-SIF CPU mutation matrix and post-Selection
dependency cutpoint

The matrix used the same fixed base SIF as the v52 normal run and APP manifest
`sha256:3f81b1c5203bc2f4117dd38a4c7a20ad53027cfeac20f526cb4f27d926afbf4d`.
The v32 APP contains the current Y-N-D User branch; v31 was rejected before a
request because its immutable `user.py` predated that branch.

Y-N-O, Y-N-C, Y-N-P, Y-N-R, Y-N-I, all three Y-N-E variants, and Y-N-L reached
their registered boundaries with the expected fail-closed results.  Their
process cleanup was clean.  The retained Y-N-D output provides the missing
post-Selection negative evidence:

| Observation | Result |
| --- | --- |
| Lifecycle | `SELECTION_COMMITTED` → `PROVIDER_EXECUTION_STARTED`; no terminal success |
| Native cutpoint | Two `NDNSF_DI_OUTPUT_WITHHELD` records for the DetectShard0 → Merge edge |
| User verdict | `DEPENDENCY_DATA_MISSING`, `observedAfterSelection=true`, no reselection |
| Merge verdict | `NDNSF_DI_DEPENDENCY_WAIT_TERMINAL` / failed signed exact Data fetch |
| Cleanup | seven children reaped, no errors, network stopped, no forced cleanup |

The original v51 aggregate process returned `2` because the driver helper
required one withheld tensor and no longer matched the actual two-tensor
cardinality.  After the helper and host semantic validator were corrected, the
immutable retained logs were rehydrated into `negative-evidence.json` and
passed the shared semantic validator.  This is valid evidence of the protocol
cutpoint, but it is recorded separately from a fresh aggregate-driver PASS.

The host-gate producer joins this Y-N-D record with v51 Y-N-E and v52 Y-B in
`evidence/t010-host-gate-producer.md`.  The combined receipt is still
`YOLO_HOST_GATE_COMPONENT_ONLY`, not a GPU/Tiger qualification.
