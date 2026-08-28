# T009 Full-Invocation TigerCluster Evidence — 2026-08-03

This record closes the request-first complete-generation probe without
repeating model preparation. Jobs 181946–181948 reused the same runtime SIF,
content-addressed Qwen3-0.6B stage manifest, and three RTX 5000 allocations.

## Immutable identities

```text
SIF SHA-256:             1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368
stage manifest SHA-256:  8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5
successful source SHA:   ed6a7e0168c3d63720dbfe4f58c4fa2e6ccb796550cbd5721268c7dabbc02513
successful job:          181948
successful submission:   spec162-submission-t009-full-20260803T044700Z-003
raw evidence:            /project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-full-20260803T044700Z-003.partial
nodes:                   itiger09, itiger10, itiger11
GPU:                     NVIDIA RTX 5000 Ada Generation (one per node)
```

The partial suffix is intentional: the Slurm wrapper retained the immutable
runtime evidence but returned `state=FAIL` during its old analyzer step. The
wrapper result is not rewritten.

## Runtime result

The retained `node-0/generation-raw.jsonl` contains one successful row:

```text
status=OK
exactReferenceMatch=true
generatedTokenCount=47
stopReason=EOS
wireRequestCount=1
tokenRequestCount=0
interTokenMs=[]
decodedText=NDN 按内容名称转发与 IP 按主机地址转发的核心区别在于：前者基于内容（如文件、消息等）进行转发，而后者基于网络地址（如IP地址）进行转发。
```

Each Provider fetched and verified its content-addressed artifact, reported
CUDA residency with `cpuFallback=false`, and executed the FULL stage loop.
The provider marker summaries were:

```text
Stage 0: 47 FULL progress events, 9 Repo progress events, fetch complete
Stage 1: 47 FULL progress events, 17 Repo progress events, fetch complete
Stage 2: 47 FULL progress events, 23 Repo progress events, fetch complete
```

## Failure and correction history

181946 and 181947 reached the same three-stage EOS computation but the
automatic user branch called `AutomaticInferenceHandle.result()`. That API
returns the adapter-decoded `bytes`; the caller then incorrectly accessed
`.payload`, producing:

```text
'bytes' object has no attribute 'payload'
```

The correction calls `handle.response()` at the transport boundary, preserving
the raw `ServiceResponse` for request-ID and payload validation. No Core,
Repo, SIF, or model change was needed.

The first post-correction wrapper analyzer also failed for an evidence-only
reason: the sealed runtime emits `LLM_PIPELINE_QWEN_MODEL_RESIDENCY` instead of
the newer explicit artifact-ready marker, and stage 0 emits one terminal
`LLM_PIPELINE_QWEN_FULL_GENERATION_FINAL` marker rather than one per token. The
corrected analyzer accepts the equivalent residency certificate and requires
the stage-0 terminal marker once.

Post-hoc command, run against the immutable partial evidence:

```bash
python3 jobs/analyze-generation-full-smoke.py \
  --root <181948-partial> \
  --stage-manifest <existing-stage-manifest.json> \
  --output-json analysis-revalidated.json \
  --enriched-jsonl generation-enriched-revalidated.jsonl
```

The analyzer source used for this revalidation is bundle
`3cee5d3d87cc9e0c48a322c4f0bf9b6f6f3bf9dc9d1450a6f5e89fe02b858194`.
Result: `status=PASS`, `wireRequestCount=1`, `tokenRequestCount=0`, exact
answer match, three stage receipts, and all three Repo fetch completions.

## Design conclusion

The application contract is one durable FULL invocation: complete input token
sequence in one request, one ACK/strategy/Selection lifecycle, internal
prefill and autoregressive hidden-state/token exchange, and one complete
Response. Internal token epochs are data-plane progress and are not separate
NDNSF Requests. A caller may render the completed answer incrementally after
receipt, but presentation rendering is outside the NDNSF-DI wire contract.
