# Contract: Reusable Qwen Preparation

## Required behavior

`prepare(modelKey, PrepareOptions)` resolves the pinned Qwen snapshot, validates graph/initializer/config identity, splits layers, and commits a Repo manifest plus two immutable layer packages. It returns a reference/lease object. It may be called repeatedly; an immutable identity hit returns the existing manifest and does not republish the full model.

The preparation result MUST expose enough metadata for request/Selection binding:

```json
{
  "modelKey": "qwen3-0.6b",
  "graphDigest": "sha256:...",
  "initializerDigest": "sha256:...",
  "initializerSize": 1503264768,
  "manifestDigest": "sha256:...",
  "layers": [
    {"stageIndex": 0, "start": 0, "endExclusive": 14, "digest": "sha256:..."},
    {"stageIndex": 1, "start": 14, "endExclusive": 28, "digest": "sha256:..."}
  ],
  "protectionEpoch": "..."
}
```

The exact native field names remain those of the current source and must be checked with CodeGraph before implementation. This JSON is a contract shape, not permission to add a parallel public API.

## Failure behavior

Digest, size, graph identity, range, schema or protection mismatch rejects before READY. A failed staging write, Repo commit or digest check removes only uncommitted staging and leaves the prior immutable manifest unchanged.
