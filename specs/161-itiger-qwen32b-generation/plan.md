# Implementation Plan: iTiger Qwen 32B Multi-Generation Campaign

**Feature**: [spec.md](spec.md)

**Date**: 2026-07-28

**Status**: In Progress — T001/T002/T003/T005 complete; T004/T006/T007 pending

## Summary

Extend the existing three-stage Qwen Transformers application path from a
single full-context forward pass into bounded greedy generation. Freeze one
Qwen2.5-32B-Instruct FP16 source revision, a matching tokenizer/chat template,
five real prompts, and H100 reference token sequences. For every generated
token, the requester sends the current full token context through the existing
secured three-node collaboration request, appends the returned top token, and
stops on EOS or at 64 tokens. Run one excluded warmup plus five measured
generations per prompt and preserve raw per-token and per-generation evidence.

This first 32B campaign deliberately measures the current full-context
`use_cache=False` layer-pipeline baseline. Distributed KV-cache reuse,
quantization, tensor parallelism, and throughput optimization are separate
future cells and must not be inferred from this campaign.

## Technical Context

**Language/Version**: Python 3.8+ in the coherent Spec 160 runtime; Bash and
Slurm job scripts

**Primary Dependencies**: current NDNSF-DI Python application SDK, PyTorch
2.6.0, Transformers 4.48.2, Qwen2.5 model/tokenizer, NFD, Apptainer

**Storage**: checksum-bound JSON/JSONL/CSV, tokenizer, and immutable stage
artifacts under `/project/tma1/ndnsf-di`; the temporary full 32B source model,
H100 reference workspace, package construction, and download caches stay on
allocation-scoped `$SLURM_TMPDIR`/`/scratch`

**Testing**: Python unit/contract tests without model weights; local fake
collaboration smoke; bounded H100 reference; bounded three-node RTX 5000
acceptance

**Target Platform**: iTiger `bigTiger`, one H100 80GB reference allocation and
three physical RTX 5000 nodes with one GPU per node for distributed acceptance

**Project Type**: distributed inference runtime plus experiment harness

**Performance Goals**: retain all 25 measured samples and reproduce per-prompt
TTFT, inter-token latency, total latency, tokens/s, p50, and p95 from raw data

**Constraints**: 64 generated-token ceiling; greedy decoding; EOS required for
a complete answer; no CPU fallback; no login-node compute; exactly-once live
identities; measured scratch peak; 20 GiB durable reserve; no automatic cleanup
or retry

**Scale/Scope**: Qwen2.5-32B-Instruct revision
`5ede1c97bbab6ce5cda5812749b4c0bdf79b18dd`, 64 layers split over three
stages, five prompts, one warmup and five measured generations per prompt

## Constitution Check

### Pre-design gate

- **Canonical dynamic runtime**: PASS. The experiment reuses generic
  collaboration requests and unified service names; no generated service path
  or Core model special case is introduced.
- **Security in the data path**: PASS. Every token step retains normal
  permission, NAC-ABE, token, replay, assignment, dependency, and response
  behavior.
- **CodeGraph first**: PASS. Existing generation, requester, stage runtime, and
  live harness paths were inspected through CodeGraph before planning.
- **Spec-driven durable work**: PASS. The new model tier and evaluation plan are
  isolated in Spec 161 instead of modifying completed Spec 160 acceptance.
- **Right-scope verification**: PASS. Pure generation and analyzer behavior
  receive local tests; real CUDA/model/network claims require bounded Slurm
  evidence.
- **Cohesive tasks**: PASS. Tests, implementation, focused validation, and
  evidence for each behavior remain together.

### Post-design gate

PASS with one operational stop condition: live preparation remains blocked
until the capacity manifest proves the 20 GiB reserve. Full-context
recomputation is explicitly a measured baseline, not an optimized architecture
claim.

## Architecture and ownership

| Concern | Owner | Decision |
|---|---|---|
| Greedy generation state and EOS classification | LLM pipeline application library | One reusable bounded state machine, independent of NDNSF transport |
| Secured token-step submission | LLM pipeline requester | Each token epoch is one normal collaboration request |
| Layer execution | Existing Qwen stage provider | Reuse three generic stage roles and current hidden-state transport |
| Reference generation | Standalone experiment harness | Same model, dtype, chat template, prompt IDs, and greedy policy |
| Repetition scheduling | Spec 161 campaign harness | One warmup plus five sequential measured generations per prompt |
| Metrics aggregation | Spec 161 analyzer | Derive summaries only from retained raw successful measured records |
| Capacity authority | Spec 161 preflight | Separately gate temporary scratch peak and durable promotion plus reserve; never infer quota |
| Temporary source/reference/build | Allocation-scoped TigerCluster scratch | Select `$SLURM_TMPDIR`, then `/scratch`, then `/tmp`; prove writable/free space; disposable after promotion |
| Tokenizer/stages/evidence | iTiger project storage | Promote only immutable final artifacts and evidence |

No generation state moves into NDNSF Core. The application requester owns token
iteration because it owns prompts, tokenizer semantics, EOS policy, and decoded
text. Providers remain generic stage executors.

## Generation flow

1. Load the frozen prompt case containing formatted input token IDs, reference
   output tokens/text, EOS token IDs, and digests.
2. Start one logical generation identity and an empty generated-token list.
3. For token epoch `n`, encode original input IDs plus generated IDs from
   epochs `[0,n)` as a normal Qwen pipeline context.
4. Submit one secured NDNSF-DI collaboration request with a unique per-token
   request ID.
5. Verify three-stage CUDA/dependency evidence and obtain the final stage's
   top token.
6. Compare the returned token with reference token `n`, append it, and record
   timing/correlation.
7. Stop with `EOS` when the token belongs to the frozen EOS set; stop with
   `TRUNCATED` at 64; fail on mismatch, timeout, malformed response, missing
   evidence, or CPU fallback.
8. Decode only the accepted ordered token sequence with the frozen tokenizer.

## Measurement design

- Five prompt cases, kept separate by stable IDs.
- One warmup generation per prompt, excluded before the measured clock.
- Five sequential measured generations per prompt; no concurrent offered load
  in this correctness/distribution campaign.
- Raw generation records are the source of truth. Summary code filters on
  `phase=measured`, `status=ok`, `stopReason=EOS`, and full exact-token match.
- Per-prompt summaries report count, completion rate, TTFT, total latency,
  per-step/inter-token distribution, output token count, and tokens/s.
- A pooled 25-sample summary is labeled descriptive because prompt lengths
  differ. No stable p99 or scalability claim is made.

## Safety, failure, and rollback

- Preflight blocks before download until an allocation proves enough writable
  scratch for the temporary source/reference/build peak and durable promotion
  retains the project-space reserve.
- The 32B source revision, stage artifacts, tokenizer, prompt manifest,
  reference, source bundle, runtime SIF, and jobs are digest-bound before a
  measured submission.
- Formal live identities are submitted once. A started failure is retained;
  replacement requires a new linked identity and explicit user authorization.
- No automatic retry, quantization, shorter output, changed prompt, changed GPU,
  or deleted failure evidence.
- A code-only rollback removes the new opt-in campaign path while leaving the
  existing one-token path unchanged.
- The public full source-model copy remains only in current-job scratch. After
  final stage/tokenizer promotion, checksum verification, and CUDA load gates,
  the current job removes its own validated scratch prefix; it never removes
  another job's scratch or treats scratch as durable evidence.
- A durable-storage rollback removes only explicitly inventoried, independently
  reproducible artifacts after authorization; evidence and the only copy of a
  model/release are never implicit cleanup targets.

## Project Structure

```text
examples/python/NDNSF-DistributedInference/llm_pipeline/
├── llm_pipeline_lib.py          # bounded greedy generation primitives
└── user.py                      # opt-in distributed generation campaign path

tests/python/
└── test_spec161_qwen_generation.py

specs/161-itiger-qwen32b-generation/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── campaign-manifest.md
│   ├── generation-evidence.md
│   └── prompt-set.json
├── jobs/
│   ├── capacity-preflight.py
│   ├── scratch-capacity-probe.sbatch
│   ├── prepare-reference.sbatch
│   ├── prepare-stages.sbatch
│   ├── generation-rank.sh
│   ├── generation-rank-inner.sh
│   ├── generation-campaign.sbatch
│   └── analyze-generation.py
└── evidence/
    └── preflight.md
```

**Structure Decision**: Keep reusable bounded generation behavior in the
existing LLM application library/requester. Keep model-tier, prompt-set,
capacity, Slurm, and aggregation behavior inside the Spec 161 experiment
directory.

## Complexity Tracking

No constitution violations require justification. Full-context recomputation
is retained as an explicit baseline because implementing distributed KV-cache
ownership would materially broaden the feature and create a different
architecture/experiment identity.
