# T014 native incremental ONNX adapter checkpoint (2026-08-23)

## Scope

This checkpoint verifies the new native adapter seam, not the formal G2
process matrix. `NativeModelRunner::runStreamed(...)` is optional; a runner
that does not implement it retains the existing one-shot worker path. The
stateful ONNX implementation uses one persistent ORT session and feeds the
three declared state families (`attention_kv`, `recurrent_state`, and
`convolution_state`) back into the next epoch.

## Verification

```text
./waf build --target=unit-tests -j2
./build/unit-tests --log_level=message
  -> PASS (562/562)

./waf build --target=integration-tests -j2
./build/integration-tests \
  --run_test=Spec175InvocationStream/NativeTinyOnnxAdapterRunsIncrementalStream \
  --log_level=message
  -> PASS
```

The tiny fixture produces the exact greedy sequence `4,5,6,7,8,9,10,2`.
Eight serialized token events are accepted by the sink, the final event is
marked `EOS`, and the returned `NDNSF-DI-FINAL-V1` payload contains the same
token IDs. A separate worker regression proves that streamed executions do
not use the exact-forward TensorBundle cache to suppress external events.

## Boundary

This is stronger than the earlier one-event native smoke and proves the C++
stateful adapter/worker contract. It is still not I01: it does not run the
native multi-Provider NDNSF Request/ACK/Selection/plan/Selection path, does
not prove activation/feedback lineage, and is not registered in the fail-closed
G2 case map. I01-I15, G0 sealing, SIF, MiniNDN, and Tiger evidence remain open.
