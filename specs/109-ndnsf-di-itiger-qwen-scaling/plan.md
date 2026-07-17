# Implementation Plan: NDNSF-DI iTiger Qwen Scaling

**Branch**: `109-ndnsf-di-itiger-qwen-scaling` | **Date**: 2026-07-12 | **Spec**: [spec.md](spec.md)
**Input**: `specs/109-ndnsf-di-itiger-qwen-scaling/spec.md`

## Material Passport

- Origin Skill: academic-research-suite/experiment-agent
- Origin Mode: plan
- Origin Date: 2026-07-12
- Verification Status: UNVERIFIED (audit-remediated design; no Spec 109 model transfer or job executed)
- Version Label: code_plan_v2_audit_remediation

## Summary

Create a gated iTiger Qwen scaling campaign with three non-interchangeable planes: a full-model Transformers/PyTorch correctness and capacity oracle, a matched staged ONNX Runtime performance baseline, and the real NDNSF-DI candidate. Keep bulk artifacts in project storage, run mutable work only in bounded Slurm allocations, reuse Spec 107 runtime semantics and Spec 108 deployment profiles by digest, and preserve every measured failure. Model-local failures remain local; campaign-wide systemic failures block only their true dependants. Physical-production authority remains deferred to Spec 106.

## Technical Context

**Languages/Tools**: Python 3, Bash, C++17, JSON/YAML, Slurm CLI, Apptainer 1.3.x, OCI/SIF, ONNX/ONNX Runtime CUDA, Transformers/PyTorch oracle path
**Primary Dependencies**: current NDNSF-DI dynamic runtime; Spec 107 Qwen generation session; Spec 108 deployment package/profile; NFD/ndn-cxx; pinned Qwen2.5 repositories
**Storage**: `/project/$USER/ndnsf-di` durable registry/images/models/manifests/evidence; allocation `/tmp/$USER/ndnsf-di/$SLURM_JOB_ID` disposable scratch; `/home` small account configuration only
**Testing**: schema and semantic-contract mutation tests, source/predecessor validation, export tensor/KV equivalence, full-model token oracle, matched staged baseline, MiniNDN security regression, bounded live Slurm cells
**Target Platform**: iTiger `bigTiger`, initially one node; RTX 5000/6000/H100 profiles rediscovered by preflight
**Performance Goals**: three original 60-second repetitions; matched baseline/candidate fingerprints; completion, confidence intervals, validity-qualified percentiles, TTFT, inter-token, throughput, resources, stage compute, queue/dependency/network wait
**Constraints**: no local bulk storage; no login-node compute; no auto-rerun; no unverified multi-node; fail-closed GPU; exact-token and numerical correctness before performance; 200 GB provisional project budget
**Scale/Scope**: Qwen2.5-Instruct 0.5B/1.5B/3B/7B/14B/32B/72B, conditional large-model and multi-node cells

## Constitution Check

| Principle | Design response | Verdict |
|---|---|---|
| Canonical dynamic runtime | Uses unified service names and the existing dynamic request/provider path | PASS |
| Security in data path | Candidate retains permission, NAC-ABE, one-time token, replay, selection, and provider-permission checks | PASS |
| CodeGraph first | Current Qwen benchmark and ONNX runner/provider evidence paths were inspected before revision | PASS |
| Spec-driven change | Source, predecessor, workload, comparison, matrix, and evidence contracts live in Spec 109 | PASS |
| Right verification scope | MiniNDN remains the functional/security gate; iTiger validates bounded container/GPU scale | PASS |
| GSD/ARS | GSD health is checked; ARS experiment rules govern variables, confounds, repetition, and claims | PASS |

No new wire name, security bypass, scheduler replacement, credential store, or physical-production authority is introduced.

## Research questions and claim boundary

**RQ1a — descriptive envelope**: What completion, latency, throughput, resource, and dependency-overhead outcomes are observed for each size under its preregistered admissible placement?

**RQ1b — controlled size effect**: Within the subset that uses an identical GPU class/count, stage topology, runtime, workload, and cache policy, how do outcomes change with parameter count? Results outside this common-resource block are descriptive, not causal size-effect evidence.

**RQ2**: For which model sizes and placements does the current NDNSF-DI generation-session and staged-artifact design remain correct and operationally admissible?

**H1**: Placement and fit expectations are planning hypotheses only; live fit/OOM results determine admissibility and cannot be interpreted as a pure model-size effect when hardware differs.

**H2**: Absolute NDNSF-DI overhead may grow with tensor/KV transfer and queue/dependency wait, while its fraction of total latency may decline with model compute. This is falsifiable, not an improvement claim.

## Variables, controls, and validity

| Type | Variables |
|---|---|
| Independent | model size; execution plane; GPU class/count; stage count/placement; separately identified parallel/quantized variants |
| Primary dependent | exact-token/numerical completion; TTFT; inter-token latency; output tokens/s; request throughput; p50/p95/p99 when valid |
| Secondary dependent | load time; CPU RSS; GPU memory/utilization; stage compute; queue/dependency wait; NDN bytes/segments; scratch/durable bytes |
| Controlled within a matched pair | exported bytes; ONNX Runtime/CUDA versions/session options; GPU allocation/stage mapping; prompt population/order; arrival/offered load/max-in-flight/timeout; decode; context/output; cache state; warmup; logging; 60-second window |
| Retained confounders | scheduler queue/preemption; node/GPU/driver; filesystem/cache state; export layout; network placement |

### Preregistered workload and statistics

- Every performance cell locks arrival mode, offered rate, maximum in-flight requests, timeout, prompt population/order, cache state, warmup count, logging, and run-order policy before submission.
- Baseline/candidate pairs use the same workload fingerprint and randomized or interleaved order. A mismatch makes the comparison descriptive only.
- Each accepted mode has three original 60-second repetitions. Report each repetition, then an across-repetition estimate and 95% bootstrap confidence interval; request pooling cannot rescue a failed repetition.
- p50 needs at least 20 completed observations, p95 at least 100, and p99 at least 1000. Otherwise record `UNAVAILABLE_INSUFFICIENT_N` and the count.
- The ±10% reproduction margin is a preregistered engineering-equivalence bound, not statistical significance. PASS requires exact tokens and a 90% confidence interval for relative median TTFT and tokens/s inside the bound; insufficient evidence is `INCONCLUSIVE`.

## Architecture

### 1. Source and predecessor authority

`source-snapshot.json` binds a clean commit or, for a dirty worktree, the commit plus sealed binary diff, untracked-file manifest/archive, and SHA-256 digests. `predecessor-gate.json` names Spec 107 `T027,T028-T038` and Spec 108 `T091-T102` individually with required status, schema, artifact path, and digest. Missing, stale, mixed, or failed entries block candidate execution. The first qualifying Spec 109 candidate may also close Spec 108 `T103`; no duplicate job is submitted for bookkeeping.

### 2. Storage plane

`ModelRegistryEntry` owns immutable source/tokenizer/license/file identity. `StorageAdmissionRecord` combines actual quota/capacity, projected transfer/export/cache/evidence peak, reserve, and protected paths. Project storage is durable; allocation scratch is disposable. Content-addressed manifests prevent directory existence from becoming completion evidence.

### 3. Candidate and workload identity

`ExperimentCandidate` binds source snapshot, predecessor manifest, model/tokenizer, OCI/SIF, dtype, exporter/opset, stage partition, prompt/workload profile, and the Spec 108 deployment-profile digest. Spec 109 does not duplicate account/QOS/CPU/memory/walltime/GRES truth. Any changed binding creates a new candidate/cell.

### 4. Full-model correctness oracle

The pinned SIF and model revision run deterministic Transformers/PyTorch inference inside Slurm. Exact token arrays, capacity, GPU, and resource evidence form the correctness/capacity oracle. Its timing is never an NDNSF-DI overhead denominator.

### 5. Export and numerical equivalence

Exports occur in scratch and promote only complete content-addressed artifacts. Validation covers graph/external data, shapes, dtypes, KV mappings, exact final tokens, and preregistered `rtol`/`atol` for stage hidden states, KV tensors, and final logits plus top-1 margin.

### 6. Matched staged ONNX baseline

For every performance candidate, run the exact exported stages with identical ONNX Runtime options, topology, GPU mapping, workload, cache, warmup, timeout, and logging, but without NDN transport, NDNSF security, selection, or orchestration. Candidate overhead is `candidate - matched baseline` only.

### 7. NDNSF-DI candidate and truthful GPU evidence

Run NFD, controller, user, and providers in Apptainer on the allocated node and preserve the normal security and generation-session path. Enable ONNX Runtime profiling for every role and retain node-to-execution-provider assignments. Registering CUDA is not proof: every executed model node must be assigned to CUDA and correlated to the Slurm GPU UUID. CPU-assigned model nodes are `DEGRADED`/`FAIL` under the locked fallback policy.

### 8. Evidence, ledger, and authority

A keyed immutable ledger guarantees unique cell/run identities. A bundled Slurm job may close several correctness cells, but each cell has an independent terminal record and the enclosing task stays open until all members are terminal. JSON Schema enforces structural conditions; one canonical repository-local semantic validator enforces uniqueness, digest resolution, comparison fingerprints, source/predecessor authority, and cross-record invariants.

### 9. Gate taxonomy

- **Campaign-wide/systemic**: invalid source snapshot, failed Spec 107/108 prerequisite, unusable deployment release, unavailable security path, unsafe shared storage, or evidence-validator failure. Blocks dependent cells across sizes.
- **Model-local**: license, download, export, fit/OOM, numerical correctness, backend assignment, or promotion for one model identity. Blocks only that model and its derivatives.
- **Placement-local**: a GPU/node mapping or network probe. Another placement needs a new candidate and does not replace the original outcome.
- Every BLOCK carries `gateScope`, `gateId`, and `gateDigest`; free-text ladder stops have no authority.

## Storage and placement ladder

| Size | Initial oracle | First candidate placement | Admission note |
|---|---|---|---|
| 0.5B | 1 RTX 5000 | one node, staged GPUs as required | MVP |
| 1.5B | 1 RTX 5000 | one node | independent local gate |
| 3B | 1 RTX 5000 | one node | license gate local to 3B |
| 7B | 1 RTX 5000 | one node, multi-GPU stages | transfer-sensitive |
| 14B | 1 RTX 6000/H100 | one node, multi-GPU | avoid marginal fit |
| 32B | 1 H100 oracle | one node, 3-4 GPUs | dedicated capacity gate |
| 72B | at least 2 H100 | one node, 3-4 H100 stages | quota expansion expected |

These are hypotheses. Live discovery and measured failures override them without rewriting the matrix.

## Experiment sequence

1. Implement offline contracts, semantic validator, source/predecessor sealing, fixtures, mutation tests, storage estimator, and keyed matrix compiler.
2. Run read-only iTiger discovery for identity, quota, partition/GRES, versions, and egress.
3. Bind the Spec 108 deployment profile/release by digest and create protected project storage.
4. Stage and seal 0.5B, then run full-model oracle, export numerical equivalence, and matched staged ONNX baseline.
5. Satisfy exact Spec 107/108 predecessor gates.
6. Run 0.5B NDNSF-DI 1/2/32 correctness, then three matched performance pairs.
7. Evaluate 1.5B, 3B, 7B, and 14B with model-local gates; continue other admissible sizes after a local failure.
8. Admit 32B/72B before transfer; one-node/multi-GPU precedes multi-node.
9. Enable multi-node only after an allocation-scoped NFD network PASS.
10. Aggregate with valid statistics, reproduce one small cell, run post-implementation audit, and hand physical-only work to Spec 106.

## Failure, retry, and cleanup

- Preflight failures produce scoped terminal gate records before role start.
- Diagnostics use separate identities and never become acceptance evidence.
- An acceptance cell is submitted exactly once and never auto-retried.
- Scheduler/OOM/timeout/preemption/cancel/network/export/correctness/security/promotion failures remain original outcomes.
- Cleanup fails closed on owner ambiguity, missing manifests, referenced paths, incomplete promotion, or an unsealed source snapshot.
- Model-local failure does not silently censor other sizes; systemic failure propagates only through explicit dependency edges.

## Project Structure

```text
specs/109-ndnsf-di-itiger-qwen-scaling/{spec.md,plan.md,research.md,data-model.md,quickstart.md,traceability.md,tasks.md,contracts/,checklists/}
packaging/ndnsf-di-container/{adapters/slurm-apptainer/,oci/}
tools/ndnsf-di/{spec109_source.py,spec109_predecessors.py,spec109_model_registry.py,spec109_storage.py,spec109_candidate.py,spec109_matrix.py,spec109_evidence.py,run_spec109_*.py}
tests/container/itiger-qwen/
tests/python/test_ndnsf_di_spec109_*.py
results/spec109-itiger-qwen/  # ignored execution evidence/pointers
```

Spec 108 remains the sole owner of deployment resources. Spec 109 adds experiment bindings only. Repository-local scripts are canonical; personal wrappers are non-authoritative.

## Validation strategy

- **Offline**: schemas plus semantic validator, source/predecessor manifests, path/storage, keyed matrix, exact-once, matched comparisons, mutation, and aggregation.
- **Local**: exporter/tensor/KV numerical checks and ONNX provider-profile fail-closed tests without claiming GPU execution.
- **MiniNDN**: security and generation-session functional regressions before live candidate performance.
- **iTiger**: read-only discovery, bounded transfer, oracle/baseline/candidate cells, conditional large-model/network cells.
- **Reproduction**: exact tokens, numerical checkpoints, and engineering-equivalence confidence intervals; insufficient data is inconclusive.

## Complexity Tracking

| Mechanism | Why needed | Simpler alternative rejected because |
|---|---|---|
| Three oracle/baseline/candidate planes | Separates correctness from overhead | Transformers and staged candidate topology are not timing-comparable |
| Per-size immutable candidates | Prevents cross-size evidence mixing | One mutable candidate breaks artifact/performance lineage |
| Node-level provider profiling | Prevents false GPU PASS after ORT CPU partitioning | CUDA registration proves availability, not node execution |
| Source and exact predecessor manifests | Makes dirty/untracked source and dependencies reconstructable | Commit-only and task-range labels are incomplete authority |
| Keyed terminal matrix | Preserves uniqueness, denominator, partial bundles, and negative results | Successful-only arrays hide duplicates and partial completion |
| Scoped gates | Avoids needless campaign censoring | A global ladder stop confuses local fit/license failures with systemic invalidity |
