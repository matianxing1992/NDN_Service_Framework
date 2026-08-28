# Feature Specification: iTiger Qwen3.6 Distributed Generation

**Feature Directory**: `specs/162-itiger-qwen36-generation`
**Created**: 2026-07-28
**Status**: ACTIVE_REQUALIFICATION — the user explicitly authorized the
2026-08-01 TigerCluster T009 small-Qwen requalification sequence. The original
architecture pause and Job 175053 identity remain immutable historical records;
they do not authorize unrelated new allocations.

This prospective pause does not change Job 175053's recorded `PYTHONPATH`
failure cause, terminal state, identity, or retained evidence.

## Current operational status — 2026-08-01

The pause text above is the historical pre-authorization state. A later
explicit authorization started the linked TigerCluster requalification
identities 181527, 181528, 181530, 181531, and 181532. The first four are
preserved failures caused before token generation; 181532 was still pending at
the documentation checkpoint. The prepared Qwen3.6-27B stages were published
through DistributedRepo, but no complete-generation `PASS`, five-prompt
distribution, or isolated Repo-throughput claim exists yet. See
`evidence/t009-requalification.md` for the immutable evidence boundary and
`docs/NDNSFDI/tigercluster-qwen36-operational-lessons.md` for deployment rules.

## User Scenarios & Testing

### User Story 1 - Generate a complete distributed answer (Priority: P1)

As an NDNSF-DI researcher, I want a current Qwen 3.x model to generate a
complete answer through three physical RTX 5000 nodes so that the result proves
more than a one-token forward pass.

**Independent Test**: For each frozen prompt, the distributed path returns the
same ordered token sequence and decoded text as the frozen reference, terminates
on EOS within 64 generated tokens, and correlates every token with all three
stages and both cross-node dependency transfers.

**Acceptance Scenarios**:

1. **Given** a frozen prompt and reference, **when** generation runs, **then**
   Stage 0, Stage 1, and Stage 2 execute on three different RTX 5000 nodes and
   the final answer matches the reference through EOS.
2. **Given** any CPU fallback, missing stage, missing dependency receipt,
   token mismatch, or missing EOS at the limit, **when** evidence is analyzed,
   **then** the generation is retained as unsuccessful and excluded from
   successful distributions.

### User Story 2 - Measure repeated-generation distributions (Priority: P2)

As an NDNSF-DI researcher, I want multiple generations for several realistic
prompts so that latency variation is visible rather than represented by one
request.

**Independent Test**: Five prompts each retain one warmup and five measured
generations. Raw evidence reconstructs per-prompt and pooled TTFT, inter-token
latency, total latency, output length, and tokens/s.

**Acceptance Scenarios**:

1. **Given** five frozen prompts, **when** the campaign completes, **then**
   exactly five warmups and 25 measured generation records are retained.
2. **Given** mixed outcomes, **when** summaries are produced, **then** warmup
   and unsuccessful rows remain in raw evidence but are excluded from
   successful percentiles.

### User Story 3 - Reproduce the experiment without H100 (Priority: P3)

As an operator, I want preparation and execution to use RTX 5000 allocations
only so that the experiment does not depend on the scarce H100 queue.

**Independent Test**: The reference/preparation allocation uses RTX 5000 GPUs,
the candidate uses three distinct RTX 5000 nodes, and no retained manifest or
Slurm allocation contains an H100 resource request.

## Edge Cases

- The newest official Qwen 3.x architecture is not supported by the existing
  Qwen2-only stage runtime.
- A 64-token limit may truncate an answer; truncation is evidence, not success.
- The model defaults to thinking output; the experiment requires one frozen
  direct-response mode so concise prompts can reasonably reach EOS.
- One stage may exceed a 32 GB GPU after embeddings, output head, activations,
  and runtime workspace are included even when average weight arithmetic fits.
- A queued older experiment may still exist; it is never silently cancelled,
  relabeled, or treated as part of this feature.
- A formal job may fail after the workload completes because of an analyzer
  error; scheduler and workload outcomes remain separate.

## Requirements

### Functional Requirements

- **FR-001**: The experiment SHALL use the newest verified official open-weight
  Qwen 3.x dense model that can plausibly fit a three-stage RTX 5000 layout at
  specification time, pinned to an immutable revision.
- **FR-002**: The model contract SHALL freeze text-only input, direct-response
  mode, BF16 weights, deterministic greedy decoding, EOS identifiers, chat
  template, tokenizer, and a maximum of 64 generated tokens.
- **FR-003**: The candidate SHALL use three distinct physical RTX 5000 nodes
  with exactly one stage and one allocated GPU UUID per node.
- **FR-004**: The 64 model layers SHALL be covered once without overlap or
  gaps by three frozen contiguous ranges.
- **FR-004a**: The full-model reference SHALL execute the same frozen layer
  ranges as the exported stages; a capacity-balanced placement that selects
  different layer boundaries SHALL be rejected before reference tokens are
  admitted.
- **FR-005**: Preparation and reference generation SHALL use RTX 5000
  allocations only and SHALL NOT request an H100.
- **FR-006**: Every token epoch SHALL use the normal NDNSF-DI secured
  collaboration path, including assignment, permissions, tokens, replay
  protection, operation status, two dependency transfers, and final response.
- **FR-007**: Every stage SHALL fail closed unless it executes on its allocated
  CUDA GPU with CPU fallback disabled.
- **FR-008**: Every generated token SHALL exactly match the corresponding
  frozen reference token; decoded text alone is insufficient.
- **FR-009**: A successful complete answer SHALL terminate on EOS within 64
  generated tokens.
- **FR-010**: The campaign SHALL contain five realistic prompts, one excluded
  warmup per prompt, and five sequential measured generations per prompt.
- **FR-011**: Raw evidence SHALL retain complete answer text, token IDs, TTFT,
  every inter-token latency, total latency, output length, tokens/s, prompt ID,
  repetition, and success/failure classification.
- **FR-012**: Summary evidence SHALL provide per-prompt distributions and a
  clearly labeled pooled descriptive distribution.
- **FR-013**: Warmup, truncation, mismatch, timeout, cancellation, fallback,
  and other failed rows SHALL remain evidence and SHALL NOT enter successful
  percentiles.
- **FR-014**: Model download, preparation, and inference SHALL run only inside
  bounded Slurm allocations; the login node is read-only control plane.
- **FR-015**: Formal live identities SHALL be exactly-once. No job SHALL be
  submitted, cancelled, retried, or replaced without explicit authorization.
- **FR-016**: The feature SHALL preserve the prior Qwen2.5-32B/H100 experiment
  and its queued or terminal state as a separate identity.
- **FR-017**: While the architecture dependency is open, no new `sbatch`,
  `srun`, `scancel`, `requeue`, model download, remote artifact mutation, or
  promotion SHALL occur; local implementation/tests/docs and read-only cluster
  observation remain allowed.
- **FR-018**: The live NDNSF-DI request SHALL be opened immediately after
  process and route readiness, without a fixed Provider/User settle interval.
  The requester SHALL collect ACKs first; graph inspection, split planning,
  model publication, and final Selection SHALL use that ACK snapshot. Each
  selected stage SHALL begin independently when its model and input/dependency
  are ready; no command may wait for all stages before starting Stage 0 or a
  dependency-ready later stage.

### Key Entities

- **Model Contract**: immutable model/tokenizer revision and generation policy.
- **Stage Placement**: role, layer range, host, allocated GPU UUID, and device.
- **Prompt Case**: stable ID, message content, formatted input IDs, reference
  output tokens/text, and EOS classification.
- **Token Epoch**: generation/request identity, input context, returned token,
  three stage receipts, two dependency receipts, and timing. In the production
  invocation model this is an internal data-plane decode step, not a separate
  NDNSF Request/ACK/Selection lifecycle.
- **Generation Record**: prompt/repetition/phase, ordered epochs, full answer,
  stop reason, TTFT, total latency, output length, and tokens/s.
- **Campaign Summary**: per-prompt and pooled descriptive distributions derived
  from retained raw records.

## Assumptions

- The experiment is text-only even if the selected official checkpoint also
  contains a vision encoder.
- Short, constrained prompts and direct-response mode are used because the user
  fixed a 64-token output ceiling.
- Full-context `use_cache=False` recomputation remains the initial correctness
  baseline; distributed KV-cache and MTP are outside scope.
- Three GPUs on one RTX 5000 node may be used for the full-model reference,
  while the NDNSF-DI candidate uses three different nodes.

## Success Criteria

- **SC-001**: All five prompts produce an exact EOS-terminated distributed
  answer within 64 generated tokens in every accepted measured repetition.
- **SC-002**: Every accepted token has exactly three CUDA stage receipts and
  two checksum-matched cross-node dependency receipts.
- **SC-003**: The campaign retains five warmups and 25 measured generation
  records without deleting unsuccessful outcomes.
- **SC-004**: TTFT, every inter-token latency, total latency, output length, and
  tokens/s can be recomputed from raw evidence for every measured generation.
- **SC-005**: All accepted stages use three distinct RTX 5000 nodes and GPU
  UUIDs with zero CPU fallback.
- **SC-006**: No preparation or candidate manifest requests or records H100.
- **SC-007**: The final report makes no unsupported KV-cache, MTP, throughput
  scaling, tensor-parallel, or production-readiness claim.
