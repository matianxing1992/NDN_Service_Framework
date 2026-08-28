# Implementation Plan: iTiger NDNSF-DI Qwen Capability

**Feature**: [spec.md](spec.md)  
**Date**: 2026-07-27  
**Status**: In progress

## Summary

Promote the accepted Spec 158 App image into a uniquely named GHCR capability
candidate, resolve its immutable digest, materialize that digest as SIF through
a short CPU Slurm job, then run two ordered GPU gates: standalone Qwen and real
secured NDNSF-DI Qwen request/response.

## Technical Context

- Local image: `ndnsf-di:spec158-app-reuse-proof`,
  `sha256:8f8d2a0b219dc6ee0216c43a3ead9d65125850431f30cc26b7aa4b88c0f2f6e4`.
- Registry: `ghcr.io/matianxing1992/ndnsf-di`, unique `spec159-*` tag followed
  by digest-only consumption.
- Cluster: iTiger `bigTiger`, Slurm, Apptainer 1.3.4, project root
  `/project/tma1/ndnsf-di`.
- Initial placement: one GPU on `itiger07` when available because its UID and
  Apptainer preflight already passed; placement is recorded, not assumed.
- Model: Qwen2.5-0.5B-Instruct at the exact frozen revision, mounted read-only.
- Evidence: local `results/spec159-itiger-qwen-capability/` plus durable remote
  `/project/tma1/ndnsf-di/evidence/spec159/`.

## Constitution Check

| Gate | Response | Status |
|---|---|---|
| Canonical dynamic runtime | Real generic secured NDNSF-DI path | PASS |
| Security in data path | No permission/token/backend bypass | PASS |
| CodeGraph first | Current Slurm and adapter paths inspected | PASS |
| Spec-driven work | Dedicated Spec 159 owns live capability evidence | PASS |
| Right-scope validation | Standalone cannot substitute for NDNSF-DI | PASS |
| Cohesive tasks | One task per independently meaningful gate | PASS |

## Execution design

1. Record live cluster/model/storage discovery and local image identity.
2. Tag and push once under a new capability identity; resolve digest from GHCR.
3. Render, inspect, and submit a bounded CPU materialization job. Build SIF in
   job scratch, verify SHA-256, then atomically promote it.
4. Submit one bounded GPU job that checks device/driver/container CUDA and runs
   deterministic standalone Qwen.
5. Submit one bounded GPU job that starts job-local NFD and the real
   controller/provider/requester path, mounts identities/model read-only, and
   correlates one response.
6. Audit evidence against every FR/SC. Preserve failures without hidden reruns.

## Safety and rollback

- No work runs on the login node beyond discovery, rendering, and submission.
- No model or credential enters OCI/SIF.
- Spec 110 and 158 artifacts remain unchanged.
- New remote files live under a unique Spec 159 prefix.
- Cleanup is explicit and never removes accepted/frozen evidence.
- A failed post-start job closes as measured negative; a replacement is a new
  task identity and requires an updated plan entry.
