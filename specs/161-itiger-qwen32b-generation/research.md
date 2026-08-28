# Research: iTiger Qwen 32B Multi-Generation Campaign

## Decision 1: Model and immutable revision

**Decision**: Use `Qwen/Qwen2.5-32B-Instruct` revision
`5ede1c97bbab6ce5cda5812749b4c0bdf79b18dd`, Apache-2.0, unquantized FP16.

**Rationale**: The official Hugging Face model record identifies this revision,
Transformers text-generation runtime, and Apache-2.0 license. Estimated FP16
weights are about 60.54 GiB: too large for one 32 GiB RTX 5000 but plausibly
partitionable across three such GPUs. A same-model H100 80GB reference remains
a live capability hypothesis until allocated.

**Alternatives considered**:

- 14B: likely fits one RTX 6000 and does not strongly prove the requested
  three-RTX-5000 capacity boundary.
- 72B: estimated preparation peak exceeds the current 200 GiB planning envelope
  before the required reserve.
- Quantized 32B: a different correctness/performance cell and not the requested
  first full-precision baseline.

Primary record:
`https://huggingface.co/Qwen/Qwen2.5-32B-Instruct/tree/main`.

## Decision 2: Layer partition

**Decision**: Freeze 64 transformer layers into `[0,21)`, `[21,42)`, and
`[42,64)` after the staged manifest verifies the actual model configuration.

**Rationale**: The existing `split_layer_ranges` algorithm distributes the one
remainder layer through floor-divided boundaries, leaving the final stage with
22 layers, and gives complete, non-overlapping coverage. Each stage receives
roughly one third of layer weights.

**Alternatives considered**:

- Move the remainder layer to Stage 0: inconsistent with the existing generic
  splitter and provides no demonstrated advantage.
- Dynamic placement: adds an uncontrolled experiment variable.

## Decision 3: Instruction formatting and reference

**Decision**: Apply the frozen Qwen tokenizer chat template to one user message
with `add_generation_prompt=true`; use greedy decoding with the tokenizer/model
EOS set and no sampling.

**Rationale**: Qwen2.5-Instruct expects instruction/chat formatting. Plain
tokenization of raw prompt text, as used by the Spec 160 one-token capability
probe, is not sufficient evidence of answer generation. Greedy decoding
produces an exact token oracle and keeps runtime variation separate from
sampling randomness.

**Alternatives considered**:

- Raw prompt tokenization: does not exercise the intended instruction format.
- Temperature/top-p sampling: prevents exact sequence comparison and confounds
  repeated runtime measurements with content randomness.

## Decision 4: Full-context baseline before KV-cache optimization

**Decision**: Each token epoch resubmits original formatted input IDs plus all
previously generated token IDs; the provider continues to execute with
`use_cache=False`.

**Rationale**: This is the smallest extension of the real, accepted Spec 160
Qwen Transformers stage path. Current distributed KV-cache support is not
validated for this Transformers path. Introducing cache ownership, persistence,
recovery, and security semantics would create a separate architecture feature.

**Alternatives considered**:

- Reuse the current ONNX native cache path: it is a different backend and a 32B
  ONNX export would materially increase storage and validation risk.
- Implement distributed KV cache now: valuable follow-up, but not required to
  prove complete generation and repeated distributions.

**Limitation**: Report the result as a full-context baseline. It is expected to
be slower than a cache-enabled production generator.

## Decision 5: Repetition and distribution

**Decision**: For each of five frozen prompts, run one excluded warmup followed
by five sequential measured generations. Report per-prompt raw values and
p50/p95, plus a descriptive pooled summary.

**Rationale**: This directly satisfies the user's request for multiple
inferences in one experiment while avoiding concurrent-load queueing as a
confound. Five repetitions are descriptive and preserve visible variation; they
are not enough for a stable p99 or hypothesis test.

**Alternatives considered**:

- One prompt repeated 25 times: stronger for one workload but does not prove
  several real inputs generate complete answers.
- Concurrent open loop: measures capacity/queueing rather than the requested
  initial correctness distribution.

## Decision 6: TigerCluster scratch for preparation, project space for promotion

**Decision**: In one bounded Slurm allocation, select writable storage from
`$SLURM_TMPDIR`, then `/scratch`, then `/tmp`; place the public full-model
download, Hugging Face cache, standalone-reference workspace, and stage-package
construction there. Promote only the verified stage packages, tokenizer/chat
template, manifests, and evidence to `/project/tma1/ndnsf-di`. Gate scratch
peak separately from durable promotion and retain 20 GiB durable reserve.

**Rationale**: TigerCluster provides large compute-node scratch specifically for
temporary preparation. Keeping both the roughly 60.54 GiB public source model
and roughly one-model-size final stage set in durable project storage would
duplicate reconstructible bytes. Read-only discovery measured 67 GiB already
under the project root, including about 29 GiB of retained SIF releases, while
the personal project quota command is unavailable. Separating scratch peak from
durable promotion avoids projecting the whole roughly 147.26 GiB preparation
peak onto project storage.

**Alternatives considered**:

- Put source/cache/build under `/project`: unnecessarily duplicates
  reconstructible model bytes in durable storage.
- Download first and choose scratch later: risks an allocation-local capacity
  failure after transfer begins.
- Delete old releases automatically: destructive, may remove the only
  reproducible artifact, and violates evidence retention.

**Scratch limitation**: Scratch is disposable and has no retention guarantee.
The job must promote checksums, manifests, logs, reference tokens, and final
stage/tokenizer artifacts before removing only its own validated scratch prefix.
