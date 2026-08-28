# Data Model: iTiger Qwen 32B Multi-Generation Campaign

## CampaignManifest

Fields:

- `schemaVersion`
- `campaignId`
- `candidateId`
- `modelRepository`
- `modelRevision`
- `modelDigest`
- `dtype`
- `quantization`
- `tokenizerDigest`
- `chatTemplateDigest`
- `promptSetDigest`
- `runtimeSifSha256`
- `sourceBundleSha256`
- `stageManifestSha256`
- `generationPolicy`
- `repetitionPolicy`
- `allocation`
- `capacityDecisionSha256`

Validation:

- Immutable identity fields are non-empty and checksum-bound.
- `dtype=fp16`; `quantization=none`.
- `maxNewTokens=64`; `strategy=greedy`.
- Exactly five prompt cases and five measured repetitions.
- Exactly three distinct nodes and one RTX 5000 per stage at acceptance.

## PromptCase

Fields:

- `promptId`
- `text`
- `textSha256`
- `formattedInputIds`
- `formattedInputSha256`
- `inputTokenCount`
- `eosTokenIds`
- `referenceGeneratedTokenIds`
- `referenceGeneratedTokenSha256`
- `referenceDecodedText`
- `referenceStopReason`

Validation:

- Prompt IDs are unique and stable.
- Text and formatted token sequence are non-empty.
- Reference output contains a non-special token and terminates with EOS within
  64 new tokens.

## GenerationSample

Fields:

- `campaignId`
- `promptId`
- `phase` (`warmup` or `measured`)
- `repetition`
- `generationId`
- `startedAt`
- `endedAt`
- `status`
- `stopReason`
- `inputTokenCount`
- `generatedTokenIds`
- `generatedTokenSha256`
- `decodedText`
- `exactReferenceMatch`
- `ttftMs`
- `interTokenMs`
- `totalMs`
- `tokensPerSecond`
- `tokenSteps`
- `error`

State transitions:

```text
CREATED -> ACTIVE -> EOS -> OK
                  -> TOKEN_LIMIT -> TRUNCATED
                  -> MISMATCH -> FAILED
                  -> TIMEOUT -> FAILED
                  -> CANCELLED
```

Only `phase=measured`, `status=OK`, `stopReason=EOS`, and
`exactReferenceMatch=true` records enter successful latency percentiles.

## TokenStep

Fields:

- `generationId`
- `tokenEpoch`
- `requestId`
- `contextTokenCount`
- `contextSha256`
- `expectedTokenId`
- `actualTokenId`
- `startedAt`
- `endedAt`
- `durationMs`
- `stageReceipts`
- `dependencyReceipts`
- `status`
- `error`

Validation:

- Token epochs begin at zero and increase contiguously.
- Request IDs are unique inside a campaign.
- Actual token equals expected token for an accepted step.
- Exactly three CUDA stage receipts and two cross-node dependency receipts are
  correlated.

## CapacityDecision

Fields:

- `measuredAt`
- `projectRoot`
- `currentUsageBytes`
- `verifiedQuotaBytes`
- `quotaAuthority`
- `scratchPath`
- `scratchCapacityAuthority`
- `scratchFreeBytes`
- `scratchWritable`
- `modelSourceBytes`
- `stageArtifactBytes`
- `scratchTemporaryPeakBytes`
- `durablePromotionBytes`
- `runtimeBytes`
- `evidenceBytes`
- `reserveBytes`
- `projectedDurableBytes`
- `allowed`
- `blockReasons`

Validation:

- `allowed=false` when quota authority is unavailable.
- `allowed=false` when scratch is not allocation-measured, writable, and large
  enough for the temporary peak plus safety margin.
- `allowed=false` when projected durable promotion plus 20 GiB reserve exceeds verified
  quota.
- The decision is regenerated before download and before formal submission.
