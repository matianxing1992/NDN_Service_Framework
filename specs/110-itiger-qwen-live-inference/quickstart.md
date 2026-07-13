# Quickstart: iTiger NDNSF-DI distributed Qwen experiment

This is the target operator workflow after implementation. It intentionally
does not submit a job during document creation. Commands that mutate scheduler
state are marked explicit.

## 1. Connect and verify access

```bash
uofm-vpn-status
ssh -o BatchMode=yes itiger.memphis.edu 'hostname; whoami'
```

Do not place a password, Duo response, SSH private key, or model/registry token
in this repository, shell history, Slurm script, or evidence.

## 2. Use project storage

```bash
ssh itiger.memphis.edu 'mkdir -p \
  /project/$USER/ndnsf-di/{source,releases,models,artifacts,identities,campaigns,evidence}'
```

Bulk content belongs under `/project`. `/home` is small config only. Compute
scratch is selected and created by the job; on the currently observed Slurm
configuration the advertised `TmpFS` is `/scratch`.

## 3. Build and publish the OCI release with GitHub Actions

Publish every locked dependency revision, then let the workflow prepare and
verify sealed Git archives before Buildx runs the existing GPU Dockerfile with
`DEPENDENCY_SOURCE_MODE=sealed`. The successful workflow publishes
`ghcr.io/matianxing1992/ndnsf-di@sha256:<digest>` and retains only the source
seal, release manifest, SBOM, signature, checksums, and logs.

Qwen `.safetensors`, `.onnx`, `.gguf`, and related weights are excluded from the
context and remain under `/project/$USER/ndnsf-di/models` or `artifacts`.

After the workflow records a verified digest, render and review one bounded CPU
Slurm job that runs `apptainer build runtime.sif.partial
docker://ghcr.io/...@sha256:<digest>`, verifies the SIF, and atomically promotes
it below `/project/$USER/ndnsf-di/releases/<release-id>`.

If GitHub's standard runner exhausts its measured disk, stop and preserve that
failure; use a paid larger or self-hosted GitHub runner under a new build
identity. The builder-SIF route remains an iTiger fallback. Job `147712` and its
replacement candidate are not retried merely because the primary route changed.

## 4. Discover iTiger and validate the materialized SIF

```bash
ssh itiger.memphis.edu '
  cd /project/$USER/ndnsf-di/source/current &&
  tools/ndnsf-di/ndnsf-di-itiger-qwen discover \
    --output /project/$USER/ndnsf-di/campaigns/spec110/cluster.json &&
  tools/ndnsf-di/ndnsf-di-itiger-qwen release validate \
    --manifest /project/$USER/ndnsf-di/campaigns/spec110/release.json
'
```

The release manifest binds the OCI digest/archive checksum, SIF SHA-256, source
snapshot, build allocation, scratch selection, locks, and secret scan.

## 5. Validate the runtime inside one allocation

Render first, then inspect the generated Slurm request and frozen process map:

```bash
ssh itiger.memphis.edu '
  tools/ndnsf-di/ndnsf-di-itiger-qwen candidate render \
    --candidate campaigns/spec110/runtime-probe.json \
    --cell runtime-probe
'
```

Explicit submission is a crash-safe state transition, not a bare `sbatch` call.
Before invoking `sbatch`, the adapter must durably write `INTENT_RECORDED` with
the immutable submission ID, script digest, deterministic job name/comment, and
candidate/cell/run identity. It then records `SUBMITTED` plus the exact job ID,
or `SUBMISSION_UNKNOWN` if acknowledgement is lost. An unknown submission is
reconciled with `squeue`/`sacct`; it is never automatically resubmitted.

```bash
ssh itiger.memphis.edu '
  tools/ndnsf-di/ndnsf-di-itiger-qwen candidate submit \
    --rendered campaigns/spec110/rendered/runtime-probe.sbatch \
    --submission-id spec110-runtime-probe-001
'
```

The probe must verify NFD, NDNSF imports/linking, PyTorch CUDA, ONNX Runtime CUDA,
allocated UUID correlation, and the recorded selected compute scratch.

## 6. Stage and seal 0.5B

Run storage admission before transfer/export. Store source model, tokenizer,
license, manifest, and stage artifacts only in project storage. Validate the
full-model oracle and stage tensor interfaces before freezing the candidate.

```bash
ssh itiger.memphis.edu '
  tools/ndnsf-di/ndnsf-di-itiger-qwen storage admit \
    --profile campaigns/spec110/qwen2.5-0.5b.json &&
  tools/ndnsf-di/ndnsf-di-itiger-qwen candidate freeze \
    --profile campaigns/spec110/qwen2.5-0.5b.json
'
```

## 7. Execute the first single-node distributed candidate

The first candidate uses one compute node, three distinct Provider processes,
three allocated GPUs, and one job-scoped NFD. Its frozen process map binds every
Controller/User/Provider process to a Slurm task rank, GPU rank, identity, NFD
socket, command, readiness condition, and shutdown order.

For each 1-, 2-, and 32-token cell:

1. render and review the allocation and process map;
2. record the submission intent, then submit once with a unique identity;
3. reconcile/monitor only the exact job ID;
4. collect and atomically promote evidence;
5. validate the bundle before advancing.

PASS requires three distinct Provider PIDs and stage records on three allocated
GPU UUIDs, exact oracle tokens, and exactly one terminal response. It requires
one node and one NFD and therefore must not claim a cross-node edge. A pre-start
block leaves the task open. A post-start failure is retained and never
auto-retried.

## 8. Qualify and execute the multi-node 0.5B extension

First render/review one five-minute CPU-only probe using the candidate-selected
NFD transport (TCP by default):

```bash
ssh itiger.memphis.edu '
  tools/ndnsf-di/ndnsf-di-itiger-qwen network render \
    --cluster campaigns/spec110/cluster.json \
    --transport tcp \
    --output campaigns/spec110/rendered/network-probe.sbatch
'
```

The probe must show two compute nodes, one unprivileged NFD per node, selected
transport face/route state, secured generic invocation, readiness ordering, and
complete teardown. UDP may be recorded as a diagnostic but cannot block a TCP
candidate. Only after this probe and the single-node 0.5B candidate both PASS may
the separately keyed 32-token multi-node extension run. Multi-node PASS requires
at least two nodes/NFDs and at least one dependency crossing the measured NDN
face; it is compared with the exact single-node NDNSF-DI placement reference.

## 9. Scale and measure

Advance the controlled single-node three-GPU correctness ladder in order:

```text
0.5B -> 1.5B -> 3B -> 7B -> 14B -> 32B -> 72B
```

Re-run live storage, quota, and GRES admission before every size. Different
hardware or partitioning creates a new placement/candidate and only a descriptive
cross-size comparison unless all confounders match.

For every correctness-PASS size, execute three original 60-second single-node
NDNSF-DI repetitions and three hardware/artifact-matched local staged
repetitions. This measures framework overhead. For 0.5B only, three accepted
multi-node repetitions may be compared against the matched single-node NDNSF-DI
runs to measure placement/network delta. Full-model Transformers timing is an
oracle and never a performance denominator. Every repetition has an independent
submission ledger, so partial completion cannot close another cell.

## 10. Validate and clean safely

```bash
ssh itiger.memphis.edu '
  tools/ndnsf-di/ndnsf-di-itiger-qwen aggregate \
    --campaign campaigns/spec110/campaign.json \
    --output evidence/spec110-summary.json &&
  tools/ndnsf-di/ndnsf-di-itiger-qwen cleanup \
    --project /project/$USER/ndnsf-di --dry-run
'
```

Never remove an accepted bundle, active job, identity set, sealed model,
referenced stage, or current/prior release from a cleanup dry-run.
