# Spec186 portability and ownership audit (initial)

**Captured:** 2026-09-12
**Scope:** exact baseline `575b43cc93bbed29932303caf3d09974f1585af7`; audit before adding the Spec186 launcher.

## Current path trace

| Path | Current owner | Observation | Classification |
| --- | --- | --- | --- |
| YOLO MiniNDN | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` | Python creates topology, identities and child lifecycle; native provider/requester owns the business path | intended split; candidate wrapper must preserve it |
| Qwen MiniNDN | `Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py` | native C++ runner owns model stages; Python only orchestrates topology and process lifetime | intended split; Qwen3 model identity must be explicit |
| local layered flow | `Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py` | check/prepare/run-local and ELF checks exist, but are not yet a Spec186 candidate contract | reusable primitive; needs resealed manifest |
| Tiger flow | `Experiments/TigerCluster/jobs/spec180/*` | scripts use Spec180 names, fixed defaults and host-specific paths | accidental coupling for Spec186; do not reuse wholesale |
| Tiger runtime | `Experiments/TigerCluster/runtime/*` | baseline identity/worker helpers exist; no Spec186 candidate lifecycle | reusable low-level owner only |
| old handoff lock | `Experiments/TigerCluster/development-handoff.lock.json` | source points to older `447f7584` | stale for Spec186; must be resealed |

## Hidden cross-host constraints to close

1. **Filesystem:** model, tokenizer, base SIF and app bundle paths must be
   profile inputs or staged run-root paths. A host path may identify an input
   at prepare time, but it cannot be assumed on a Tiger node.
2. **NDN transport:** NFD socket, face endpoint, route and identity store must
   be explicit per role/node. Shared writable PIB/TPM and an implicit local
   socket are forbidden.
3. **Runtime loading:** `LD_LIBRARY_PATH` is part of the candidate closure only
   when it points to the matching packaged base/app layers. Host libraries must
   not override packaged ABI dependencies; `readelf` and `ldd -r` are required.
4. **GPU:** CUDA visibility, GPU UUID and provider placement are observations,
   not assumptions. A readiness probe alone cannot close a measured inference.
5. **Lifecycle:** SSH/rsync/staging/`sbatch` must occur only after the
   pre-dispatch validator returns success. Every child has a bounded deadline,
   owner and cleanup record.

## Ownership map

| Layer | Owns | Must not own |
| --- | --- | --- |
| Core/NDN | wire protocol, security, transport primitives | YOLO/Qwen model policy |
| DI/native C++ | model loading, stage execution, typed request/response and numerical outputs | host SSH, Slurm or topology orchestration |
| application bundle | changing YOLO/Qwen app binaries and configs | replacement of base libraries |
| Tiger adapters | profile validation, staging, node/GPU mapping, launch/collect | business inference logic |
| Python harness | topology, identity setup, process lifecycle, receipts | duplicate native inference implementation |
| evidence/docs | candidate/run identity, hashes, oracle and terminal status | promoting smoke or READY to PASS |

No product source was changed during this audit. T003/T004 must implement the
missing candidate and lifecycle contracts under `Experiments/TigerCluster/`.
