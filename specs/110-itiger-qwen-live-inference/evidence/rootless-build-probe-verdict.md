# Rootless compute-builder probe verdict

**Date**: 2026-07-13
**Task**: T046
**Verdict**: `EXECUTED_FAIL`

## Immutable identity

```text
probeId=spec110-rootless-probe-20260713-002
submissionId=spec110-submission-3757081f091030c057ec
runId=spec110-run-ae2a0ab7f17ea96d0901
cellId=spec110-cell-1ead00e324e409ed05cc
jobId=147712
scriptSha256=sha256:1186d9401193e677d0263bbd5afbb5f2a6bc6aeb41c4c7618b9139384ae5d421
diagnosticIdentityDigest=sha256:506362703dd9cce459d47f570a9545b8d9740c4729d940e5af1f6f63a8bf3107
```

The crash-safe journal durably recorded `INTENT_RECORDED` before one `sbatch`
call, then `SUBMITTED` with job 147712. No second submission was made.

## Measured outcome

Slurm allocated one node, four CPUs, and 8 GB on `itiger01` under
`devs/bigTiger/normal`. The job started and terminated in the batch step with:

```text
state=FAILED
exitCode=4:0
reasonCode=ROOTLESS_BUILD_TOOL_MISSING:podman
selectedScratch=/tmp/tma1/ndnsf-di/147712/spec110-rootless-probe-20260713-002
scratchSource=tmp-fallback
```

Manifest:

```text
/project/tma1/ndnsf-di/campaigns/spec110/rootless-build/spec110-rootless-probe-20260713-002/manifest.json
recordDigest=sha256:16f1588a00082531c2d718b4cacd48b10a4605afd87b7a9785c4776970f81b17
```

No OCI archive or SIF was created. Scratch was removed and no release path was
promoted. Login-node Podman 5.2.2/Buildah 1.33.7 therefore cannot be treated as
compute-node capability. Slurm's advertised `TmpFS=/scratch` also did not yield
a writable `/scratch` path to this job; the adapter correctly selected and
recorded its `/tmp` fallback.

## Consequence

This is a valid post-allocation operational negative, not a pre-start blocker
and not an inference result. T046 is closed as executed FAIL; T047 remains locked.
The same identity must never be retried. T148-T151 own a separately pinned
builder-SIF fallback and a new replacement diagnostic. T151 requires explicit
human authorization before submission. `candidateExperiment` and
`physicalProduction` remain ineligible/DEFERRED.
