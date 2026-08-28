# Contract: Default Local Deployment Gate CLI

## Entry point

```text
python3 Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py
```

Without a subset flag, the command runs every mandatory Gate A-D case.

## Frozen default profile

- Qwen model: `Qwen/Qwen3-0.6B`
- revision: `e6de91484c29aa9480d55605af694f39b081c455`
- download: disabled
- prompts: two fixed non-empty prompts
- warmup: one per prompt
- measured: three per prompt
- minimum retained generated tokens: eight per measured invocation
- MiniNDN roles: one User, one Controller/security carrier, at least three
  Providers
- container workload: identical workload digest
- TigerCluster: disabled

## Outputs

The command creates a unique run directory and writes:

- canonical workload manifest;
- one fidelity record per case;
- MiniNDN measurements and lineage;
- container measurements, lineage, image/resource/OOM evidence;
- deadline negative matrix;
- `aggregate-verdict.json`;
- `summary.md`.

The command returns success only if the two summaries agree and every mandatory
case passes.

## Safety

The command does not download mutable model data, submit remote jobs, erase
prior results, or silently fall back between CPU and GPU. A missing model,
container image, backend, MiniNDN dependency, or required evidence is a
nonzero result with an actionable reason.
