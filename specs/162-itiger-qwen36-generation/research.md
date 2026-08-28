# Research: iTiger Qwen3.6 Distributed Generation

## Material Passport

- Origin Skill: academic-research-suite experiment-agent
- Origin Mode: plan
- Origin Date: 2026-07-28
- Verification Status: ANALYZED
- Version Label: code_plan_v1

## Decision 1: Freeze Qwen/Qwen3.6-27B

**Decision**: Use `Qwen/Qwen3.6-27B` at immutable Hugging Face revision
`6a9e13bd6fc8f0983b9b99948120bc37f49c13e9`.

**Rationale**: The official Qwen repository describes Qwen3.6 as the latest
Qwen family release. The official model card identifies the 27B checkpoint as
a 64-layer dense causal language model with a vision encoder, 27,781,427,952
BF16 parameters, Apache-2.0 license, and text-only serving support.

**Sources**:

- https://github.com/QwenLM/Qwen3.6
- https://huggingface.co/Qwen/Qwen3.6-27B
- https://huggingface.co/Qwen/Qwen3.6-27B/raw/main/config.json

**Alternatives considered**:

- Qwen3-14B: simpler and already close to the current Qwen2 runtime, but not the
  newest Qwen 3.x release requested by the user.
- Qwen3.6-35B-A3B: newer MoE option, but expert routing changes the experiment
  and stage memory/ownership semantics.

## Decision 2: RTX-only preparation and three-node candidate

**Decision**: Generate the full reference on one RTX 5000 node using three
allocated GPUs; run the candidate on three distinct RTX 5000 nodes with one GPU
and one stage per node.

**Rationale**: Live iTiger discovery shows five RTX 5000 nodes with eight GPUs
per node. The BF16 checkpoint is about 55.6 GB of parameter bytes, so a
multi-GPU reference avoids H100 while three contiguous stage artifacts are
plausible on 32 GB GPUs. Plausibility is not acceptance: each frozen stage must
pass an allocation-measured CUDA memory/load gate before live collaboration.

**Alternatives considered**:

- H100 reference: technically simple but conflicts with the user's resource
  choice and is currently queue-constrained.
- Cross-node tensor-parallel oracle: adds communication and framework behavior
  to the reference; a same-node three-GPU reference is a cleaner oracle.

## Decision 3: Stage ranges [0,21), [21,42), [42,64)

**Decision**: Use three contiguous ranges `[0,21)`, `[21,42)`, and `[42,64)`.

**Rationale**: They cover all 64 layers once and keep counts at 21/21/22. Stage
0 owns input embeddings; Stage 2 owns final normalization and LM head. Exact
artifact bytes and CUDA peak memory remain a mandatory preflight because those
fixed components make average layer arithmetic insufficient.

## Decision 4: Add a Qwen3.5/3.6 hybrid-layer adapter

**Decision**: Add a new fail-closed `qwen3_5` stage implementation; do not
pretend the existing Qwen2 adapter is compatible.

**Rationale**: Current source imports `Qwen2DecoderLayer` and rejects every
model type except `qwen2`. Qwen3.6 uses `Qwen3_5ForConditionalGeneration` with
`Qwen3_5TextModel`, alternating three Gated DeltaNet layers and one gated full
attention layer. PyPI Transformers 5.14.1 contains the required public classes;
the historical 4.57.1 wheel does not, despite the model config carrying that
older generation-time version.

**Runtime lock**: Candidate runtime must pin Transformers 5.14.1 wheel SHA-256
`9db974c4079ede2d1a3ea7ca5a240df33f2cc26fc2b36ba64c5f2a4f43b6e725`
or a later explicitly audited replacement. The configured local pip mirror is
not authoritative because it currently stops at 4.46.3.

## Decision 5: Text-only, non-thinking, deterministic generation

**Decision**: Freeze text-only input, `enable_thinking=False`, greedy decoding,
EOS, and 64 generated tokens.

**Rationale**: Qwen3.6 thinks by default and the official card recommends much
larger output budgets. The user explicitly fixed 64 tokens, so concise prompts
and direct-response mode are necessary. Greedy decoding provides exact
reference equivalence; the campaign measures latency distribution, not output
sampling diversity.

## Decision 6: Five by five descriptive campaign

**Decision**: Five prompts, one excluded warmup and five sequential measured
generations per prompt; retain per-prompt and pooled descriptive distributions.

**Rationale**: This yields 25 measured complete generations and exposes
run-to-run latency variation. It is enough for p50 and descriptive p95 reporting
with explicit small-sample caution, not stable p99 or causal performance claims.

## Rejected inference claims

- no claim of production readiness;
- no claim of throughput scaling or speedup;
- no claim of distributed KV-cache or MTP;
- no claim that one matching token is a complete answer;
- no claim that theoretical parameter division proves 32 GB fit.
