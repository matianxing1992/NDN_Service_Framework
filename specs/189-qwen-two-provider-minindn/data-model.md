# Data Model: Qwen Two-Provider Candidate

## `PreparedQwenModel`

`PreparedQwenModel` is the reusable result of `prepare`, not a model byte buffer.

| Field | Owner | Rule |
| --- | --- | --- |
| `modelKey` | application/config | stable logical name; never identifies bytes alone |
| `sourceDigest` | native prepare | digest of the canonical source identity and revision |
| `graphDigest` | native prepare | digest of canonical ONNX graph with external data references |
| `initializerDigest` / `initializerSize` | native prepare | exact external initializer identity and size |
| `manifestDigest` | Repo | committed manifest identity returned by prepare |
| `layerRefs[2]` | Repo/prepare | two immutable references with `stageIndex`, `start`, `endExclusive`, digest and size |
| `protectionEpoch` | Core authorization | request-time epoch check; not a model default |
| `leaseId` | Runtime | keeps reference and active materialization alive until drain |

## `QwenPlacementPlan`

Selection binds `attemptId`, `manifestDigest`, `protectionEpoch`, `providerId`, `layerRef` and `role` for exactly two providers. Ranges must be non-overlapping and cover layers `0..28` for the initial candidate.

## `LayerPackage`

An immutable Repo object containing one stage graph/weights package and its metadata. It is addressed by manifest digest and layer range. A provider may read only the package named in its authenticated Selection.

## `QwenExecutionEvidence`

The durable record contains candidate tuple, ordered event markers, C++ oracle output, per-provider fetch/assembly/execute counters, resource samples, child exit statuses, cleanup counters and one verdict.

## State transitions

```text
UNPREPARED
  -> PREPARING -> READY_REFERENCE
  -> REQUESTED -> ACKED -> SELECTED
  -> PROVIDER_0_ASSEMBLING -> PROVIDER_0_EXECUTED
  -> PROVIDER_1_ASSEMBLING -> TERMINAL
  -> DRAINED
```

Any digest/epoch/range mismatch transitions to `REJECTED` before fetch. Resource guard, cancellation or provider stop transitions to `ABORTED`, followed by `DRAINED`. No terminal state is PASS without `DRAINED`.
