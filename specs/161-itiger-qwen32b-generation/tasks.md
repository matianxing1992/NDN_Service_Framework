# Tasks: iTiger Qwen 32B Multi-Generation Campaign

**Input**: Design documents from
`specs/161-itiger-qwen32b-generation/`

**Prerequisites**: `spec.md`, `plan.md`, `research.md`, `data-model.md`,
`contracts/`, and `quickstart.md`

## Phase 1: Setup and frozen contracts

- [x] T001 Freeze the Qwen2.5-32B-Instruct revision, FP16/no-quantization
  boundary, five-prompt chat-template contract, 64-token EOS policy,
  three-stage placement, repetition policy, and no-live-execution gate in
  `specs/161-itiger-qwen32b-generation/`.

## Phase 2: Foundational capacity gate

- [x] T002 Implement a fail-closed, read-only capacity decision with tests and
  preflight evidence in
  `specs/161-itiger-qwen32b-generation/jobs/capacity-preflight.py`,
  `specs/161-itiger-qwen32b-generation/jobs/scratch-capacity-probe.sbatch`,
  `tests/python/test_spec161_qwen_generation.py`, and
  `specs/161-itiger-qwen32b-generation/evidence/preflight.md`; separately
  define the bounded no-download Slurm scratch probe required to measure writable
  `$SLURM_TMPDIR`/`/scratch` capacity for the temporary
  source/reference/build peak, and require measured project-space capacity for
  only the promoted stages/tokenizer/manifests/evidence plus 20 GiB reserve
  before any download or inference; produce an explicit non-destructive cleanup
  inventory when blocked.

## Phase 3: User Story 1 - Complete distributed answer (Priority: P1)

**Goal**: Generate and verify an EOS-terminated answer rather than one token.

**Independent Test**: A pure callback-driven fixture appends each returned
token to the next context, stops on EOS within 64 tokens, decodes text, and
classifies truncation/mismatch without model weights.

- [x] T003 [US1] Deliver bounded full-sequence greedy generation by first adding
  failing EOS/context-growth/mismatch/truncation tests, then implementing the
  reusable state machine and opt-in requester integration in
  `tests/python/test_spec161_qwen_generation.py`,
  `examples/python/NDNSF-DistributedInference/llm_pipeline/llm_pipeline_lib.py`,
  `examples/python/NDNSF-DistributedInference/llm_pipeline/user.py`, and
  `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen/pilot.py`;
  close with the focused local test and retain the existing one-token default.

- [ ] T004 [US1] After T002 passes and the user explicitly authorizes live
  preparation, use allocation-scoped TigerCluster scratch for the public
  full-model download, H100 chat-template reference, and stage construction;
  promote only the tokenizer, 64-layer stage artifacts `[0,21)`, `[21,42)`,
  `[42,64)`, manifests, and evidence after checksum/CUDA validation, then run
  one three-node complete-generation development smoke through
  `specs/161-itiger-qwen32b-generation/jobs/prepare-reference.sbatch`,
  `prepare-stages.sbatch`, and the generation rank harness; remove only the
  current job's validated scratch prefix after promotion and preserve any first
  failure without retry.

## Phase 4: User Story 2 - Repeated-inference distribution (Priority: P1)

**Goal**: Produce 25 independent measured generation records and reproducible
per-prompt distributions.

**Independent Test**: A synthetic five-prompt fixture produces one warmup plus
five measured records per prompt; the analyzer retains all rows, excludes
warmup/failures from success percentiles, and reproduces the raw statistics.

- [x] T005 [US2] Deliver the sequential five-prompt campaign and evidence
  analyzer by adding failing count/filter/percentile/correlation tests,
  implementing stable generation/token request identities and JSONL records in
  `examples/python/NDNSF-DistributedInference/llm_pipeline/user.py`,
  `specs/161-itiger-qwen32b-generation/jobs/analyze-generation.py`, and
  `tests/python/test_spec161_qwen_generation.py`, and closing a local fake
  one-warmup-plus-five-measured smoke without loading Qwen weights.

- [ ] T006 [US2] After T004 passes and the user explicitly authorizes the formal
  campaign, exactly-once run five prompts with one excluded warmup and five
  measured generations each through
  `specs/161-itiger-qwen32b-generation/jobs/generation-campaign.sbatch`;
  preserve all 30 generation records, verify 25 measured full-token matches,
  correlate every token epoch to three CUDA stages and two dependency transfers,
  and report per-prompt plus descriptive pooled distributions.

## Phase 5: User Story 3 - Auditable capacity-safe closure (Priority: P2)

**Goal**: Make the final allow/block and evidence status independently auditable.

**Independent Test**: Checksums, capacity inputs, job identities, raw records,
summary derivation, credential scan, and retained failures reproduce the final
status without trusting task checkboxes.

- [ ] T007 [US3] Audit Spec 161 against every FR/SC, verify all artifact and
  evidence checksums, prove zero retained credentials and no implicit cleanup,
  classify claims as proposed/implemented/executed/measured, and write the
  final traceability and completion status under
  `specs/161-itiger-qwen32b-generation/` without upgrading a failed or
  unsubmitted live gate.

## Dependencies and execution order

`T001 -> T002 -> T003 -> T004 -> T005 -> T006 -> T007`

- T002 blocks all model download and live execution.
- T003 and T005 may be implemented and tested locally without model weights,
  but T004 must pass before formal T006.
- T004 and T006 each require a separate explicit live authorization.
- No task authorizes deletion, automatic retry, Git commit, or push.

## Task cohesion review

- T002 keeps preflight tests, implementation, and evidence together.
- T003 keeps generation tests, reusable behavior, requester wiring, and focused
  validation together.
- T005 keeps campaign scheduling, analyzer behavior, and local smoke together.
- Live preparation (T004) and formal measured execution (T006) remain separate
  because they use different allocations, risks, identities, and acceptance
  evidence.

## MVP

T001 through T003: a fully testable, opt-in 64-token EOS generation path with no
32B download or live GPU submission.
