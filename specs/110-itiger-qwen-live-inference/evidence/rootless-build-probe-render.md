# Rootless build probe render evidence

**Date**: 2026-07-13
**Task**: T045
**Verdict**: `PASS` for login discovery and render/review only; no Slurm job was
submitted and compute capability remains `UNVERIFIED` pending T046.

## Live discovery

Durable remote artifact:

```text
/project/tma1/ndnsf-di/campaigns/spec110/build-discovery.json
recordDigest=sha256:27dda6b76f0c5ead56329816e80849f04c2d579929bf7eb056b24367a10f1d28
```

Observed facts:

- account `devs`, partition `bigTiger`, QOS `normal`;
- Slurm 24.05.2, configured `TmpFS=/scratch`, node `TmpDisk=14545150`;
- rootless Podman 5.2.2, Buildah 1.33.7, Apptainer 1.3.4;
- login Podman graphroot `/home/tma1/.local/share/containers/storage` is
  forbidden for the probe;
- project shared `df` is explicitly not treated as a user quota;
- credentials recorded: false;
- compute observation: `UNVERIFIED`, owner T046.

## Reviewed job

Diagnostic identity:
`spec110-rootless-probe-20260713-002` (`diagnosticOnly=true`, no candidate or
production authority).

```text
script=/project/tma1/ndnsf-di/campaigns/spec110/rendered/spec110-rootless-probe-20260713-002.sbatch
scriptSha256=sha256:1186d9401193e677d0263bbd5afbb5f2a6bc6aeb41c4c7618b9139384ae5d421
renderRecordSha256=sha256:436085618a4e1b91b66ecd64228612dbe8ffb0a3d85bd204b5ed2dcd29d96213
builderSha256=sha256:b3806efd7162dafcf5269f21f1b2750bccbb15f50faf48de57dbd2e26d2f1bcf
ociInspectorSha256=sha256:dd11f649ceaeadfc8c0aa715de0fd48571973ddc910a8685d9cd5ec42400727d
```

Review confirmed one node/task, 4 CPUs, 8 GB memory, five-minute wall time,
`devs/bigTiger/normal`, no GRES, no embedded `sbatch`, pinned Alpine/CUDA base
digests, project evidence paths, and render-owned read-only builder assets.

The earlier `...-001` render is preserved remotely as `SUPERSEDED_NOT_SUBMITTED`:
it exposed two pre-submit operator defects (an eager `jsonschema` import and a
mutable builder reference). Both were fixed and regression-tested before the
new identity was rendered. Neither render attempted scheduler submission.
Its adjacent supersession record has digest
`sha256:25fd29db871146d704e2d4472600e79210c95fb0748c770abcae93b5c42eb778`.
