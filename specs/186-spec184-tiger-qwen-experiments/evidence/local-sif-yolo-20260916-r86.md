# Spec186 local exact-SIF YOLO boundary — 2026-09-16

## Scope

This receipt records the smallest requested proof: one real local Apptainer SIF
composition completed a cross-process NDNSF-DI YOLO Y-A request on CPU. It is
application evidence only. The bounded harness teardown remained unqualified,
so this receipt does not close MiniNDN qualification, SIF promotion, or Tiger
GPU acceptance.

## Candidate and composition

| Field | Value |
| --- | --- |
| Candidate | `spec186-yolo-local-atomic-r86` |
| Candidate digest | `sha256:b1fbcadf7d423a9cb0910b2344d8c397f6c9b8a1bc236c475a604b02411e0312` |
| Source commit | `5d2b5d785fc365c46c45f97b12ca11ce30b69bb7` |
| Source seal | `sha256:1cfafed9c78b8f256fe4d5bd881fe2603125f6c679ad8c66044dae6cba4c5ce9` |
| Apptainer | `/usr/local/bin/apptainer` 1.5.3 |
| Base SIF | `.codex-tmp/spec186-local-r86.sif`, `sha256:089a4bc942db5fcc9d01ba2bf62b01ae1836416a232f0806a61931f26d946547` |
| Application bundle | `.codex-tmp/spec186-app-bundle-r86-worker`, tree `sha256:92934b89e842483a12b6669cf935b62a1ec8b1750d1093f097ccc4cbcbc58f79` |
| Backend/model | `onnxruntime-cpu`, YOLO26n ONNX |

The native assembly worker was compiled in the matching SIF builder and supplied
through a separate read-only application bundle. The base SIF was not mutated.

## Observed result

The run directory is
`.codex-tmp/spec186-r86-runs/spec186-yolo-local-atomic-r86-clean1/`.

- Controller, Repo, native Provider and User started in separate processes.
- The User observed one ACK and committed Selection:
  `NDNSF_DI_AUTOPLANNING_ACK_CLOSED` and
  `NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED`.
- The terminal response marker was:
  `YOLO_ACK_DRIVEN_RESULT status=true payload_bytes=7267`.
- The native Provider emitted `realCompute=true`, `runnerKind=onnxruntime-cpu`,
  `runtimeVersion=1.26.0`, `loadCompleted=true` and `warmupCompleted=true`.
- The numerical oracle in `evidence/yolo-numerical.json` reports
  `matched=true`, shape `[1,50,6]`, and `maxAbsError=0.0005340576171875`
  with `rtol=0.0001`, `atol=0.001`.
- `evidence/lifecycle.jsonl` contains `ACK_CLOSED`,
  `SELECTION_COMMITTED` and terminal `TERMINAL_RESPONSE` with `status=true`.

## Boundary and follow-up

`process.log` ended with
`SPEC180_CASE_RESULT status=UNQUALIFIED error=CASE_TERMINAL_CLEANUP_FAILURE:controller:-9`.
The corrected Controller script was mounted read-only for diagnosis, but the
native Controller/Face shutdown still encountered a socket EOF and the bounded
harness forced teardown. Therefore the application response and numerical match
are retained as real execution evidence, while cleanup remains a separate
failure.

The next promoted candidate must put the worker and shutdown repair into the
fixed recipe, validate the complete builder consumer closure, and produce one
fresh SIF from a fresh builder stage. Do not repair this SIF in place or reuse its
forced-teardown result as a clean qualification pass.
