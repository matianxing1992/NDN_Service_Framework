# Data Model: iTiger Qwen3.6 Generation

## ModelContract

- `modelId`, `revision`, `license`, `parameterDtype`
- `processorRevision`, `chatTemplateSha256`
- `textOnly`, `enableThinking`, `decoding`, `eosTokenIds`, `maxNewTokens`
- `transformersVersion`, `transformersWheelSha256`

Validation: immutable 40-hex revision; BF16; text-only; thinking disabled;
greedy; maximum 64 new tokens.

## StageArtifact

- `stageIndex`, `role`, `startLayer`, `endLayerExclusive`
- `ownsEmbedding`, `ownsFinalNorm`, `ownsLmHead`
- `artifactSha256`, `artifactBytes`
- `estimatedWeightBytes`, `cudaPeakAllocatedBytes`, `cudaPeakReservedBytes`
- `host`, `gpuModel`, `gpuUuid`, `device`, `cpuFallback`

Validation: ranges are `[0,21)`, `[21,42)`, `[42,64)`; union is `[0,64)`;
every accepted load uses an RTX 5000 UUID and no CPU fallback.

## PromptCase

- `promptId`, `messages`, `formattedInputIds`, `inputSha256`
- `referenceTokenIds`, `referenceText`, `referenceSha256`
- `eosTokenIds`, `stopReason`

Validation: prompt requests a concise direct answer; reference ends in EOS
within 64 generated tokens.

## TokenEpoch

- `campaignId`, `generationId`, `promptId`, `repetition`, `phase`
- `tokenIndex`, `requestId`, `contextTokenCount`
- `expectedTokenId`, `actualTokenId`, `exactMatch`
- `submittedAt`, `completedAt`, `latencyMs`
- exactly three `StageReceipt` values
- exactly two `DependencyReceipt` values

## StageReceipt

- request/role/provider/host/GPU/layer-range identity
- start/end/compute timing
- device, backend, CPU-fallback flag
- input/output Data name, bytes, segments, SHA-256
- operation-status sequence

## GenerationRecord

- prompt/repetition/phase identity
- ordered token epochs and output IDs/text
- `status`, `stopReason`, `exactTokenMatch`
- `ttftMs`, `interTokenLatencyMs[]`, `totalLatencyMs`
- `outputTokenCount`, `tokensPerSecond`

State:

```text
CREATED -> RUNNING -> EOS_OK
                   -> TRUNCATED
                   -> MISMATCH
                   -> TIMEOUT
                   -> FAILED
```

Only `EOS_OK` with exact evidence enters successful summaries.

## CampaignSummary

- raw record/evidence digests
- per-prompt counts, completion rate, TTFT/inter-token/total/tokens-s summaries
- pooled descriptive summary
- retained failure counts by reason
- explicit unavailable metrics and claim boundary
