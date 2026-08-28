# Code Experiment Plan

## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: plan
- Origin Date: 2026-08-04
- Verification Status: UNVERIFIED
- Version Label: spec168_t011_plan_v1

## Experiment Overview

- **Title**: Qwen3-0.6B three-node cold preparation versus identity-compatible GPU reuse
- **Objective**: determine whether repeated NDNSF-DI requests in one unchanged
  TigerCluster allocation reuse the exact Provider/model/graph/partition/device
  residency created by the first request, without duplicate Repository transfer
  or redundant device loading, while preserving the complete authenticated
  response lifecycle.
- **Hypothesis**: after the first cold request loads one assigned shard on each
  GPU, all compatible later requests select the same three Provider roles with
  `REUSE_CACHED`; their cumulative Repository unique-byte and device-load
  counters do not increase, while GPU-hit counters do increase.
- **Type**: distributed systems experiment with deterministic greedy inference.

## Variables and controls

- Independent variable: request cache class (`COLD` first sample versus
  identity-compatible `WARM` later samples).
- Primary dependent variables: per-Provider deltas of `repoUniqueBytes`,
  `repoWireBytes`, `deviceLoadCount`, `gpuHitCount`, and the placement
  preparation mode.
- Secondary dependent variables: TTFT, per-token latency, total latency and
  tokens/s, summarized per prompt and cache class.
- Controls: same allocation, Provider boot epochs, GPU UUIDs, SIF digest,
  model content/semantics digest, graph/partition digest, stage artifacts,
  source identity, routes, security policy, greedy decoding and maximum 64
  generated tokens.
- Confound handling: latency is not accepted as proof of reuse; a warm sample
  is admitted only when all identity fields match and the transfer/load deltas
  are zero. Provider restart, eviction, reassignment or identity drift changes
  the cache class and fails the unchanged-warm assertion.

## Setup

- **Runtime**: the existing sealed NDNSF-DI SIF on three TigerCluster RTX 5000
  Ada nodes, one stage per node.
- **Workload**: existing `formal-campaign.json`, SHA-256
  `d106b82508e1714f8cf542f83ac8c1af06e56376408534422ea71f7dce105001`.
- **Schedule**: five real prompts, one warmup plus five measured invocations per
  prompt, sequential, for exactly 30 rows.
- **Assets**: reuse the existing Qwen3-0.6B stage manifest, tokenizer, Repository
  payload, SIF, policy and route configuration; do not rebuild or re-prepare.

## Expected outputs and success criteria

| Output | Format | Success criterion |
|---|---|---|
| generation rows | JSONL | exactly 30 unique request IDs; all `OK`; complete nonempty answers; 1 FULL wire request and 0 token requests per row |
| placement evidence | log/JSON | first row `GENERATED`; every unchanged later row `REUSE_CACHED` with exact ACK/score evidence |
| residency evidence | JSON markers | three stable boot epochs/GPU UUIDs; first row loads once; later rows add zero unique Repo bytes and zero device loads, and add GPU hits |
| lifecycle evidence | marker logs | every request has ACK closure, committed Selection, causal stage dependency flow and one terminal Response |
| metric analysis | JSON/Markdown | per-prompt and per-cache distributions for TTFT, inter-token latency, total latency and tokens/s; warm/cold claims remain causal-counter based |
| security evidence | manifest/log | no plaintext tokens or private keys retained; no CPU fallback; authenticated request/response lineage preserved |

## Monitoring configuration

- Monitor Slurm state plus request-progress markers; use hard and no-progress
  deadlines, never a fixed Provider-settle sleep.
- Preserve the last completed request and stage checkpoint on failure.
- No automatic retry. A failed phase closes its identity and returns to a local
  or exact-container reproducer.

## Analysis plan

- Report every row; do not discard failures or warmups.
- Use medians, quartiles, minimum and maximum for the five measured repetitions
  per prompt. With only five repetitions, do not claim population-level
  significance or fit inferential models.
- Treat prompt length/output length as controlled strata, not as noise to pool
  indiscriminately.
- The primary result is the counter/identity invariant. Latency distributions
  characterize operational benefit but cannot establish cache reuse by
  themselves.
