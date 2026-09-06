# Spec170 sealed-source staging preflight — 2026-08-16

## Why this gate exists

The source-seal creator writes dependency archives at the staging root, but
`rootless-build.sh` reads them from `.spec110-build/archives`. The first r8
submission therefore passed source creation and failed only after Slurm started
with `ROOTLESS_BUILD_DEPENDENCY_ARCHIVE_MISSING:NAC-ABE`.

The skill now includes
`~/.codex/skills/itiger-ndnsf-ops/scripts/validate-sealed-source-root.py`.
It validates the exact final staging directory before dispatch, rather than
assuming that a successful seal or an earlier copy implies a valid layout.

## Validator behavior

The validator checks:

- source-seal schema and recomputed body digest;
- workspace archive size and SHA-256;
- dependency lock SHA-256 when supplied;
- byte-identical root and `.spec110-build/source-seal.json`;
- every dependency archive's size, SHA-256, and byte equality in both
  `archives/` and `.spec110-build/archives`.

The same command is run against local staging and the remote project staging
path. Both reports must agree on the source revision, seal digest, dependency
count, and workspace digest.

## Results

The intentionally incomplete local staging layout failed closed with:

```text
SEALED_SOURCE_ROOT_MIRROR_MANIFEST_MISSING
```

After the mirror was materialized, local validation passed:

```text
sourceRevision: 07738d708d36596a14f1988db4395f1c2bf33fcd
sealDigest: sha256:5f3ec949a995ce74cf27b8079dbc154c24c8117786e10c62edeba20ec55b972d
dependencyCount: 8
workspaceDigest: sha256:3f86c7a095b1d6c5ebdcca3cc606e60b4680d75984905d79f302222667ccce01
status: PASS
```

The remote staging path
`/project/tma1/ndnsf-di/staging/spec170-07738d70-source-sealed-20260816`
also passed with the same values. This closes the staging-layout defect; it
does not replace full-build, SIF-hash, or runtime request/response gates.
