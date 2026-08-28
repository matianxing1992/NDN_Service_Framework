# Campaign Manifest Contract

Schema: `ndnsf-di-qwen-generation-campaign-v1`

Required top-level shape:

```json
{
  "schemaVersion": "ndnsf-di-qwen-generation-campaign-v1",
  "campaignId": "spec161-qwen32b-generation-001",
  "candidateId": "sha256:<digest>",
  "model": {
    "repository": "Qwen/Qwen2.5-32B-Instruct",
    "revision": "5ede1c97bbab6ce5cda5812749b4c0bdf79b18dd",
    "dtype": "fp16",
    "quantization": "none",
    "digest": "sha256:<digest>"
  },
  "tokenizer": {
    "digest": "sha256:<digest>",
    "chatTemplateDigest": "sha256:<digest>",
    "eosTokenIds": [151643, 151645]
  },
  "generation": {
    "strategy": "greedy",
    "maxNewTokens": 64,
    "requireEos": true
  },
  "repetitions": {
    "warmupPerPrompt": 1,
    "measuredPerPrompt": 5,
    "sequential": true
  },
  "promptSetPath": "contracts/prompt-set.json",
  "promptSetSha256": "sha256:<digest>",
  "capacityDecisionSha256": "sha256:<digest>",
  "artifacts": {
    "runtimeSifSha256": "sha256:<digest>",
    "sourceBundleSha256": "sha256:<digest>",
    "stageManifestSha256": "sha256:<digest>",
    "referenceSha256": "sha256:<digest>"
  },
  "allocation": {
    "reference": {"gpuClass": "h100_80gb", "gpuCount": 1},
    "distributed": {
      "nodes": 3,
      "gpuClass": "rtx_5000",
      "gpusPerNode": 1
    }
  }
}
```

The concrete EOS IDs above are illustrative until read from the frozen
tokenizer. The promoted manifest must contain the tokenizer-derived values and
their digest.

Changing model revision, tokenizer/chat template, prompt bytes, generation
policy, repetition policy, stage manifest, SIF, source bundle, or allocation
creates a new candidate identity.
