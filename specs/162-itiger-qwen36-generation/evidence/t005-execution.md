# T005 Execution Ledger

## Capacity authority revision

On 2026-07-28 the user explicitly authorized the measured global free space of
the `/project` NFS filesystem as the storage authority for this development
experiment. This does not assert a per-user or per-project quota.

```text
filesystem=itigercage-ibnet:/project
filesystemType=nfs4
availableBytes=925521711988736
currentProjectTreeBytes=80896185622
additionalDurableBytes=77309411328
additionalDurableWithReserveBytes=98784247808
verifiedQuotaBytes=0
decision=allowed
decisionSha256=ee15eabdf338f5d1c9709bfa7a3401d20ca1d6274e468fd426749be532bf5e13
```

The original `t004-capacity-decision.json` remains unchanged and blocked.
The revised decision is `t005-capacity-decision.json`.

## First authorized preparation identity

```text
jobId=175053
runId=spec162-qwen36-t005-b2f0e3412bff-001
submissionId=spec162-submission-prep-b2f0e3412bff-001
sourceSha256=b2f0e3412bffca8ad0611891c30c542555e68041c48c02e540f637e8d5ab730c
sifSha256=6bb55d85bd1244e30d0233d84c753be887e527695422ba47df9ec89c20c1d072
node=itiger07
resources=1 node, 3 RTX 5000 GPUs, 256 GiB, 8 CPUs
state=FAILED
exitCode=1:0
elapsed=00:00:39
modelDownloaded=false
modelComputePerformed=false
modelSourcePromoted=false
```

The source manifest, SIF, capacity decision, three GPU identities, and scratch
capacity passed before the failure. The preparation process then stopped at:

```text
ModuleNotFoundError: No module named 'ndnsf_distributed_inference'
```

The frozen job exposed `/source/llm_pipeline` but omitted the installed SDK
path `/opt/ndnsf-app/python` from `PYTHONPATH`. Durable failure evidence is
under:

```text
/project/tma1/ndnsf-di/evidence/spec162/qwen36-prep/.spec162-submission-prep-b2f0e3412bff-001.partial/
results/spec162-itiger-qwen36-generation/prep-b2f0e3412bff-001/
```

No final artifact directory exists. The write-once empty partial artifact
directory is retained and was not deleted:

```text
/project/tma1/ndnsf-di/artifacts/spec162/qwen36/.spec162-submission-prep-b2f0e3412bff-001.partial/
```

## Corrected but unsubmitted replacement

The local preparation script now exposes both:

```text
PYTHONPATH=/source/llm_pipeline:/opt/ndnsf-app/python
HOME=/home/${USER}
```

The exact OCI runtime passed a non-root import of
`ndnsf_distributed_inference`, `PlannerKind`, and `llm_pipeline_lib`; the eight
focused preparation contracts and shell syntax also pass. This correction has
not been submitted. A replacement preparation job requires a new source
digest, new write-once identity, and new explicit authorization. The linked
three-node smoke remains unsubmitted.

Spec 161 Job 174382 was not cancelled, retried, or modified. At the read-only
refresh before Job 175053 submission it had naturally reached `FAILED 1:0`.

## Architecture pause disposition

The following disposition was recorded prospectively after reviewing the
dissertation-scoped NDNSF-DI architecture. It does not alter the failure cause
or any evidence above.

```text
disposition=PAUSED_DEPENDENCY
dependency=specs/163-di-collaboration-planning
job175053=IMMUTABLE_FAILED
job175053FailureCause=MISSING_INSTALLED_SDK_PYTHONPATH
correctedPreArchitectureReplacement=RETIRED_UNSUBMITTED
priorAuthorizationCarriesForward=false
evidenceDeletedOrRelabeled=false
```

No replacement preparation or linked smoke may be submitted until Spec 163 and
the fresh Spec 162 requalification gate pass and a new live identity is
explicitly authorized.
