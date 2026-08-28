# Feature Specification: iTiger Qwen 32B Multi-Generation Campaign

**Feature Directory**: `161-itiger-qwen32b-generation`

**Created**: 2026-07-28

**Status**: In Progress — local implementation complete; live capacity gate blocked

**Input**: Run a model large enough to require three RTX 5000 GPUs, use real
questions, generate complete answers rather than one token, and execute enough
independent inferences in one experiment to expose the latency distribution.

## User Scenarios & Testing

### User Story 1 - Receive a complete distributed answer (Priority: P1)

As a researcher, I can submit a real question to a 32B instruction model whose
layers are split across three physical GPU nodes and receive the complete
decoded answer through the normal secured NDNSF-DI collaboration path.

**Why this priority**: A single next-token match proves only one forward pass;
it does not demonstrate useful language generation.

**Independent Test**: Submit one frozen prompt, generate greedily until the
model emits an end-of-sequence token within 64 new tokens, and verify the full
token sequence and decoded text against the frozen same-model reference.

**Acceptance Scenarios**:

1. **Given** a capacity-qualified three-node allocation and a frozen model,
   tokenizer, prompt, and reference, **When** one inference is submitted,
   **Then** all three stages execute on distinct allocated GPUs and the
   requester receives a non-empty, end-of-sequence-terminated decoded answer.
2. **Given** the distributed answer, **When** it is compared with the reference,
   **Then** every generated token matches in order, not only the first or last
   token.
3. **Given** a generation reaches 64 tokens without an end-of-sequence token,
   **When** the result is classified, **Then** it is retained as `TRUNCATED`
   and does not count as a complete-answer pass.

---

### User Story 2 - Observe a repeated-inference distribution (Priority: P1)

As a researcher, I can run five real prompts five measured times each in one
campaign and inspect raw and summarized timing data without confusing warmup,
failed, truncated, or mismatched generations with successful samples.

**Why this priority**: One successful request cannot characterize runtime
variation or tail latency.

**Independent Test**: Run one excluded warmup and five measured generations for
each of the five frozen prompts, then verify that all 25 measured sample records
are independently identified and that summaries can be reproduced from the raw
records.

**Acceptance Scenarios**:

1. **Given** the frozen five-prompt set, **When** the campaign executes,
   **Then** each prompt has exactly one excluded warmup and five independently
   identified measured attempts.
2. **Given** the raw measured samples, **When** the report is generated,
   **Then** it reports completion rate, time to first token, inter-token
   latency, end-to-end latency, generated tokens per second, and p50/p95 while
   retaining every failed or truncated sample.
3. **Given** prompts with different input or output lengths, **When** results
   are summarized, **Then** per-prompt distributions remain separate and any
   pooled campaign distribution is explicitly labeled descriptive.

---

### User Story 3 - Run only a capacity-safe, auditable campaign (Priority: P2)

As an operator, I can determine before download or allocation whether the model,
stage artifacts, runtime, evidence, and reserve fit durable storage and whether
the requested three-GPU placement is feasible.

**Why this priority**: The current project storage already contains prior
images and evidence; a large model must not be staged by exhausting shared
storage or by deleting evidence implicitly.

**Independent Test**: Produce a preflight manifest that binds measured current
usage, projected peak bytes, retained reserve, immutable artifact identities,
allocation limits, and an allow/block decision without downloading model
weights or submitting a GPU job.

**Acceptance Scenarios**:

1. **Given** unavailable or insufficient quota evidence, **When** preflight is
   evaluated, **Then** model download and GPU submission remain blocked.
2. **Given** a passing storage and allocation preflight, **When** artifacts are
   staged, **Then** every promoted model, tokenizer, stage, runtime, prompt-set,
   and reference artifact is checksum-bound and read-only during acceptance.
3. **Given** a started failure, **When** an operator considers another attempt,
   **Then** the failure remains immutable evidence and any replacement uses a
   new linked identity with explicit authorization.

### Edge Cases

- The model source fits, but source plus three stage packages, temporary files,
  and reserve do not fit simultaneously.
- One stage is placed on CPU, on the wrong GPU, or on the same physical node as
  another stage.
- A generated token matches initially but diverges from the reference later.
- The model emits an end-of-sequence token immediately and the decoded answer
  is empty.
- A generation reaches the 64-token ceiling without end-of-sequence.
- One prompt has a much longer tokenized input or output than the others.
- A measured request fails after producing some tokens.
- A summary omits a failed sample or includes warmup as measured data.
- The requester receives a correct answer but stage/dependency evidence cannot
  be correlated to that generation and token epoch.
- The scheduler grants fewer than three nodes or the required GPU class changes.

## Requirements

### Functional Requirements

- **FR-001**: The campaign SHALL use `Qwen/Qwen2.5-32B-Instruct` at one frozen
  immutable revision, its matching tokenizer, and unquantized FP16 weights.
- **FR-002**: Distributed acceptance SHALL use exactly three distinct physical
  nodes with exactly one RTX 5000 GPU and one model stage on each node.
- **FR-003**: The three stages SHALL cover every model layer exactly once,
  without overlap or gaps, and each stage SHALL fail closed on CPU fallback.
- **FR-004**: Prompts SHALL use the model's frozen instruction/chat formatting
  and the same formatted input SHALL be used by reference and distributed runs.
- **FR-005**: Each generation SHALL use deterministic greedy decoding, append
  each generated token to the next token step, stop on any frozen
  end-of-sequence token, and enforce a 64-new-token ceiling.
- **FR-006**: A complete answer SHALL contain at least one non-special generated
  token, terminate by end-of-sequence, and expose both ordered generated token
  IDs and decoded text to the requester.
- **FR-007**: Correctness SHALL compare the entire distributed generated-token
  sequence with the same-model reference sequence and SHALL classify any count
  or token-position mismatch as failure.
- **FR-008**: The frozen workload SHALL contain exactly five real, self-contained
  prompts; each prompt SHALL have one excluded warmup and five measured
  generations, for 25 measured samples.
- **FR-009**: Every generation SHALL have a stable campaign ID, prompt ID,
  repetition ID, logical generation ID, and per-token request ID.
- **FR-010**: Raw evidence SHALL record prompt/input token counts, output token
  IDs and count, decoded text, stop reason, time to first token, each token-step
  duration, inter-token latency, total duration, generated tokens per second,
  terminal status, and error classification.
- **FR-011**: Summary evidence SHALL report per-prompt raw samples, completion
  rate, min/max, p50/p95, and a separately labeled descriptive pooled summary;
  it SHALL NOT report p99 as a stable estimate from 25 samples.
- **FR-012**: Warmup, failed, mismatched, cancelled, timed-out, and truncated
  generations SHALL remain in evidence but SHALL NOT enter successful latency
  percentiles.
- **FR-013**: Every token step SHALL use the normal secured NDNSF-DI request,
  collaboration assignment, dependency transfer, operation-status, and response
  path; no model- or campaign-specific Core bypass is allowed.
- **FR-014**: Evidence SHALL correlate every generation and token epoch with
  node, GPU UUID, role, layer range, backend, dependency names/digests/bytes,
  request timing, and terminal response.
- **FR-015**: A same-revision, same-dtype, same-chat-template standalone
  reference SHALL be frozen before distributed acceptance and SHALL record the
  exact expected token sequence and decoded text for every prompt.
- **FR-016**: Raw model download, standalone reference work, and stage-package
  construction SHALL use allocation-scoped TigerCluster scratch selected from
  `$SLURM_TMPDIR`, then `/scratch`, then `/tmp`, only after the allocation
  proves the selected path is writable and large enough.
- **FR-017**: Durable project storage SHALL retain only the promoted stage
  packages, tokenizer/chat-template artifact, immutable manifests, runtime, and
  evidence required to reproduce identities. The temporary full source-model
  copy and build workspace SHALL NOT be promoted when the same public revision
  can be re-fetched and the promoted stage packages have passed checksum and
  CUDA validation.
- **FR-018**: No model download, stage preparation, reference inference, or
  distributed inference SHALL occur until measured durable usage, projected
  durable promotion, measured allocation-scratch capacity, temporary scratch
  peak, explicit reserve, and allocation capacity produce a passing preflight
  decision. One explicitly authorized, bounded, no-download Slurm capacity probe
  MAY run solely to establish allocation-scoped scratch facts.
- **FR-019**: No superseded image, model, artifact, or evidence directory SHALL
  be deleted implicitly; cleanup requires an explicit target inventory and
  authorization.
- **FR-020**: All preparation and inference compute SHALL run in bounded Slurm
  allocations, never on the login node.
- **FR-021**: Live submission identities SHALL be exactly-once; failures SHALL
  not be retried automatically or overwritten.
- **FR-022**: Tokens, private keys, passwords, session cookies, and other
  credentials SHALL not enter model artifacts, runtime images, prompts, logs,
  or durable evidence.
- **FR-023**: Campaign reporting SHALL describe layer-pipeline collaboration and
  SHALL not claim tensor parallelism, production readiness, semantic answer
  quality, or general throughput scaling from this workload.

### Key Entities

- **Campaign Manifest**: Immutable campaign identity, model/tokenizer/runtime
  identities, prompt-set digest, generation policy, repetition policy,
  allocation contract, and evidence locations.
- **Prompt Case**: Stable prompt ID, user-visible text, formatted input digest,
  input token IDs/count, reference output, and completion constraint.
- **Generation Sample**: One warmup or measured attempt with stable identities,
  ordered token-step records, final text, status, and metrics.
- **Token Step**: One token epoch with request identity, input context digest,
  selected token, reference token, timing, stage/dependency correlation, and
  terminal state.
- **Capacity Decision**: Measured durable usage, projected source/stage/temp
  bytes, reserve, allocation facts, and allow/block reason.

## Success Criteria

### Measurable Outcomes

- **SC-001**: One acceptance generation produces a non-empty decoded answer,
  terminates by end-of-sequence within 64 new tokens, and matches the reference
  token-for-token.
- **SC-002**: All 25 measured generations are present as independently
  identifiable records; no warmup record is counted as measured.
- **SC-003**: All accepted measured generations execute three complete,
  non-overlapping layer ranges on three distinct RTX 5000 GPU UUIDs with zero
  CPU fallback.
- **SC-004**: The campaign report can reproduce completion rate, per-prompt
  p50/p95 end-to-end latency, time to first token, inter-token latency, and
  tokens per second directly from retained raw records.
- **SC-005**: Every accepted generated token is traceable through one secured
  request and two cross-node dependency transfers to one requester-visible
  response.
- **SC-006**: Preflight blocks execution unless the selected allocation scratch
  proves enough free space for the temporary full model, reference runtime,
  stage construction, and safety margin, while projected durable project usage
  retains at least 20 GiB reserve after only the promoted stages, tokenizer,
  manifests, runtime, and evidence.
- **SC-007**: Evidence verification reports zero missing checksum-bound
  campaign artifacts and zero retained credentials.

## Assumptions

- The current iTiger account limit of three concurrent nodes remains available
  at execution time and is rediscovered before submission.
- One H100 80GB can host the standalone reference; this is a planning
  assumption until a bounded allocation proves it.
- The prompt set requests concise answers so a correct model response can
  normally emit end-of-sequence within 64 new tokens.
- Greedy decoding is selected to isolate runtime variation from sampling
  randomness.
- Five repetitions per prompt provide a small descriptive distribution, not a
  statistically powered performance comparison or stable p99 estimate.
- Existing Spec 160 transport, coherent runtime, collaboration, and
  operation-status evidence may be reused only as prerequisite evidence; the
  32B multi-generation campaign receives new identities and acceptance evidence.
- Physical deployment, concurrent-load scaling, quantization, tensor
  parallelism, semantic answer scoring, and comparisons against other
  frameworks are out of scope.
