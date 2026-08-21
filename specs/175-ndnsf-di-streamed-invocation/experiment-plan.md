# Experiment Plan: NDNSF-DI Streamed Generation Qualification

## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: plan
- Origin Date: 2026-08-21
- Verification Status: UNVERIFIED
- Version Label: spec175_code_plan_v1

## Experiment Overview

- **Title**: Request-scoped multi-Provider ONNX token streaming through NDNSF-DI
- **Objective**: Verify that one authorized placement can prefill once, produce
  an ordered local-LLM-like token stream, and close with one exact final result;
  then determine whether the frozen three-Provider Qwen3.6-27B deployment reaches
  20 token/s on Tiger.
- **Primary hypothesis**: The implementation produces an exact ordered token
  stream and complete result with one Request/plan/prefill per generation.
- **Performance hypothesis**: The warm three-Provider Qwen3.6-27B pipeline has
  median steady-state output >=20.0 token/s and p95 inter-token interval <=75 ms.
- **Type**: deterministic protocol/inference validation plus environment-sensitive
  GPU performance qualification.

## Research Questions

1. Does the request-scoped stream preserve exact token order, at-most-once
   application delivery, attempt/plan fencing, and complete terminal closure?
2. Does the distributed generation loop perform prefill once and reuse exact
   provider-local KV instead of repeating full-context inference or placement?
3. Do deterministic loss/reorder/duplicate/cancellation cases recover or fail
   exactly as registered without partial success?
4. On the frozen Tiger candidate, what components determine TTFT and TPOT, and
   does steady-state output meet the 20 token/s qualification threshold?

## Variables

### Independent variables

- gate/environment: in-process CPU, real MiniNDN CPU, Tiger CUDA;
- role plan: 1, 2, or 4 tiny-model roles; 1 Qwen3-0.6B role; 3 Qwen3.6-27B roles;
- fault case: none, first loss, reordering, duplication, cancellation, stale
  attempt, callback failure, bounded backpressure, opt-in replacement;
- prompt ID: two fixed prompts;
- process repetition: three fresh processes.

### Dependent variables

- exact token/final-result/transcript verdict;
- success/failure and terminal reason;
- Request, ACK closure, plan, Selection, prefill, decode, End, and Response counts;
- TTFT, inter-token intervals, TPOT, steady-state tokens/s, total latency;
- ORT/activation/sampling/feedback/event/queue component timings;
- retransmission/gap/duplicate/reorder counts;
- KV hit/miss/recompute and provider replacement counts;
- CPU/GPU/memory/network utilization and CPU fallback count.

### Controlled variables

- code tree, exact SIF, ONNX, tokenizer, adapter, workload, and contract hashes;
- model revisions and role splits;
- Greedy sampling, maximum tokens, prompt set, request deadline, stream options;
- NDN-SVS version/configuration, Boost, ORT, CUDA, Apptainer, NFD/MiniNDN;
- Tiger node/GPU allocation recorded per process;
- no admission control, adaptive event policy, runtime download, or implicit
  fallback.

### Potential confounds and controls

| Confound | Control |
|---|---|
| Cold model/artifact/session preparation | record and exclude one cold invocation per performance process |
| Different GPU/node characteristics | record GPU UUID/model/clock/driver; retain process-level values |
| NDN cache and retained event state | fresh request/generation/stream epochs; fresh process groups; report cache counters |
| Prompt/token length | fixed prompt IDs and max-token contract; report actual EOS length |
| Model/tokenizer drift | immutable revisions and content digests |
| Host/SIF ABI drift | SIF-native preflight and remote hash verification |
| Hidden CPU fallback | requested/actual ORT providers plus per-role fallback count must be zero |
| Logging overhead | same bounded evidence profile in all measured runs |
| Retry/fault interaction | one registered fault dimension per case; no mixed faults in primary matrix |
| Stochastic token divergence | Greedy primary workload; seeded fixed-logit unit test only for stochastic sampler |

## Setup

- **Working directory**: `/home/tianxing/NDN/ndn-service-framework`
- **Local entry commands**: exactly those in
  [validation-contract.md](contracts/validation-contract.md)
- **Tiger entry command**:
  `packaging/ndnsf-di-container/jobs/spec175/submit.sh`
- **Local environment**: current system toolchain for C++ unit/integration;
  exact locally built SIF for MiniNDN workload processes.
- **Tiger environment**: exact promoted SIF under Slurm/Apptainer with one GPU per
  selected Qwen3.6 role.
- **Dependencies**: frozen by candidate manifest; no network model download.

## Inputs

| Input | Path/identity | Description |
|---|---|---|
| Contract | `specs/175-ndnsf-di-streamed-invocation/contracts/` | API, wire, generation, and gate authority |
| Tiny model | `tests/fixtures/spec175/tiny-causal-lm-v1/manifest.json` | deterministic CPU ONNX oracle |
| Small Qwen | `Qwen/Qwen3-0.6B@e6de91484c29aa9480d55605af694f39b081c455` | Tiger single-GPU functional subject |
| Large Qwen | `Qwen/Qwen3.6-27B@6a9e13bd6fc8f0983b9b99948120bc37f49c13e9` | Tiger three-role subject |
| Workload | `packaging/ndnsf-di-container/jobs/spec175/workload.json` | fixed prompts, seed, tokens, deadlines, stream options |
| Candidate | `$SPEC175_CANDIDATE_MANIFEST` | immutable SIF/source/runtime/artifact identity |

## Expected Outputs

| Output | Path | Format | Success criterion |
|---|---|---|---|
| Gate manifests | `results/spec175/g*/qualification-manifest-v1.json` | JSON | schema-valid, complete case inventory, honest verdict |
| Per-invocation events | `results/spec175/g*/events/*.jsonl` | bounded JSONL | exact cursor/lineage/timing fields; no plaintext prompt/answer/KV/logits |
| Evidence index | `results/spec175/g*/evidence-index.json` | JSON | every retained file has SHA-256 |
| Tiger result | project evidence directory mirrored to manifest | JSON/JSONL | exact SIF/workload hash and Slurm/job/node identity |
| Summary | `results/spec175/g7/summary.json` | JSON | correctness plus TTFT/TPOT/tokens/s/resource distributions |

Canonical results are local evidence until reviewed and reduced into tracked
Spec evidence. Raw repeated debug runs are not committed.

## Monitoring Configuration

- **Local timeout**: 120 s per unit/integration case; 300 s per MiniNDN case;
  45 min for complete G4.
- **Tiger timeout**: 10 min preflight; 30 min single-GPU functional; 90 min
  three-Provider functional; 4 h performance campaign.
- **Idle timeout**: 30 s without a verified lifecycle/decode/event progress
  transition; does not extend the absolute timeout.
- **Process monitoring**: process tree, Slurm state, CUDA OOM, NFD liveness,
  per-role heartbeat, evidence file growth, disk free space.
- **Automatic action**: no automatic retry of a crashed experiment. The runner
  terminates only on its registered hard timeout/cancellation and preserves the
  partial manifest.
- **Disk policy**: use the existing project cleanup/retention policy; do not copy
  content-addressed models or SIFs per run.

## Repetition and Workload Schedule

### Deterministic correctness

- Unit tests: normal suite execution; property/mutation loops use fixed seed.
- Integration healthy cases: three fresh processes; fault cases one fresh
  process each.
- MiniNDN M01-M09: three fresh MiniNDN processes per case (27 total).
- Exact tokens, counts, digests, and terminal reason must match; timing is not
  compared for exact reproducibility.

### Tiger functional gates

- G5 and G6: for each of two fixed prompts, record one cold preparation and run
  three fresh warm processes; six measured invocations total per gate.
- Functional gating uses exact tokens/digests and zero fallback, not the 20
  token/s threshold.

### Tiger performance gate

- Three independent processes.
- Each process: one cold invocation recorded/excluded, followed by ten measured
  warm invocations alternating the two prompt IDs.
- Total measured warm units: 30.
- No failed or slow valid unit is excluded.

## Analysis Plan

### Correctness

The experimental unit is one invocation. Report all counts and exact oracle
verdicts. Any missing/duplicate logical token, digest mismatch, wrong terminal
reason, multiple Response, stale delivery, or CPU fallback fails that unit and
the mandatory gate.

### Performance

Primary metric: per-invocation steady-state tokens/s after first-token delivery
and before terminal drain. Secondary metrics: TTFT and inter-token intervals.

Across the 30 measured G7 invocations report:

- count and failures;
- min, median, mean, p95, p99, maximum;
- process-level values and prompt-stratified values;
- nonparametric bootstrap 95% confidence interval for median tokens/s using
  fixed analysis seed `1750003` and 10,000 resamples;
- p95 inter-token interval from all measured warm intervals, plus per-invocation
  p95 to expose long-answer weighting;
- component attribution by median and p95 duration and fraction of TPOT.

No null-hypothesis significance test is required: qualification is against
registered engineering thresholds. The bootstrap interval describes uncertainty
and does not replace the literal verdict rule.

### Verdict

- `FAIL`: any mandatory correctness, security, provenance, or zero-fallback
  condition fails.
- `FUNCTIONAL_PASS_PERFORMANCE_MISS`: all correctness conditions pass but either
  median tokens/s <20.0 or p95 inter-token >75 ms.
- `PERFORMANCE_PASS`: all correctness conditions pass and both thresholds pass.

## Reproducibility Classification

- Token/result/security/count outputs: deterministic; exact match required for
  the same candidate/workload.
- Timing/resource outputs: environment-sensitive; retain environment identity
  and distributions, never require byte-identical timing.
- Re-running with a different SIF, model digest, GPU class, driver, role plan, or
  workload produces a new experiment subject and cannot be pooled as a repeat.

## Stop and Escalation Conditions

- Stop before Tiger if any G0-G4 gate fails.
- Stop a Tiger sequence after the first failed preflight or mandatory functional
  case; do not launch the performance campaign.
- Preserve partial evidence for OOM, timeout, process exit, SIF mismatch, NFD
  failure, or protocol failure; do not silently resubmit.
- Reproduce a Tiger failure at the smallest applicable local gate and update the
  registered test before a new candidate is promoted.
- If G6 is functionally correct but misses performance, close the functional
  feature honestly and open a separate optimization Spec based on measured
  component attribution; do not change Spec 175 defaults post hoc.
