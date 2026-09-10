# TigerCluster deployment diagnosis: v69–v91

**Updated 2026-09-10.** This record separates local CPU evidence, Tiger
substrate evidence, and the end-to-end YOLO qualification verdict. The latest
real Tiger run reached CUDA and Provider readiness but did **not** produce a
YOLO numerical response.

## Boundary evidence

| Run / boundary | Observation | Qualification meaning |
| --- | --- | --- |
| v69 / job `210254` | `itiger02` reached the runner, then rank 0 stopped at `STAGING_FAILED` / `ValueError: STORAGE_SIF_BUDGET`; profile declared `3525861376` bytes for a `3901079552`-byte SIF. | Stale capacity metadata; failure occurred before Provider/CUDA launch. |
| v70 / APP staging | External APP entrypoints arrived as `0444`; `/app/bin/di-native-provider` returned `Permission denied`. | Transport mode error; APP/SIF bytes were not invalid. Binaries were restored to executable read-only `0555`. |
| v71 / local exact-SIF CPU | Fresh local two-request run passed `NORMAL_EXPERIMENT_PASS` after the mode repair. | Local composition/oracle gate passed; not Tiger GPU evidence. |
| v72 / job `210258` | Exact SIF, capacity, XFS/scratch and NFD socket passed; `apps.yolo prepare` exited 2 while removing `root/.ndn`. | Preparation failed before Provider launch. |
| v73 / job `210259` | Fresh run reproduced the same `OSError(39)` / `OSError(16)` during `identities.issue`. `findmnt` showed `/identities/root` as a 64 MiB tmpfs; importing `ndnsf` created an open PIB there. | Reproduction on a new run rules out stale residual state as the primary cause. The fault is the nested Apptainer HOME plus an open NFS-backed PIB. |
| v75 / local exact-SIF CPU | Corrected wrapper/profile again passed the two-request CPU oracle. | Local regression remained green before the later journal fix. |
| v76 / shared staging | Preparation rejected `SHARED_STAGING_REQUIRED` because the output argument named a run directory instead of `/project/tma1/ndnsf-di/runs`. | Layout contract failure; no candidate bytes changed. |
| v77 / job `210269` | Storage/SIF/socket gates passed; CUDA probe failed `OSError: libcuda.so.1: cannot open shared object file`. | The wrapper had overwritten `LD_LIBRARY_PATH` and hidden Apptainer `--nv`'s `/.singularity.d/libs`; no Provider inference ran. |
| v79 / local exact-SIF CPU | `tiger-local-cpu-v79` returned `PASS` / `NORMAL_EXPERIMENT_PASS`; candidate `sha256:d88c0fb9c3ab699f57d54bd0cf009e12ccbe2cc8046905535c7f3384b4f81ac8`; both requests were `shape=[1,50,6]`, `matched=true`, `maxAbsError=0.0005340576171875`. | Exact local CPU graph and collector passed for the v32 APP/profile. This is a prerequisite, not Tiger qualification. |
| v80 / job `210273` | Exact SIF bytes `3901079552`, SIF hash `sha256:2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5`, socket and allocation receipts passed. CUDA probe observed GPU UUID `GPU-254d5117-9a30-dfd1-5c38-51d196853e8b`, visible device `0`. BackboneNeck, DetectShard0 and DetectShard1 reported `onnxruntime-cuda`; Merge reported CPU and all four Providers became ready. | Tiger substrate and role startup passed. This is still not a YOLO result. |
| v80 / User boundary | User exited `APP_EXIT:user-0:2`; retained JSON classified `RuntimeJournalLockError` at `_ExclusiveJournalLock.__enter__`, before warmup/measured requests. A remote probe reproduced `flock(LOCK_EX)` failure on an `rb` descriptor and success on `r+b`. | Real Tiger end-to-end run failed at the shared journal lock. No `SINGLE_NODE_GPU_PASS` receipt exists. |
| v81 / post-fix submit | APP v33 (journal `r+b` fix; manifest `sha256:2df82daa7f5684ebda690fa325e054ca3d992da1d36a6b2fcbda54af343f2b42`) was built against the unchanged v22 SIF and prepared, but submit rejected retained v79 `localSif` evidence because its app/profile identity was old. | Candidate-bound gate correctly failed closed; a new local/host gate is required. |
| v83/v84 / gate provisioning | v83 could not find `/opt/apptainer/1.5.3/bin/apptainer` on the login node. v84 used `/usr/bin/apptainer` but the login node reports `1.3.4-1.el9`, while the allocated compute-node declaration is `1.5.3`. | Login-node tool discovery cannot replace compute-node verification; no new Tiger inference was started. |
| v85 / host gate | A fresh `-j4` native closure rebuilt successfully; `_ndnsf.so`, the real MiniNDN runner and `ldd` closure passed. | APP v33 host gate is valid, with qualification limited to `YOLO_HOST_GATE_COMPONENT_ONLY`. |
| v86 / exact-SIF Y-B | APP v33 plus the unchanged v22 base SIF completed the normal MiniNDN graph and numerical oracle. | Local normal graph remains green; this is not Tiger evidence. |
| v88 / exact-SIF Y-N | All eight registered authorization/dependency subcases passed, including expired, forged-authority, wrong-recipient and withheld-dependency paths. | Local negative matrix is green; it does not prove remote admission or GPU behavior. |
| v89/v90 / local operator | Calling the development `provision` helper or a direct executor after `submit.py prepare` left a run-owned execution record and was rejected (`LOCAL_RUN_ALREADY_STARTED`). | The maintained flow is `prepare` then `submit.py local`; each failed attempt needs a fresh run id. |
| v91 / remote local CPU | The same staged base+APP v33 composition completed `tiger-local-cpu-v33i-20260910` with `NORMAL_EXPERIMENT_PASS`, two requests, `shape=[1,50,6]`, `matched=true` and `maxAbsError=0.0005340576171875`. | Cross-host SIF+APP+MiniNDN local CPU composition is green; it still does not qualify Tiger GPU. |
| job `210316` / Tiger single-node GPU | One fresh single-node allocation reached `itiger02`; exact SIF/hash, CUDA probe and all four Provider readiness gates passed. Model Providers advertised `onnxruntime-cuda`, Merge advertised CPU, but every V3 ACK carried `"resources":[]`. `PreSplitFirstStrategy` requires a per-CUDA-device `free_memory_mb` row, so no feasible placement was produced and `apps/yolo.py` returned 2 before `PLACEMENT_DECISION`/Selection. | The failure is a native offer-capacity contract defect, not SIF transport or CUDA visibility. Fix the provider-owned signed resource snapshot, rebuild only the APP, then rerun a fresh bounded GPU job. |

## Root causes and fixes

1. **Capacity and staging metadata.** v69 used a stale SIF peak value and v70
   lost executable bits. The profile now records the measured SIF size and the
   wrapper/app launchers are sealed executable read-only. v76 additionally
   confirms that the shared output parent must be supplied exactly.
2. **Offline identity preparation.** Apptainer's `--home /identities/root`
   mounted a tmpfs below the writable identity bind. Import-time PIB creation
   left an open file that NFS could not remove. `baseline.py` now uses
   `/tmp/ndnsf-di-preparation-home` only during offline preparation; issued
   credentials still use `/identities/<role>`.
3. **CUDA library visibility.** The wrapper now appends
   `/.singularity.d/libs` to the sealed runtime `LD_LIBRARY_PATH`, preserving
   `--nv` driver injection.
4. **NFS journal locking.** `_ExclusiveJournalLock` now opens the existing lock
   file with `r+b`, which satisfies Tiger NFS's `flock` requirement without
   changing journal bytes. The focused contention regression mirrors this
   descriptor mode.
5. **V3 GPU offer capacity.** The native C++ offer path previously hard-coded
   `resources:[]` even when a Provider advertised `cuda:0`. The Python
   planner correctly fails closed when no device resource row can prove the
   required free memory. The offer now carries a signed, per-ACK resource
   snapshot supplied by the provider; the executable queries CUDA
   `cudaMemGetInfo` through the runtime-visible library and returns no row when
   the measurement is unavailable. An empty row therefore remains a safe
   rejection rather than fabricated capacity.

## Reproducible execution flow

Run one candidate through these gates in order; stop and retain the first
failure, then use a fresh run identity after a partial prepare or allocation:

| Stage | Required observation | Stop condition |
| --- | --- | --- |
| 1. `check` | Profile schema, source/input/runtime/dispatch hashes, exact SIF size, modes, tool declarations and shared output root. | Any mismatch, stale gate identity or unknown field. |
| 2. `prepare` | Offline issuer receipt, role homes, preparation digest and sealed run bundle. | Residual/open PIB, non-empty private path, or digest mismatch. |
| 3. `local` | Exact base+app SIF composition, MiniNDN process boundary, two CPU requests, independent numeric oracle and clean cleanup. | Import/entrypoint, protocol, dependency, numeric or cleanup failure. |
| 4. `submit` / allocation | Shared transport modes, exact byte/hash receipt, XFS and scratch capacity, NFD socket, node/GPU and Apptainer observation. | Sender/receiver/layout, storage, socket or toolchain mismatch. |
| 5. `apps.yolo prepare` on compute node | Fresh identity preparation with the temporary HOME, role configuration and signed permissions. | Any preparation exit or cleanup residue. |
| 6. Provider startup | Three model roles report CUDA execution; Merge reports CPU post-processing; all four identities/routes are ready; each CUDA ACK contains a fresh signed device resource row. | Missing CUDA/ORT, wrong role placement, route/readiness timeout, empty/stale capacity rows or early exit. |
| 7. User warmup + measured | ACK/Selection, cross-role dependency Data, terminal response and independent oracle (`shape=[1,50,6]`, tolerance contract). | `RuntimeJournalLockError`, missing dependency, infeasible placement, timeout, numeric mismatch or CPU fallback. |
| 8. `collect` / reconcile | Per-request identity, backend/GPU, exit codes, cleanup and accounting/queue digests agree; write immutable evidence under the run root. | Any missing receipt, unknown job state or cleanup failure. |

Only a complete run through stage 8 may be recorded as
`SINGLE_NODE_GPU_PASS`. v80 is explicitly `FAILED` at stage 7 and remains a
diagnostic component-readiness result.

## Current status and next gate

- **Local CPU:** v91 and remote v33i are valid `NORMAL_EXPERIMENT_PASS` runs for
  the APP v33/base v22 composition; v86/v88 provide the corresponding MiniNDN
  normal/negative matrix.
- **Tiger single-node GPU:** T012.b/T013.a remain **IN_PROGRESS**. Job 210316
  proves allocation, CUDA visibility and four-Provider startup, but placement
  stopped because native V3 offers omitted device capacity; no numerical YOLO
  inference passed.
- **Current candidate:** APP v33 is built and staged with the unchanged base
  SIF; its manifest is `sha256:2df82daa7f5684ebda690fa325e054ca3d992da1d36a6b2fcbda54af343f2b42`.
  No base rebuild is required for this app-only fix.
- **Next action:** rebuild APP v34 from the native resource-offer fix, refresh
  the host/local exact-SIF gate, then submit one fresh shared-layout Tiger run.
  Require non-empty CUDA resource rows before accepting placement. Do not reuse
  v91/v33i as GPU evidence or reuse any failed run directory.

See [`tasks.md`](../tasks.md) for task ownership/status and
[`docs/failure-log.md`](../../../docs/failure-log.md) for symptom/root-cause/
lesson entries.
