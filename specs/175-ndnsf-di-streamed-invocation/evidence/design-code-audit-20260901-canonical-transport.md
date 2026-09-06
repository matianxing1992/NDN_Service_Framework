# Spec175 design-to-code audit: canonical transport repair

**Date**: 2026-09-01  
**Scope**: the post-G4 canonical-artifact publication repair, its production
callers, and the resulting local qualification boundary.

## Verdict

**PASS for the repaired implementation boundary and local G0--G3
qualification.** The previous `20260901-v3-boundary-fix` source seal and its
G0--G3 manifests were created before this behavior-affecting repair. They
remain historical and cannot authorize a new SIF or G4 replay. The new
source-bound G0--G3 records are listed in
`evidence/t020-t022-canonical-transport-20260901.md`.

## Controlling finding and repair

The v5 exact-SIF replay selected canonical `/ndnsf-di/...` identities in V3,
but the tiny and Qwen publication paths still carried repository/encrypted
transport names as if they were the canonical `assignedArtifact`. Providers
therefore attempted to fetch an unpublished canonical name and received an
NAC-ABE Nack after Selection.

The repaired path now has two explicit names for every published role:

1. `artifact_data_names_by_role` is the stable, content-bound canonical
   identity (`assignedArtifact`).
2. `artifact_fetch_data_names_by_role` is the routable encrypted Data name
   carried as Selection `artifactDataName`.

`AutomaticPlanningCoordinator` certifies the draft role recipe, invokes the
request-scoped canonical ensurer only after `ACK_CLOSED`, re-certifies the
exact root digest returned by publication, and seals both names.  The native
Provider prefetches the transport name and stores the verified bytes under the
canonical identity before `NativeCanonicalOnnxAssembler` resolves the root.
The tiny V3 ensurer publishes the ONNX source and ACTIVE root through
`publish_encrypted_large_data`, and the Qwen registration adapter now maps
`canonicalName` to the identity and `objectName` to the transport reference.

## Evidence

- `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py`:
  separate publication/fetch maps, post-publication binding, and ordinary V3
  proposal validation.
- `NDNSF-DistributedInference/ndnsf_distributed_inference/plan.py`:
  sealed plan digest includes the transport map while preserving canonical
  role identities.
- `examples/python/NDNSF-DistributedInference/llm_pipeline/user.py`:
  request-scoped tiny source/root publisher and Qwen registration mapping.
- `tests/python/test_spec175_canonical_transport.py`: canonical-vs-transport
  regressions for legacy defaults, tiny root/source publication, and Qwen
  registration records.

Focused verification after the repair:

```text
PYTHONPATH=NDNSF-DistributedInference:NDNSF-DistributedRepo/pythonWrapper:pythonWrapper \
  python3 -m pytest -q tests/python/test_spec175_*.py
333 passed, 1 skipped
```

The structural Spec Kit audit remains `PASS` (82 FR, 21 SC, 42 tasks, 36
currently closed, 6 open after the T020/T022 rerun, all FRs traced). The G0--G3
qualification rerun is recorded separately in
`evidence/t020-t022-canonical-transport-20260901.md`; no SIF, CUDA run, Slurm
job, or Tiger submission was started after this repair.

## Remaining qualification

The next valid sequence is one exact SIF build/preflight from the new source
seal, then a G4 replay whose route probe includes the exact post-Selection
canonical root/source fetch. Only that candidate may proceed to
T025/T026/T034/T027. The current v5 SIF and all earlier downstream results are
diagnostic-only.
