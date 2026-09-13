# Spec186 design-to-code convergence audit — checkpoint 1

**Date:** 2026-09-12
**Candidate scope:** profile and lifecycle implementation on `SPEC184Experiments`; source seal `575b43cc93bbed29932303caf3d09974f1585af7`.
**Verdict:** `BLOCK`

## Verified wiring

| Contract | Code/evidence | Result |
| --- | --- | --- |
| Profile schema and case registry | `Experiments/TigerCluster/runtime/spec186_candidate.py::load_profile`; four declared profiles | focused mutation suite passes; unknown fields and duplicate JSON fail closed |
| Candidate tuple and invalidation | `build_candidate_manifest`, `earliest_restart_gate` | deterministic digest and external-model restart test pass |
| Pre-dispatch boundary | `pre_dispatch` | no SSH/rsync/staging/sbatch calls; receipt reports zero counters |
| Effective argv/env/binds | `jobs/spec184/submit.py::render_effective` | case, candidate digest, NFD transport, role map and read-only binds are explicit |
| Scheduler ownership | `jobs/spec184/submit.py::submit` | scheduler call is after pre-dispatch only; invalid candidate test observes zero calls |
| Process ownership/cleanup | `local_run`, `jobs/spec184/run.sbatch` | one process group, finite request/cleanup budgets, reaping receipt; focused terminal tests pass |
| Native business ownership | existing `NDNSF_DI_YoloAckDriven_Minindn.py` and `NDNSF_DI_Qwen06B_Native_Minindn.py` are referenced as harness launchers | Python adapter passes through to native entrypoints; no model logic added |

## Controlling gaps

1. The current host's built `di-native-provider` exits with an unresolved
   `DeploymentControlMessage` vtable, and importing `_ndnsf.so` selects an
   incompatible `/usr/local/lib/libndn-service-framework.so` with unresolved
   `ndnsd::discovery` symbols. `readelf -d`/`ldd -r` reproduce this loader
   boundary. T006 must rebuild in the locked dependency environment and seal a
   matching base SIF/app bundle before this audit can pass.
2. No Qwen3-0.6B weights/tokenizer are present. The only discovered GGUF is a
   Qwen2.5 0.5B file and is intentionally rejected as a substitute. T008 is
   `WAITING_EXTERNAL_INPUT` until a compatible model/backend tuple arrives.
3. This local host has no `sbatch`, `nvidia-smi` or installed `minindn` command.
   Tiger single/two-node execution and local MiniNDN execution therefore have
   no runtime evidence yet. T007 and T009–T012 remain open.
4. The Tiger `run.sbatch` payload now executes the explicit rendered argv, but
   a real Tiger staging receipt must prove that the launcher, app bundle,
   base SIF and model paths resolve inside the declared run root. A scheduler
   payload echo is not a qualification result.

## Closing action

Run the T006 clean build/ABI closure with packaged dependency paths first,
materialize the matching base+application layers, then rerun this audit. A
`PASS` requires the native import and `--help` checks to succeed and the
effective launcher to resolve in the same candidate tuple; until then no
MiniNDN or Tiger status may be promoted.

## Checkpoint 2 after profile expansion

The case registry now contains all eight contract cases, including explicit
Qwen stage order/dependency metadata and signed GPU-capacity placeholders.
`test_spec186_candidate.py` passes 11 focused tests and the effective launcher
maps YOLO to `--case Y-A/Y-N` and Qwen to the native stage-manifest entrypoint.
The verdict remains `BLOCK`: these structural changes do not replace the
missing ONNX toolchain, unresolved native ABI, Qwen3 artifact, local MiniNDN
inputs or Tiger scheduler/GPU evidence listed above.

## Checkpoint 3 after dependency probes

The Python ONNX 1.17.0 package was sufficient for a bounded temporary
full-protobuf archive probe, but the result is outside the repository and is
not source-sealed. Waf reconfiguration with that probe and the matching
NAC-ABE prefix passed dependency discovery before stopping at the absent pinned
Rust tokenizer toolchain and offline cargo home. Existing native artifacts
still fail the import/`--help` closure, so the verdict remains `BLOCK` and no
runtime or Tiger result is promoted.

## Checkpoint 4 after all-profile pre-dispatch

Offline `prepare/check` produced a deterministic candidate manifest for each of
the eight cases; the receipts are in
`evidence/pre-dispatch-boundary-20260912.md`. Every invalid or externally
incomplete tuple was rejected with zero SSH, rsync, staging or scheduler side
effects. The first local runtime boundaries remain the missing base SIF/native
loader closure for YOLO and the missing Qwen3 model/stage/tokenizer tuple for
Qwen. This strengthens the dispatch gate but leaves the audit verdict `BLOCK`.
