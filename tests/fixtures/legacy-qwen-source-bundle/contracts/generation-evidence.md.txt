# Contract: Qwen3.6 Generation Evidence

## Campaign shape

```text
prompts: 5
warmup per prompt: 1
measured per prompt: 5
measured generations: 25
offered load: sequential
maximum generated tokens: 64
success stop reason: EOS
```

## Required raw files

- immutable candidate/source/runtime/model/prompt manifests;
- reference token/text records;
- generation JSONL with warmup and measured rows;
- token-epoch JSONL;
- stage receipt JSONL;
- dependency receipt JSONL;
- operation-status snapshots;
- Slurm stdout/stderr, node/GPU map, exit record;
- checksum manifest.

After architecture requalification, the raw evidence additionally requires:

- placement strategy name, version, and state digest;
- validated provider ACK snapshot digest;
- exact model graph/candidate digest and the selected split materialization
  record;
- whether the initial catalog was `REQUIRES_DISTRIBUTED_REPO_REGISTRATION` or
  an already active content-addressed snapshot;
- Repo publication start/completion, root-manifest receipts, and the exact
  artifact digest-to-Data-name binding;
- sealed collaboration-plan digest;
- original inference request identity and attempt;
- provider identities and boot epochs;
- final Selection digest under that same request/attempt;
- proof that each Provider used distinct purpose-bound SelectionToken values
  and consumed each at most once;
- message counts proving one Request publication, one ACK closure, one final
  Selection commit, and no second inference Request in the accepted attempt;
- per-role model-fetch/load completion plus data-driven execution readiness:
  Stage 0 requires its request input, while later stages require the exact
  predecessor dependency receipt;
- selected preparation action and compensation/release outcome.
- request-gate marker proving that the User opened the request immediately
  after process/route readiness and that no fixed Provider/User settle was
  used; this marker is distinct from the protocol `ACK_CLOSED` event.

## Required generation fields

Every row records full answer text, ordered token IDs, status, stop reason,
TTFT, total latency, output length, tokens/s, prompt/repetition/phase,
exact-match state, and evidence correlation outcome. When available, internal
stage progress may additionally record inter-token timing, but this is a
data-plane metric only: it MUST NOT be represented as a separate NDNSF
Request/ACK/Selection cycle.

## Summary rules

Successful distributions include only measured, EOS-terminated, exact-match,
three-stage, two-dependency, zero-fallback generations. All excluded rows
remain in raw evidence and are counted by reason.

Per-prompt p50 and descriptive p95 are reported from five measured values only
with a small-sample warning. The pooled 25-row view is labeled descriptive
because prompt lengths and outputs differ. p99 is unavailable.

## Request-driven preparation evidence

The accepted lifecycle is `Request -> ACK_CLOSED -> candidate planning and
split publication -> final Selection -> Provider-local preparation ->
dependency-driven stage execution -> Response`; a fixed Provider/User settle
interval is not a correctness condition. For every cold-request stage, raw
evidence must retain monotonic `LLM_PIPELINE_QWEN_REPO_FETCH_PROGRESS` records,
one `LLM_PIPELINE_QWEN_REPO_FETCH_COMPLETE` record with byte/segment coverage,
and a stage output or final-response publication record. The User must persist
exactly one `LLM_PIPELINE_GENERATION_FINAL_RESPONSE` marker whose SHA-256
matches the full answer stored in generation JSONL. Rank cleanup copies these
logs before terminating runtime processes, including on cancellation.
The User log must also contain `NDNSF_DI_AUTOPLANNING_GRAPH_READY` after the
matching `ACK_CLOSED` marker. This marker proves that graph inspection and
candidate enumeration used the actual ACK snapshot rather than a deployment-
time Provider-settle assumption.

The complete-generation API is one durable invocation. The input token sequence
is prefetched once; autoregressive decode steps may exchange hidden-state/KV
data between stages, but MUST remain inside that invocation. They MUST NOT
reopen ACK collection or rerun placement/Selection. The normative wire result
is one complete Response. Any typing animation or other incremental rendering
is a local presentation of that completed result, not a `ResponseChunk` wire
stream and not an additional collaboration.
