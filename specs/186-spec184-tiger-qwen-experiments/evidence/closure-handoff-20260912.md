# Spec186 closure handoff — in progress

**Date:** 2026-09-12
**Branch:** `SPEC184Experiments`
**Source baseline:** `575b43cc93bbed29932303caf3d09974f1585af7`
**Qualification state:** `IN_PROGRESS`
**Convergence verdict:** `BLOCK`

This handoff records the current boundary without promoting a component check
or an offline receipt to a runtime qualification result.

| State | Verified scope | Evidence |
| --- | --- | --- |
| Implemented | eight strict profiles, deterministic candidate manifests, change-plane invalidation, zero-side-effect pre-dispatch, lifecycle and bounded cleanup | `Experiments/TigerCluster/runtime/spec186_candidate.py`, `jobs/spec184/submit.py`, `tests/test_spec186_candidate.py`; 73 focused/regression tests pass |
| Wired | effective argv/env/bind rendering maps YOLO Y-A/Y-N and Qwen stage-manifest entrypoints; scheduler call follows the gate | `evidence/design-code-convergence.md`, `evidence/pre-dispatch-boundary-20260912.md` |
| Executed | offline prepare/check for all eight candidates; fresh Y-A/Y-B/Y-N attempts stopped at declared environment preflight with no protocol startup | `evidence/pre-dispatch-boundary-20260912.md`, `evidence/minindn-yolo-boundary-20260912.md` |
| Measured | standalone YOLOv8n ORT CUDA reference only; no accepted Spec186 protocol, `[1,50,6]` oracle, token, cross-node or candidate timing result | T005–T012 remain open or waiting |

## Blocking rows and recovery inputs

| Row | Current state | Smallest recovery input |
| --- | --- | --- |
| T005/T006 | `BLOCKED_AFTER_BOUNDARY` | recreate locked Rust `cargo` + offline cargo home, same-revision exported NAC-ABE library/headers, source-sealed ONNX/NAC-ABE/NDN-SVS tuple, rebuild after the provider `--help` source repair, then pass native import/`--help`/`readelf`/`ldd -r` closure |
| T007 | `WAITING_EXTERNAL_INPUT` | complete local MiniNDN environment variables, package/registry/key maps, topology/config and a closed native candidate |
| T008 | `WAITING_EXTERNAL_INPUT` | actual Qwen3-0.6B model, tokenizer, stage manifest, compatible ONNX or GGUF-Q3 backend and digests |
| T009.a | `VERIFIED` | `evidence/tiger-preflight-20260912.md` records Slurm allocation, GPU UUID/capacity, project storage and compute-node Apptainer `--version`; the preflight script is now bounded |
| T009–T010 | `WAITING_EXTERNAL_INPUT` | local Apptainer 1.5.3 now matches compute Apptainer 1.5.3; next inputs are an exact source-sealed Spec186 SIF/app, NFD route and staged candidate receipt |
| T011 | `WAITING_EXTERNAL_INPUT` | T008 model closure plus T009 resource closure; then run the dedicated Qwen profile |
| T012 | `NOT_STARTED` | a passing T010 normal run and an independent new allocation |

## Recovery order

1. Recreate the exact dependency prefix and locked tokenizer bridge, then run
   the clean Waf build at no more than `-j4`; the provider help repair must be
   included in the new source seal.
2. Build or reseal the layered base SIF and matching read-only application
   bundle; rerun native import, `--help`, RPATH and `ldd -r` checks.
3. Re-run fresh local YOLO Y-A/Y-B/Y-N with the complete environment and retain
   terminal, numerical-oracle and cleanup receipts.
4. Supply and verify the Qwen3-0.6B tuple, then run the CPU cold/follow-up
   MiniNDN case.
5. On Tiger, run single-node YOLO, two-node normal/negative, Qwen conditionally,
   and finally the independent reuse allocation. Reconcile each receipt by
   candidate digest before advancing the next row.

Until these steps produce measured receipts, the branch is a reproducible
implementation and dispatch boundary, not a completed Spec186 qualification.
