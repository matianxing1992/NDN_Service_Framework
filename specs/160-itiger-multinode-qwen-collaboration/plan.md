# Implementation Plan: iTiger Multi-Node Qwen Collaboration

**Feature**: [spec.md](spec.md)  
**Date**: 2026-07-27  
**Status**: Complete

## Summary

Reuse the existing three-stage Qwen layer pipeline and frozen Qwen stage
artifacts, but do not continue the mixed-runtime path exposed by live Attempt
005. First preserve the accepted cross-node NFD probe and Qwen stage artifacts.
Then rebuild a coherent NDNSF-DI Qwen runtime image/SIF in which NDN-CXX,
NAC-ABE/OpenABE, NDN-SVS, NDNSF, Python bindings, repo client bindings, and
Python dependencies are built or installed as one runtime contract. Only after
native-constructor and single-node NDNSF-DI smoke gates pass may a replacement
three-node secured collaboration request be submitted.

## Technical Context

- Cluster: iTiger `bigTiger`, account `devs`, QOS `normal`, three nodes,
  one `rtx_5000` GPU and one task per node.
- Baseline runtime SIF:
  `/project/tma1/ndnsf-di/releases/spec159-3ac97b8792ff302d/runtime.sif`,
  SHA-256
  `7e904e7fe7502957277ee28778957f781c90f29c8299518c477de129e20b9284`.
  This SIF is now accepted only as historical baseline evidence for T002/T003
  and failed mixed-runtime diagnostics. It is not acceptable for another live
  inference attempt when patched by externally mounted replacement native
  libraries, Python extensions, or vendor-site dependencies.
- Replacement runtime: a new coherent NDNSF-DI Qwen image/SIF with one Python
  runtime and internally matched C++/Python dependencies. The image may mount
  model/stage artifacts and evidence directories, but must not rely on
  externally mounted replacement `.so`, pybind extension, or vendor-site
  directories for formal live acceptance.
- Model: project-mounted Qwen2.5-0.5B exact revision from Spec 159.
- Existing pipeline: `/LLM/Pipeline/Stage/{0,1,2}` with layer ranges
  `[0,8)`, `[8,16)`, `[16,24)`.
- Intermediate transport: planned exact-name segmented Data through
  `publish_output_large_reference()` and `prefetch_input_large()`.
- Evidence roots: local `results/spec160-itiger-multinode-qwen/` and remote
  `/project/tma1/ndnsf-di/evidence/spec160/`.

## Constitution Check

| Gate | Response | Status |
|---|---|---|
| Canonical dynamic runtime | Existing collaboration API and unified service name | PASS |
| Security in data path | Normal permissions, tokens, ACK/Selection, encrypted scopes | PASS |
| CodeGraph first | Collaboration, dependency I/O, and Qwen stage code verified | PASS |
| Spec-driven work | Dedicated Spec 160 owns all new live work | PASS |
| Right-scope validation | Slurm plus allocation-local cross-node NFD and real GPUs | PASS |
| Cohesive tasks | One task per independently meaningful gate | PASS |

## Design decisions

1. Use three nodes because the existing Qwen reference is already a validated
   three-stage, 24-layer pipeline; do not invent a new two-stage model.
2. Use one provider identity and one role per node. Role/provider/node mapping
   is frozen before the requester submits.
3. Use FP16 stage packages to keep each stage bounded and execute each package
   on its local CUDA device. Stage creation is a separate bounded Slurm job.
4. Use one NFD per node with explicit allocation-local faces and routes. No
   ports or services persist after the allocation.
5. Preserve the first probe and inference outcomes. No automatic retry.
6. Treat the run as capability evidence. Record timings, but do not infer
   throughput or scaling.

## Execution sequence

1. Freeze source, baseline SIF, model, stage-generation, policy, and job
   identities already used for T002/T003 and mixed-runtime diagnostics.
2. Run one three-node NFD/GPU allocation probe; stop if it fails.
3. Run one bounded artifact-preparation job if exact stage packages are absent;
   checksum and atomically promote them.
4. Record live Attempt 005 and constructor diagnostics as mixed-runtime
   failure evidence: the crash occurs at `NativeServiceProvider` construction,
   before Qwen model preload and before request execution.
5. Rebuild a clean runtime image/SIF and prove, inside Slurm, that the rebuilt
   image can construct controller/provider/user native objects and run a
   single-node NDNSF-DI smoke without external runtime overrides.
6. Launch controller/requester and one role provider on each allocated node
   using the rebuilt runtime.
7. Verify explicit NFD routes, provider readiness, and role placement.
8. Submit one collaboration request and preserve all logs/results.
9. Analyze correlation and close only if every success criterion passes.

## Safety and rollback

- No login-node inference, build, conversion, or NFD.
- No persistent daemon, firewall change, or exposed cluster service.
- Baseline SIF/model mounts are read-only; replacement runtime images are
  immutable once selected for a live attempt; outputs use unique Spec 160 paths.
- Do not repair a live runtime by mounting locally rebuilt C++ libraries,
  Python extension modules, or ad-hoc vendor-site directories over an older
  SIF. Such a run is diagnostic only and cannot satisfy live acceptance.
- Ephemeral identities live only inside job-local scratch.
- A failed job is immutable evidence, not overwritten or automatically retried.
- Source changes, if required for CUDA device placement, are narrowly scoped,
  tested locally, sealed, and mounted by digest; no Core Qwen special case.
