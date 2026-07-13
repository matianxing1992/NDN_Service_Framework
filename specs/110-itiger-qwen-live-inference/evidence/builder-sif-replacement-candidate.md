# Builder-SIF replacement candidate

**Date**: 2026-07-13  
**Owner task**: T150 (candidate prepared; render remains open)  
**State**: `PREPARED_AWAITING_AUTHORIZATION`

The offline-qualified builder-SIF implementation at commit
`b50253c` was staged as minimal render/build assets under:

```text
/project/tma1/ndnsf-di/source/b50253c-diagnostic
```

The new diagnostic-only candidate is:

```text
probeId=spec110-rootless-builder-sif-probe-20260713-001
candidateId=spec110-c1-bbad7b9ac589-ab399bdad781-cb3118f6c76a-7d78d6469998-c42f3ce5a86c-f11c7a76dc7c
candidateDigest=sha256:ea8116406d2c4d13657262932188b26bc9f7ec9b2e211508d15d9a67bc2d83c6
cellId=spec110-cell-59b3596d1450f2497810
recordDigest=sha256:9591e11af354cbfaf8691376d4cee9cf584c2cb3433feecf9eed14e6e2408fa3
```

Durable candidate record:

```text
/project/tma1/ndnsf-di/campaigns/spec110/rootless-build/
  spec110-rootless-builder-sif-probe-20260713-001/candidate-identity.json
```

It binds the pinned Buildah OCI manifest, `apptainer-sif` mode, VFS/chroot,
the short CPU allocation profile, tiny diagnostic workload, and the staged
asset hashes. It links to the failed predecessor:

```text
replacesRunId=spec110-run-ae2a0ab7f17ea96d0901
replacesSubmissionId=spec110-submission-3757081f091030c057ec
```

No authorization was inferred from a generic continuation request. Therefore
`humanAuthorizationDigest`, `runId`, and `submissionId` remain null; no new
SBATCH script was rendered and `sbatch` was not called. After an explicit
authorization statement, T150 may derive the replacement run/submission
identities, bind the authorization digest, render/review exactly one script,
and stop again before T151 performs the single allowed submission.
