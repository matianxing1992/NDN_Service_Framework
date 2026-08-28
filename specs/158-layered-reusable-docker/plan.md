# Implementation Plan: Layered Reusable NDNSF-DI Docker

**Feature**: [spec.md](spec.md)  
**Date**: 2026-07-26  
**Status**: Complete (local no-GPU acceptance)

## Summary

Replace the monolithic local-foundation plus GPU-delta development workflow
with three independently versioned build boundaries:

```text
official CUDA/cuDNN + pinned Python
  -> ML Base (devel/runtime)
  -> NDN Base (devel/runtime)
  -> App Runtime (ndn-svs + NDNSF + NDNSF-DI)
```

The stable prefixes are `/opt/onnxruntime`, `/opt/venv`, and `/opt/ndn-base`.
Frequently changed content installs under `/opt/ndnsf-app`. The final runtime
is assembled from the runtime bases; compilers and sealed source archives stay
in builder stages. Existing Spec 110 Dockerfiles remain unchanged as rollback.

## Technical Context

**Languages**: Dockerfile, Bash, Python 3, JSON, C++17 build inputs  
**Container engine**: Docker Buildx/BuildKit  
**Compatibility baseline**: Ubuntu 20.04, CUDA 12.4.1 runtime with cuDNN 9,
Python 3.10, ONNX Runtime 1.20.1, PyTorch 2.6.0 cu124  
**Stable native dependencies**: ndn-cxx, NFD, OpenABE/RELIC, NAC-ABE,
websocketpp  
**Mutable dependency chain**: ndn-svs, NDNSD (which links to ndn-svs), NDNSF
Core/bindings, NDNSF-DI packages and native adapters  
**Testing**: static contract tests, lock/seal mutation tests, Docker layer
probes, ELF/Python closure, unprivileged runtime smoke, application-only rebuild
cache proof  
**Host constraint**: approximately four CPU cores and no local NVIDIA GPU;
build parallelism defaults to two and live CUDA remains deferred  
**Storage constraint**: preserve accepted image and maintain a pre-build disk
reserve; cleanup only enumerated rebuildable images

## Constitution Check

| Gate | Design response | Status |
|---|---|---|
| Canonical dynamic runtime | Packaging only; no runtime API or wire change | PASS |
| Security in data path | Security libraries preserved; no credentials enter images | PASS |
| CodeGraph first | Current packaging and test ownership inspected before design | PASS |
| Spec-driven durable work | Spec, contracts, tasks, audit, and evidence required | PASS |
| Right-scope validation | Local build/static evidence is separated from later GPU/iTiger evidence | PASS |
| Cohesive tasks | Tasks close independently reviewable layer or operational outcomes | PASS |

Post-design check: the plan changes packaging ownership only. It does not modify
NDNSF authorization, protocol behavior, frozen experiments, or external systems.

## Architecture and ownership

| Product | Parent | Owns | Explicitly excludes | Install roots |
|---|---|---|---|---|
| `ml-base-devel` | pinned CUDA/cuDNN runtime + pinned Python | Python runtime, venv, PyTorch, ORT GPU Python/C++ SDK, Transformers; no unused CUDA compiler | NDN, app, models, secrets | `/usr/local`, `/opt/venv`, `/opt/onnxruntime` |
| `ml-base-runtime` | pinned CUDA runtime | runtime copy of ML closure | compilers, NDN, app, models | same ML roots |
| `ndn-base-devel` | exact `ml-base-devel` image ID | ndn-cxx, NFD, OpenABE/RELIC, NAC-ABE, websocketpp, headers | ndn-svs, NDNSD, NDNSF, models, credentials | `/opt/ndn-base` |
| `ndn-base-runtime` | exact `ml-base-runtime` image ID | runtime copy of stable NDN closure | headers/build tools where removable, app | `/opt/ndn-base` |
| `app-runtime` | exact NDN devel/runtime image IDs | ndn-svs, NDNSD, NDNSF, Python bindings, NDNSF-DI, examples/adapters | models, credentials, build source | `/opt/ndnsf-app` |

### Dependency invalidation

| Input change | Rebuild |
|---|---|
| CUDA/cuDNN, Python, PyTorch, ORT, Transformers lock | ML + NDN + App |
| ndn-cxx/NFD/OpenABE/NAC-ABE lock | NDN + App |
| ndn-svs or NDNSD revision | App only |
| NDNSF/NDNSF-DI source | App only |
| runtime entrypoint/probe | App only |
| model weights or deployment identity | Never build; runtime mount only |

### Source and release identity

Four lock documents isolate platform, ML, stable NDN, and application inputs.
Stable third-party repositories use checksum-bound archives in a layer-specific
seal. The local workspace is copied through a filtered context and recorded as
a development source identity. A dirty workspace produces a development
candidate, never a formal release label.

The build manifest records:

- lock and seal SHA-256 values;
- parent image references and resolved image IDs;
- produced image IDs, RepoDigests when available, size, timestamps, duration;
- build command and selected parallelism;
- probe and scan results;
- `developmentCandidate` versus `formalRelease` authority.

The ML development product intentionally uses the same CUDA/cuDNN runtime base
as the ML runtime product. Current code compiles no CUDA kernels: the native
adapter consumes the independently pinned ONNX Runtime C++ headers, while the
NDN/App builder installs the normal C++ toolchain. Pulling the 2.63 GB CUDA
compiler layer would add no required capability.

## Build implementation

New files are additive under `packaging/ndnsf-di-container/oci/layered/`:

```text
Dockerfile.ml
Dockerfile.ndn
Dockerfile.app
locks/
scripts/build-layered-local.sh
scripts/prepare-layer-seals.py
scripts/verify-layer-contract.py
```

The driver supports `--target ml`, `--target ndn`, `--target app`, and
`--target all`. It never discovers a parent through a human/floating tag.
Each parent tag is derived from the owning lock digest and is written once; the
driver resolves its local image ID, records it, and verifies the tag-to-ID
binding immediately before and after every child build. This avoids the
non-portable assumption that Buildx can use a daemon-local image ID directly in
`FROM`, while still failing closed on tag drift. BuildKit cache mounts are
scoped per dependency family. Logs and a JSON manifest go under a
caller-selected output directory.

## Validation sequence

1. Static contract and mutation tests before any expensive build.
2. Validate pinned base digests and layer-specific locks.
3. Free only the already identified rebuildable legacy builder images if disk
   reserve is insufficient; keep the accepted Spec 110 runtime.
4. Cold-build and probe ML devel/runtime.
5. Cold-build and probe NDN devel/runtime; assert ndn-svs, NDNSD, and NDNSF absent.
6. Build and probe App runtime; verify C++/Python imports and unprivileged static
   health check.
7. Scan final filesystem/context for models, secrets, sources, caches, and
   unresolved DSOs.
8. Repeat App build with a new identity, parse BuildKit metadata/logs, and prove
   stable image IDs unchanged with no base compilation.
9. Run post-implementation Spec Kit audit and reconcile every requirement.

## Migration and rollback

- No existing Dockerfile is modified until the new route passes.
- New images use `spec158-local-*` tags and cannot overwrite Spec 110 tags.
- A failed layer leaves earlier accepted images intact.
- The accepted Spec 110 runtime is deleted only after explicit final cleanup
  evidence and remains available remotely regardless.
- This feature does not push, dispatch GitHub Actions, materialize SIF, or
  submit Slurm work.

## Evidence

Durable design/audit evidence lives under this Spec. Generated local build logs
and manifests live under `results/spec158-layered-reusable-docker/<build-id>/`
and are referenced, not copied wholesale, into the completion report.
