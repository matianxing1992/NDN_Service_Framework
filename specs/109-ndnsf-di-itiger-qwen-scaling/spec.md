# Feature Specification: NDNSF-DI iTiger Qwen Scaling

**Feature Branch**: `109-ndnsf-di-itiger-qwen-scaling`
**Created**: 2026-07-12
**Status**: Historical terminal-accounting closure — live inference scope superseded by Spec 110
**Input**: Design and execute a storage-safe NDNSF-DI experiment on iTiger across Qwen model sizes without storing bulk models locally.

> **Formal erratum (2026-07-13):** The `165/165` closure recorded by this
> feature means that every planned cell received a terminal accounting record;
> it does not mean that the requested iTiger GPU inference matrix executed.
> Zero GPU inference jobs were submitted. The live NDNSF-DI distributed-
> inference intent, corrected completion semantics, and new once-only identities are owned by
> [Spec 110](../110-itiger-qwen-live-inference/spec.md). Historical Spec 109
> evidence remains immutable; see [erratum.md](erratum.md).

## Scope and authority

Spec 109 establishes candidate-bound experimental evidence for Qwen2.5-Instruct 0.5B through 72B on iTiger. It separates full-model correctness/capacity, exported-artifact correctness, matched staged performance baseline, actual NDNSF-DI behavior, and NDNSF-DI overhead. It consumes Spec 107 generation-session behavior and the Spec 108 Slurm/Apptainer adapter rather than duplicating them. Physical-production, real-UAV/network, production-security, and long-soak authority remain with Spec 106.

## User Scenarios & Testing

### User Story 1 - Stage models without exhausting storage (Priority: P1)

An operator discovers actual quota and scratch capacity, stages a pinned Qwen model entirely on iTiger, and cleans temporary or unreferenced data without putting bulk artifacts in `/home` or on the workstation.

**Why this priority**: Model work is unsafe and irreproducible without capacity admission and durable storage.

**Independent Test**: Stage Qwen2.5-0.5B-Instruct from an immutable revision, verify its file manifest, prove no local model files were created, and dry-run cleanup without touching protected data.

**Acceptance Scenarios**:

1. **Given** quota and projected peak, **When** reserve would be violated, **Then** reject before download or allocation.
2. **Given** a completed transfer, **When** finalized, **Then** source, tokenizer, license, revision, sizes, and hashes are recorded.
3. **Given** job scratch, **When** the job exits, **Then** required outputs are promoted durably and scratch remains disposable.
4. **Given** cleanup, **When** active jobs, accepted evidence, referenced models, identities, and current/prior releases exist, **Then** none can be deleted.

---

### User Story 2 - Establish correctness oracles and matched performance baselines (Priority: P1)

An operator runs two deliberately different references for each eligible Qwen size: a full-model Transformers/PyTorch correctness oracle for deterministic token and capacity checks, and a staged ONNX Runtime baseline with the same exported stages, GPU mapping, workload, and runtime settings as the candidate but without NDNSF networking/security/orchestration.

**Why this priority**: Correctness needs an independent full-model oracle, while NDNSF-DI overhead needs a matched staged baseline. One reference cannot validly serve both purposes.

**Independent Test**: Run one 0.5B greedy full-model oracle and one staged ONNX baseline; retain exact tokens, stage numerical comparisons, per-node execution-provider assignment, GPU identity, workload fingerprint, peak memory, load time, TTFT, inter-token latency, and durable evidence.

**Acceptance Scenarios**:

1. **Given** a declared GPU, **When** inference runs, **Then** observed GPU/backend match and no undeclared CPU fallback occurs.
2. **Given** identical identity and deterministic decoding, **When** repeated, **Then** output token IDs match exactly.
3. **Given** OOM, scheduler, timeout, or load failure, **When** the cell ends, **Then** retain it without changing GPU, quantization, or context.
4. **Given** a candidate overhead calculation, **When** the reference is inspected, **Then** it uses the matched staged ONNX baseline and never the full-model Transformers timing.

---

### User Story 3 - Run the first real NDNSF-DI Qwen GPU candidate (Priority: P1)

An operator runs Qwen2.5-0.5B-Instruct through the real NDNSF-DI request, security, provider, dependency, GPU backend, generation-session, and response path.

**Why this priority**: This is the minimum proof that NDNSF-DI itself runs Qwen on iTiger.

**Independent Test**: Complete 1-, 2-, and 32-token exact-output cells plus three independent 60-second repetitions with complete candidate/backend/security evidence.

**Acceptance Scenarios**:

1. **Given** valid Spec 107 and 108 prerequisites, **When** providers start, **Then** candidate, stage, OCI, SIF, backend, GPU, and identity bindings are recorded.
2. **Given** full-model reference tokens and staged numerical checkpoints, **When** NDNSF-DI produces 1/2/32 tokens, **Then** token IDs and terminal semantics match exactly and the preregistered numerical tolerances pass.
3. **Given** unavailable CUDA, **When** fallback is disabled, **Then** readiness fails; explicit fallback produces `DEGRADED`, never GPU PASS.
4. **Given** normal execution, **When** requests complete, **Then** permissions, NAC-ABE, one-time tokens, replay protection, selection, and provider permission remain active.

---

### User Story 4 - Measure the 1.5B-14B ladder (Priority: P1)

An operator evaluates 1.5B, 3B, 7B, and 14B with model-local gates and campaign-wide systemic gates, then compares each candidate only with its matched staged ONNX baseline. A license or fit failure for one size does not censor otherwise admissible later sizes; a systemic failure such as invalid source, broken deployment release, insufficient shared storage, or failed security prerequisite blocks all dependent cells.

**Why this priority**: These sizes cover the practical single-node iTiger range and expose scale-dependent compute and transfer costs.

**Independent Test**: Execute each preregistered tier or emit a terminal gate record, retaining three unreplaced repetitions for every accepted size.

**Acceptance Scenarios**:

1. **Given** an advancing tier, **When** staged, **Then** it receives a new immutable model/export/candidate identity.
2. **Given** staged baseline and NDNSF-DI results, **When** overhead is calculated, **Then** artifacts, runtime, execution-provider assignment, stage/GPU mapping, workload, cache state, warmup, and window match.
3. **Given** a model-local gate failure, **When** another size is independently admissible, **Then** that other size may proceed and the failed size remains visible.
4. **Given** a campaign-wide systemic gate failure, **When** dependent cells are considered, **Then** they remain unstarted with the same blocking-gate digest.

---

### User Story 5 - Gate 32B/72B and multi-GPU extensions (Priority: P2)

An operator mechanically decides whether 32B/72B are admissible using actual quota, export amplification, GPU memory, allocation, and network evidence, then tries one-node/multi-GPU before multiple nodes.

**Why this priority**: Large models consume scarce storage and GPUs and must not be attempted by guesswork.

**Independent Test**: Produce admission decisions; admitted sizes retain standalone and NDNSF-DI outcomes, while rejected sizes prove no download or GPU job began.

**Acceptance Scenarios**:

1. **Given** insufficient quota, **When** a large-model cell is requested, **Then** reject before download and state the required capacity action.
2. **Given** an admitted model, **When** placement is chosen, **Then** try one-node/multi-GPU first.
3. **Given** no passing NFD network probe, **When** multi-node is requested, **Then** remain `DEFERRED`/`BLOCKED`.
4. **Given** quantization, tensor parallelism, or a different GPU count, **When** proposed, **Then** create a distinct candidate/cell.

---

### User Story 6 - Produce reproducible scaling evidence (Priority: P2)

An investigator reconstructs every attempted cell, compares only matched cells, preserves failures, and derives a scale report without upgrading substrate or standalone evidence into candidate or physical PASS.

**Why this priority**: Scaling results need auditable lineage, denominators, failures, and claim boundaries.

**Independent Test**: Mutate candidate/job/artifact/backend/token/timing/promotion records and verify aggregation blocks, then reproduce one accepted small-model cell.

**Acceptance Scenarios**:

1. **Given** a campaign, **When** aggregated, **Then** every cell is `PASS`, `FAIL`, `BLOCKED`, `DEFERRED`, or `NOT_STARTED` with reason/evidence.
2. **Given** unmatched configuration, **When** compared, **Then** label it descriptive and exclude matched-overhead claims.
3. **Given** any bundle, **When** authority is inspected, **Then** physical production remains deferred to Spec 106.

### Edge Cases

- Email quota differs from live quota or shared capacity.
- Compute nodes lack model-registry egress or a partial download exists.
- Model/tokenizer revision moves or LFS pointers are incomplete.
- Source and export coexist beyond projected peak.
- Apptainer versions differ or a cached SIF came from another release.
- Physical GPU mapping differs from container-local device zero.
- Only some model stages select CUDA.
- CUDA Execution Provider is registered but one or more graph nodes are assigned to CPU.
- Export changes tensor/KV dtype, shape, or tokens.
- A 60-second large-model window completes zero requests.
- A 60-second run has too few observations to support p95 or p99.
- A model-local license or fit failure occurs while a later model is otherwise admissible.
- The Git worktree is dirty or contains untracked experiment source.
- A predecessor task is named by range but one required task or digest is absent.
- Jobs queue, preempt, timeout, cancel, OOM, or fail during evidence promotion.
- Multi-node NFD transport is unavailable.
- License terms differ across sizes.

## Requirements

### Functional Requirements

- **FR-001**: Keep bulk models/exports off the workstation and out of `/home`.
- **FR-002**: Use `/project/$USER/ndnsf-di` for durable artifacts and allocation `/tmp` only for disposable work.
- **FR-003**: Admit storage using target-path quota/free-space, reserve, and projected peak, never shared capacity alone.
- **FR-004**: Protect active jobs, identities, accepted evidence, referenced models, and current/prior releases.
- **FR-005**: Cleanup defaults to dry-run and rejects protected/referenced paths.
- **FR-006**: Bind every model to repository, immutable revision, tokenizer, license, sizes, and hashes.
- **FR-007**: Transfer is bounded and finalized only by a complete manifest.
- **FR-008**: The first matrix uses Qwen2.5-Instruct 0.5B/1.5B/3B/7B/14B/32B/72B; other families or quantization are separate matrices.
- **FR-009**: Bind every cell to model, tokenizer, source, candidate, OCI, SIF, dtype, export, stages, prompts, resources, and repetition.
- **FR-010**: Evaluate sizes in ascending scheduling order, but distinguish model-local gates from campaign-wide systemic gates; only systemic failures stop all dependent sizes.
- **FR-011**: Standalone references run only in Slurm allocations and record requested/allocated resources and actual GPU UUID/model.
- **FR-012**: Standalone references use deterministic greedy decoding and retain exact input/output token IDs.
- **FR-013**: Development smokes are diagnostic and never acceptance eligible.
- **FR-014**: Candidate cells use the real dynamic runtime and normal permission, NAC-ABE, token, replay, selection, and provider-permission checks.
- **FR-015**: Consume, not duplicate, Spec 107 generation-session and Spec 108 deployment authority.
- **FR-016**: Fail closed on missing, failed, mixed, stale, or tampered prerequisite records.
- **FR-017**: GPU PASS requires every executed model stage and every profiled ONNX graph node to be assigned to CUDA, with allocation-correlated GPU identity; provider registration alone is insufficient.
- **FR-018**: Explicit CPU fallback is `DEGRADED`, never GPU PASS.
- **FR-019**: Validate export graph/external data, stage tensors, shapes, dtypes, KV mappings, and hashes.
- **FR-020**: Prove 1/2/32-token equality before performance.
- **FR-021**: Exclude warmup and use a 60-second measured window.
- **FR-022**: Retain three independent repetitions per accepted size; pooling cannot rescue failure.
- **FR-023**: Measure completion/failure, TTFT, inter-token latency, tokens/s, throughput, p50/p95/p99, resources, and stages.
- **FR-024**: Measure queue/dependency wait and NDN bytes/segments for overhead attribution.
- **FR-025**: Matched performance comparisons require identical exported artifacts, ONNX Runtime version/options, dtype, prompt/load profile, context/output, cache state, stage topology, GPU mapping, warmup, logging, timeout, and measured window; only NDNSF networking, security, and orchestration may differ.
- **FR-026**: Retain OOM, timeout, preemption, scheduler, conversion, correctness, security, network, and promotion failures.
- **FR-027**: Never auto-retry, replace, shorten, quantize, move, or delete a measured cell.
- **FR-028**: Attempt one-node/multi-GPU before multi-node placement.
- **FR-029**: Disable multi-node placement until allocation-scoped NFD transport passes.
- **FR-030**: Do not transfer/export 32B/72B before dedicated admission PASS.
- **FR-031**: Quantized/parallel/GPU-count/node-count variants have distinct identities and baselines.
- **FR-032**: Every job has unique ID, bounded resources, exact-job lifecycle, and termination finalization.
- **FR-033**: Promote evidence durably without hiding original workload exit.
- **FR-034**: Exclude credentials, MFA, private keys, registry/security tokens, and unrestricted prompt/tensor content from evidence.
- **FR-035**: Retain scheduler, container, model, candidate, backend, GPU, network, storage, timing, terminal, and promotion lineage.
- **FR-036**: Reject missing/tampered/mixed/duplicate/partial/fallback/false-authority evidence.
- **FR-037**: Give every preregistered cell one explicit terminal state and reason.
- **FR-038**: Separate substrate, standalone, artifact, candidate correctness/performance, and physical authority.
- **FR-039**: Keep negative and capacity-bound results visible.
- **FR-040**: Retain exact reproduction commands/environment for accepted cells.
- **FR-041**: Record license/attribution before execution.
- **FR-042**: Keep physical production `DEFERRED`, owned by Spec 106.
- **FR-043**: Use the full-model Transformers/PyTorch path only as correctness and capacity oracle; never use its timing as the NDNSF-DI overhead denominator.
- **FR-044**: Compute candidate overhead only against a matched staged ONNX Runtime baseline using the candidate's exact stage artifacts and GPU placement without NDNSF networking/security/orchestration.
- **FR-045**: Bind every performance cell to arrival mode, offered rate, maximum in-flight requests, timeout, prompt population/order, cache state, warmup count, measured window, and run-order policy.
- **FR-046**: Record completed-sample counts and confidence intervals; mark p50/p95/p99 `UNAVAILABLE_INSUFFICIENT_N` below preregistered thresholds instead of fabricating a value.
- **FR-047**: Bind correctness to exact input/output token arrays plus preregistered per-stage hidden-state, KV-cache, and final-logit tolerances and top-1 margin checks.
- **FR-048**: Bind every campaign to a reproducible source snapshot: clean commit, or commit plus sealed binary diff, untracked-file manifest/archive, and content digests.
- **FR-049**: Name exact predecessor task IDs, accepted statuses, artifact paths, schema versions, and digests; vague spec-level or task-range references must fail closed.
- **FR-050**: Consume the Spec 108 deployment profile and release by digest; Spec 109 may add experiment bindings but must not maintain a duplicate Slurm resource source of truth.
- **FR-051**: Enforce cross-field, uniqueness, terminal-state, authority, source, predecessor, and comparison invariants through both JSON Schema and one canonical semantic validator.
- **FR-052**: Keep repository-local scripts/contracts as canonical automation; personal wrappers cannot be execution dependencies.
- **FR-053**: Represent every generated experiment cell in an immutable keyed ledger; a bundled Slurm job may contain multiple cells, but each cell closes independently and the task remains open while any member lacks a terminal record.
- **FR-054**: Separate pre-implementation design audit from post-implementation code/evidence audit; neither document may satisfy the other's gate.

### Key Entities

- **Model Registry Entry**: Pinned model/tokenizer/license/file manifest and storage state.
- **Storage Admission Record**: Quota, capacity, projected peak, reserve, protected paths, and decision.
- **Experiment Candidate**: Source/model/export/runtime/container/stage/prompt/resource identity.
- **Experiment Cell**: One preregistered configuration, repetition, and terminal state.
- **Full-Model Oracle**: Deterministic Transformers/PyTorch complete-model output and capacity oracle; never the overhead denominator.
- **Stage Artifact Set**: Export pieces, external data, tensor/KV contract, partition, and hashes.
- **Backend Observation**: Requested/selected provider, fallback, runtime, device mapping, and stage coverage.
- **Source Snapshot**: Commit, dirty-state proof, sealed diff/untracked archive, and content digests.
- **Predecessor Gate**: Exact Spec 107/108 task/status/schema/artifact digest manifest.
- **Workload Profile**: Arrival, load, concurrency, timeout, prompts, cache, warmup, window, and run-order identity.
- **Matched Baseline Pair**: Candidate/baseline fingerprints whose only permitted difference is the NDNSF layer.
- **Slurm Run Record**: Request/allocation, lifecycle, node/GPU, exit, and evidence location.
- **Scaling Evidence Bundle**: Candidate-bound correctness, performance, network, storage, and authority evidence.
- **Scale Matrix**: Planned cells and explicit terminal outcomes.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Zero bulk model/SIF/ONNX/cache bytes are intentionally written to the workstation or `/home`.
- **SC-002**: Every attempted model has a complete registry entry; every rejected model has a pre-download gate record.
- **SC-003**: The 0.5B standalone GPU reference completes with exact tokens and durable evidence.
- **SC-004**: The 0.5B candidate matches 1/2/32 reference tokens with no undeclared fallback.
- **SC-005**: Every accepted size has three original 60-second repetitions with independent metrics.
- **SC-006**: Every GPU stage reports CUDA and allocation-correlated identity; any CPU stage blocks GPU PASS.
- **SC-007**: Every 0.5B-72B preregistered cell has one explicit terminal status/reason.
- **SC-008**: 1.5B-14B obey explicit dependency gates; systemic blocks stop dependants, while model-local failures do not start that model's derivatives and do not censor unrelated admissible sizes.
- **SC-009**: 32B/72B start only when capacity exceeds projected peak plus reserve.
- **SC-010**: Matched overhead is reported without mixing model, dtype, workload, or hardware.
- **SC-011**: Stage compute and dependency/network wait explain at least 99% of completed token-step critical paths or attribution is incomplete.
- **SC-012**: Secret scans find zero credential/private-key/MFA/raw-token disclosures.
- **SC-013**: Tamper, missing, mixed, fallback, duplicate, and false-authority tests fail closed.
- **SC-014**: One small-model cell reproduces exact tokens and numerical checkpoints, and its 90% confidence intervals for relative median TTFT and tokens/s lie within the preregistered ±10% engineering margin; insufficient samples are `INCONCLUSIVE` and larger deviations are failures.
- **SC-015**: Zero multi-node model cells start before network-gate PASS.
- **SC-016**: Every final artifact keeps physical production deferred to Spec 106.
- **SC-017**: Every reported NDNSF-DI overhead value is `candidate - matched staged ONNX baseline`; zero values use full-model Transformers timing as denominator.
- **SC-018**: Every GPU PASS includes complete per-node execution-provider assignment proving zero CPU-assigned model nodes.
- **SC-019**: Every reported percentile includes its sample count; p50 requires at least 20 observations, p95 at least 100, and p99 at least 1000, otherwise it is explicitly unavailable.
- **SC-020**: Every campaign and accepted cell resolves to a source snapshot and exact predecessor manifest whose digests reproduce byte-for-byte.
- **SC-021**: Contract tests reject duplicate cell/run identities, contradictory terminal/authority records, incomplete source/predecessor bindings, unmatched baseline pairs, and false GPU PASS.
- **SC-022**: A model-local failure does not prevent terminal evaluation of another independently admissible size; systemic blocking is always tied to one explicit gate digest.

## Assumptions

- Qwen2.5-Instruct is the controlled first family because current work uses 0.5B.
- Cluster GRES, versions, quota, and egress are rediscovered before execution.
- Project capacity is approximately 200 GB until live quota evidence replaces the email statement.
- Public transfer avoids credentials; required tokens use approved secret injection and are never recorded.
- Quantization is excluded from the first matrix.
- The primary scaling analysis is descriptive unless sizes share an identical hardware/resource block; causal model-size claims are limited to that controlled subset.
- Percentile availability thresholds are validity rules, not performance targets.
- Quota expansion is acceptable for large models; abusing `/home` or scratch is not.

## Dependencies

- Spec 107: exact prerequisite set `T027` and `T028-T038` for generation-session selection, implementation, security, artifact, and exact-token behavior.
- Spec 108: exact prerequisite set `T091-T102` for OCI GPU release, truthful backend, Slurm/Apptainer, storage, and evidence contracts; the first qualifying Spec 109 job may also close Spec 108 `T103` without duplicate submission.
- Spec 106: physical-production and real deployment authority.
- Pinned Qwen repositories/licenses and mutable iTiger services are external dependencies.

## Out of Scope

- Training/fine-tuning/dataset creation.
- Mixing Qwen families, specializations, or quantization in the first matrix.
- Login-node conversion/inference or local bulk model storage.
- Persistent public service deployment.
- Security bypasses for performance.
- Physical-production claims.
