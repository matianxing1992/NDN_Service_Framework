# Generation Evidence Contract

Schema: `ndnsf-di-qwen-generation-sample-v1`

Each JSONL row represents one complete warmup or measured generation:

```json
{
  "schemaVersion": "ndnsf-di-qwen-generation-sample-v1",
  "campaignId": "spec161-qwen32b-generation-001",
  "promptId": "ndn-vs-ip",
  "phase": "measured",
  "repetition": 0,
  "generationId": "spec161-qwen32b-generation-001-ndn-vs-ip-m0",
  "status": "OK",
  "stopReason": "EOS",
  "inputTokenCount": 42,
  "generatedTokenIds": [123, 456],
  "generatedTokenSha256": "sha256:<digest>",
  "decodedText": "answer",
  "exactReferenceMatch": true,
  "ttftMs": 0.0,
  "interTokenMs": [0.0],
  "totalMs": 0.0,
  "tokensPerSecond": 0.0,
  "tokenSteps": [],
  "error": ""
}
```

Allowed `status` values:

- `OK`
- `FAILED`
- `TRUNCATED`
- `TIMEOUT`
- `CANCELLED`

Allowed `stopReason` values:

- `EOS`
- `TOKEN_LIMIT`
- `TOKEN_MISMATCH`
- `EMPTY_OUTPUT`
- `REQUEST_FAILURE`
- `TIMEOUT`
- `CANCELLED`

Every `tokenSteps` entry must correlate one token epoch to:

- unique NDNSF request ID;
- expected and actual token ID;
- three role/provider/node/GPU/layer/backend receipts;
- two dependency Data names, digests, byte/segment counts, and producer/consumer
  identities;
- request start/end and terminal status.

Analyzer rules:

1. Never discard a row.
2. Include only measured `OK` + `EOS` + exact-match rows in successful latency
   percentiles.
3. Report completion and each failure class over all 25 measured rows.
4. Preserve per-prompt summaries.
5. Label pooled statistics descriptive and do not report stable p99.
