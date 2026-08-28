# Data Model

## Candidate

Fields: local image ID, unique tag, OCI digest, source authority, creation time.

## Materialization

Fields: candidate identity, Slurm job ID, node, SIF path, SIF SHA-256,
Apptainer version, terminal status.

## GPU run

Fields: run identity, predecessor materialization, job/node/GPU UUID, model
revision, prompt digest, backend/provider, generated tokens, timing, terminal
status, evidence paths.

## State transitions

```text
DISCOVERED -> OCI_PUBLISHED -> SIF_VERIFIED
  -> GPU_RUNTIME_PASS -> STANDALONE_PASS -> NDNSF_DI_PASS
```

Any post-start failure is terminal for that run identity.
