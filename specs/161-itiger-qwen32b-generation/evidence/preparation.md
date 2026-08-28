# Spec 161 Qwen 32B Preparation Evidence

## Current status

`SUBMITTED_PENDING_RESOURCES`

T004 is authorized and in progress. It is not complete.

## Exactly-once H100 preparation identity

- Submission ID: `spec161-qwen32b-prep-001-bd7766adcc13`
- Run ID: `spec161-run-qwen32b-prep-001`
- Slurm job: `174382`
- Submitted state: `PENDING (Resources)`
- Slurm estimated start at last observation:
  `2026-07-28T23:58:57`
- Requested allocation:
  - partition `bigTiger`;
  - account `devs`;
  - QOS `normal`;
  - one node;
  - one `h100_80gb` GPU;
  - eight CPUs;
  - 256 GiB host memory;
  - four-hour walltime.
- Source bundle SHA-256:
  `bd7766adcc1373da41ea7db083457d96b59c916a0f4b7883bc1975b8a03c2a59`
- Read-only source root:
  `/project/tma1/ndnsf-di/spec161/source/spec161-qwen32b-prep-001-bd7766adcc13/root`
- Runtime SIF:
  `/project/tma1/ndnsf-di/releases/spec160-af0c3a3ce357-operation-status/runtime.sif`
- Runtime SIF SHA-256:
  `6638c813016e151fd6c19825db8dc01e3a326c23716084677eee3c4da935d52e`
- Capacity decision SHA-256:
  `e072f484e688ebee7b7a36a32f9695aadf684ca25b40c59f0ae5cac5857da671`

## Pending execution gates

When Job 174382 starts, the same allocation must:

1. revalidate scratch capacity and the frozen capacity decision;
2. download revision
   `5ede1c97bbab6ce5cda5812749b4c0bdf79b18dd` only into job scratch;
3. run five H100 full-context, `use_cache=False`, greedy references to EOS;
4. construct `[0,21)`, `[21,42)`, and `[42,64)` FP16 stage packages;
5. load each stage on CUDA and fail closed on any fallback;
6. recheck exact promotion bytes plus the 20 GiB project reserve;
7. promote only tokenizer, stages, policy, manifests, reference, and evidence;
8. remove only this job's validated scratch prefix after successful promotion.

No model byte had been downloaded and no GPU compute had started at the last
recorded `PENDING` observation.

## Local harness evidence

- Spec 161 focused tests: `12/12` passed.
- Python compilation checks: passed.
- H100 preparation and three-node smoke Bash syntax checks: passed.
- Synthetic smoke correlation proves the analyzer requires three stage receipts
  and two dependency receipts for every generated token.
- Formal T006 campaign remains unauthorized and unsubmitted.
