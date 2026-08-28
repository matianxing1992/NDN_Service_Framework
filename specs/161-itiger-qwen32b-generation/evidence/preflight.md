# Spec 161 Preflight Evidence

## Current status

`CAPACITY_PASS_AWAITING_PREPARATION_AUTHORIZATION`

No 32B model download, reference inference, stage preparation, or formal
distributed job has been submitted.

The bounded no-download scratch-capacity probe was submitted exactly once as
Slurm job `174363`. No model preparation, reference, GPU, or distributed
inference job has been submitted.

## Read-only discovery

- Account: `tma1`
- Cluster: iTiger `bigTiger`
- Association limit: 3 nodes, 24 GPUs
- Available GPU classes discovered: H100 80GB, RTX 6000, RTX 5000
- Project root: `/project/tma1/ndnsf-di`
- Measured current project-root usage: 67 GiB
- Measured retained release usage: about 29 GiB
- Account-specific `quota` command: unavailable
- Conservative project quota authority: University of Memphis HPC Storage
  policy states that `/project/$USER` initially has a 1 TB quota
- Estimated Qwen2.5-32B FP16 source weights: 60.54 GiB
- Estimated original all-durable preparation peak: 147.26 GiB

## Executed scratch-capacity probe

- Submission ID:
  `spec161-capacity-20260728T091249Z-2675b7cf`
- Slurm job: `174363`
- Node: `itiger05`
- Terminal state: `COMPLETED`, exit `0:0`, elapsed `00:00:02`
- Script SHA-256:
  `2675b7cf0775a05f103cebc16beb7a118bc47a376ee8591a64ec75a57a680913`
- Selected job-scoped path: `/tmp/tma1/ndnsf-di/spec161-174363`
- Filesystem: allocation-namespaced XFS on node-local NVMe
- Total bytes: `15358812467200`
- Free bytes: `15251694034944`
- Fsync probe: `67108864` bytes, passed
- Download performed: `false`
- Model compute performed: `false`
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec161/capacity/spec161-capacity-20260728T091249Z-2675b7cf`
- Evidence checksums: all five recorded files passed `sha256sum -c`
- Partial directory after completion: absent
- Job scratch prefix after completion: removed by the bounded job-local cleanup
- Capacity input SHA-256:
  `d37d4d7f7525cccbca15b0024ca23b6bc79e0fcbeb0a33ec4378544ce6f3dc07`
- Capacity decision SHA-256:
  `e072f484e688ebee7b7a36a32f9695aadf684ca25b40c59f0ae5cac5857da671`
- The capacity input and decision were promoted read-only (`0400`) into the
  same durable evidence directory after their hashes were verified.

## Revised TigerCluster storage design

Temporary preparation belongs in allocation-scoped TigerCluster scratch:

1. `$SLURM_TMPDIR`;
2. `/scratch`;
3. `/tmp`.

The authorized preparation job must prove its selected path is writable and has
enough free bytes before download. The full source model, Hugging Face cache,
standalone-reference workspace, and stage construction remain there.

Only checksum-verified final stage packages, tokenizer/chat-template artifact,
manifests, runtime identity, and evidence are promoted to
`/project/tma1/ndnsf-di`. The public, reconstructible full source-model copy is
not promoted. After promotion and CUDA validation, the job may remove only its
own validated scratch prefix.

## Capacity decision

The checksum-bound capacity input uses:

- current durable usage: `71924419072` bytes;
- conservative verified quota: `1000000000000` bytes;
- projected stage promotion: `69793218560` bytes;
- projected evidence: `1073741824` bytes;
- additional runtime: `0` bytes because the retained runtime is already
  included in current usage;
- reserve: `21474836480` bytes;
- projected durable usage with reserve: `164266215936` bytes;
- temporary scratch peak: `158119221002` bytes.

This produces `allowed=true`. Exact promoted stage/tokenizer sizes must still be
checked against the same reserve before promotion.

## Remaining authorization and execution gates

- T004 was authorized. H100 preparation Job `174382` was submitted exactly once
  and is currently pending resources; see `evidence/preparation.md`.
- T004 model download, H100 reference, stage construction, and development
  smoke have not yet executed.
- Final stage-package and evidence sizes remain projections until T004 writes
  them.
- No cleanup target is authorized. Existing releases and evidence remain
  untouched.

## Local implementation evidence

Completed without model weights or cluster submission:

- fail-closed capacity decision separating allocation-scratch peak from durable
  promotion capacity;
- bounded, CPU-only, no-download scratch probe definition with validated
  job-local cleanup;
- 64-token, EOS-required, full-sequence greedy generation state machine;
- requester integration that submits the complete growing context through the
  normal collaboration request once per generated token;
- five-prompt campaign scheduler with one warmup plus five measured attempts;
- raw JSONL analyzer retaining failures and reporting per-prompt plus
  descriptive pooled p50/p95 without p99.

Focused verification on 2026-07-28:

```text
test_spec161_qwen_generation.py: 9 passed
test_ndnsf_di_deployment_readiness.py: 21 passed
test_ndnsf_di_spec107_attribution.py: 13 passed
test_spec111_llm_pipeline_pacing.py: 2 passed
test_spec160_qwen_stage_device.py: 3 passed
Python compilation checks: passed
scratch-capacity-probe.sbatch shell syntax: passed
Spec 161 strict structural audit: passed
```

These results establish only local implementation behavior. They do not
establish scratch capacity, 32B model preparation, H100 reference tokens,
three-node RTX 5000 placement, CUDA execution, or formal campaign results.
