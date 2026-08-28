# Spec 168 bounded large-model local gate

**Status: PASS (metadata/fixture only).** This gate intentionally does not
materialize Qwen3.6-27B or any other large-model weight on the 8 GiB host.
CUDA loading, large-shard fetching, and inference remain TigerCluster-only.

## Verified locally

- The pinned Qwen3.6-27B adapter emits one immutable three-stage graph with
  contiguous layer ranges `(0,21)`, `(21,42)`, `(42,64)` and exactly two direct
  hidden-state dependencies.
- A bounded arithmetic fixture makes the complete model exceed one RTX 5000
  capacity while each individual stage, including workspace, activation,
  transient bytes, and the 1.10 safety margin, fits the recorded GPU budget.
  Invalid non-contiguous ranges fail closed.
- Stage artifact digests and the adapter-generated graph identity remain bound
  to the candidate; no path-based or display-name-only reuse is accepted.
- `AdaptiveArtifactTransfer` proves a bounded window/backlog, unequal segment
  arrival, duplicate suppression, timeout retransmission, and retry counters.
- `AtomicArtifactDestination` proves out-of-order verified ranges, durable
  resume checkpoints, incomplete destinations remaining hidden, and rejection
  of an artifact identity mismatch.

## Verification

```text
PYTHONPATH=.:pythonWrapper:NDNSF-DistributedInference:NDNSF-DistributedRepo/pythonWrapper \
  python3 tests/python/test_spec168_large_model_gate.py                 3 passed
```

This is the required non-materializing local gate. It does not satisfy the
large-model response requirement (T015); that requires one admitted, immutable
three-node TigerCluster request after the small-model schedule and defect
closure gates pass.
