# T016 / S4 — Exact-SIF Y-B Replay (Revision 121) — 2026-09-04

> **INVALIDATED — DO NOT REUSE.** Revision 123 changed the Controller
> PUBPARAMS readiness/runtime source after this replay. This historical PASS
> is retained for audit history only; its SIF and evidence cannot satisfy a
> new T014/T015/T016 gate or be combined with the current source.

**Status**: `SPEC180_EXACT_SIF_REPLAY status=PASS case=Y-B` — one complete
Y-B request executed with NFD and every application child inside the sealed
candidate image; no source or package overlay was used.

## Candidate identities

- Source seal: `sha256:5087b78767579433f2388fa32de92e7bc43b891280c169d0b00c05300ddfa096`
  (workspace.tar `sha256:9620108e60727bcb5d0f275859b8c0815f9b6b1969eb3492a6d6721233ac56d9`)
- SIF: `.local-tmp/spec180-candidate-r119/spec180-runtime.sif`
  `sha256:c7c84006ade8c3d657e95ad888589ac748cead5a17738856337e842fee20ec77`
  (3.5 GB)
- Definition: `.local-tmp/spec180-candidate-r119/spec180-runtime-final.def`
  (boundary validator PASS: `container-runtime-in-sif`, no host binary
  inputs, stale base artifacts replaced)
- Apptainer 1.5.3 (`/opt/apptainer/1.5.3/bin/apptainer`)
- Base: the qualified Spec174 r24 runtime SIF (CUDA/TensorRT/CPU ORT
  providers present; PyTorch/Transformers removed in the final stage)

## In-image closure (executed)

- `import ndnsf._ndnsf, ndnsf_distributed_inference, py_repoclient, ndn` OK
- `yolo_2x2_lib` imports from the sealed replay repo
- `ldd` for `di-native-provider`, `di-native-fault-provider`, and the
  `_ndnsf` extension: zero unresolved dependencies
- The runner contract markers are present in the in-image runner copy

## Replay execution

- Host-side driver: `packaging/ndnsf-di-container/jobs/spec180/replay-exact-sif.py`
  (validates Apptainer version, SIF digest, and the runner command-provider
  contract before launch; fresh output root required)
- The host MiniNDN namespace remains the outer context; NFD and all
  Controller/Repo/Provider/User children run through the Apptainer command
  provider (`--cleanenv`, per-node `--home`, `--pwd /opt/ndnsf-di/replay/repo`).
- Lifecycle: the complete ten-milestone journal (ackCount=4, shared
  candidate 57547ffc…, providerCount=4, TERMINAL_RESPONSE status=True)
- Terminal result digest:
  `sha256:e6f942bc3e9d35409f8694b732732c542b108bad2a9ede4737eb2665f8dc23aa`
  — byte-identical to the host MiniNDN Y-B/Y-A results and the offline
  full-model forward.

## Code changes this revision (each with a focused test)

- The runner carries the exact-SIF command-provider contract
  (`SPEC180_RUNTIME_SIF`/`SPEC180_RUNTIME_APPTAINER`, `sif_exec_prefix`,
  `Spec180SifNfd`, `--cleanenv`, host-process-fallback marker); child
  commands use the sealed repo paths; the operator's secret/data trees are
  bound explicitly (sudo-safe home resolution).
- `prepare-local-sif-source.py` FILES includes the YOLO replay closure
  (legacy `NDNSF_DI_Yolo2x2_Minindn.py` + `examples/python/.../yolo_2x2`).
- The YOLO Provider accepts `--backend {onnxruntime-cpu,onnxruntime-cuda}`;
  the executor selects the ORT provider chain from the closed
  `NDNSF_ORT_PROVIDERS` allowlist (default CPU).

## Boundary

Local exact-SIF replay evidence. The Tiger submission (S5) is the next
step and requires the T013-owned Tiger launcher path (the in-image
dispatcher currently execs the MiniNDN runner, which has no MiniNDN
substrate on a Tiger node).
