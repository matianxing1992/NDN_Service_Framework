# Spec186 closure handoff — in progress

**Date:** 2026-09-13
**Branch:** `SPEC184Experiments`
**Source baseline:** `575b43cc93bbed29932303caf3d09974f1585af7`
**Qualification state:** `IN_PROGRESS`
**Convergence verdict:** `PASS (implementation); BLOCK (runtime qualification)`

This handoff records the current boundary without promoting a component check
or an offline receipt to a runtime qualification result.

| State | Verified scope | Evidence |
| --- | --- | --- |
| Implemented | eight strict profiles, deterministic candidate manifests, directory-aware immutable app bundle digest, change-plane invalidation, zero-side-effect pre-dispatch, lifecycle and bounded cleanup | `Experiments/TigerCluster/runtime/spec186_candidate.py`, `jobs/spec184/submit.py`, `tests/test_spec186_candidate.py`; 74 focused/regression tests pass |
| Wired | effective argv/env/bind rendering maps YOLO Y-A/Y-N and Qwen stage-manifest entrypoints; scheduler call follows the gate; native provider/requester/authority/worker closure passes | `evidence/design-code-convergence.md`, `evidence/native-abi-closure.md`, `evidence/pre-dispatch-boundary-20260912.md` |
| Executed | offline prepare/check for all eight candidates; fresh Y-A/Y-B/Y-N attempts stopped at declared environment preflight with no protocol startup | `evidence/pre-dispatch-boundary-20260912.md`, `evidence/minindn-yolo-boundary-20260912.md` |
| Measured | standalone YOLOv8n ORT CUDA reference only; no accepted Spec186 protocol, `[1,50,6]` oracle, token, cross-node or candidate timing result | T005–T012 remain open or waiting |

## Blocking rows and recovery inputs

| Row | Current state | Smallest recovery input |
| --- | --- | --- |
| T005 | `VERIFIED` | implementation convergence passed after the provider `--help` repair and fresh native closure; qualification remains governed by T006–T012 |
| T006.a | `BLOCKED_AFTER_BOUNDARY` | the locked 188/188 native target build and identity verifier pass; complete Core/Repo/DI unit/integration campaign is still required |
| T006.c | `BLOCKED_AFTER_BOUNDARY` | local/staged app bundle `badf6a0afb36e43d02f7103cba36383bf8f0336e2310a0fd546c33223734734d` and source handoff seal `sha256:597c44a97b34655dfb67b9fc3bff3693b844f5cc1f10624870554bdee8e658e2` exist; current host-gate manifest and exact source-sealed 1.5.3 base SIF/composition receipt are still absent |
| T007 | `WAITING_EXTERNAL_INPUT` | complete local MiniNDN environment variables, package/registry/key maps, topology/config and a closed native candidate |
| T008 | `WAITING_EXTERNAL_INPUT` | actual Qwen3-0.6B model, tokenizer, stage manifest, compatible ONNX or GGUF-Q3 backend and digests |
| T009.a | `VERIFIED` | `evidence/tiger-preflight-20260912.md` records Slurm allocation, GPU UUID/capacity, project storage, compute-node Apptainer `--version`, and the staged 9-file app bundle digest; the preflight script is now bounded |
| T009–T010 | `WAITING_EXTERNAL_INPUT` | local Apptainer 1.5.3 now matches compute Apptainer 1.5.3 and the app bundle is staged; next inputs are an exact source-sealed Spec186 SIF, NFD route and matching base-plus-app composition receipt. The old SIF failed the app `--help` probe on missing `libboost_system.so.1.71.0` and is rejected. |
| T011 | `WAITING_EXTERNAL_INPUT` | T008 model closure plus T009 resource closure; then run the dedicated Qwen profile |
| T012 | `NOT_STARTED` | a passing T010 normal run and an independent new allocation |

## Recovery order

1. Recreate the exact dependency prefix and locked tokenizer bridge, then run
   the clean Waf build at no more than `-j4`; the provider help repair must be
   included in the new source seal.
2. Build or reseal the layered base SIF with local Apptainer 1.5.3 and the
   matching read-only application bundle; rerun native import, `--help`, RPATH
   and `ldd -r` checks inside that exact composition.
3. Re-run fresh local YOLO Y-A/Y-B/Y-N with the complete environment and retain
   terminal, numerical-oracle and cleanup receipts.
4. Supply and verify the Qwen3-0.6B tuple, then run the CPU cold/follow-up
   MiniNDN case.
5. On Tiger, run single-node YOLO, two-node normal/negative, Qwen conditionally,
   and finally the independent reuse allocation. Reconcile each receipt by
   candidate digest before advancing the next row.

Until these steps produce measured receipts, the branch is a reproducible
implementation and dispatch boundary, not a completed Spec186 qualification.
