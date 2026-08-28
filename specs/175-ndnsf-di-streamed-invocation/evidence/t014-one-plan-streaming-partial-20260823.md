# T014 one-plan streamed generation (partial)

**Date**: 2026-08-23  
**Status**: partial; multi-Provider exact-oracle integration remains open

## Implemented

- The deferred collaboration stream bridge reuses the original Request ID and
  Core lifecycle; it does not create a per-token Request or second plan.
- `OnePlanGenerationLoop` performs one prefill, incremental decode epochs,
  final-role sampling, separate internal activation/token-feedback names, and
  one terminal callback. Activation names use the selected role-to-Provider
  mapping rather than a fixed Provider map.
- The example Qwen full-generation path publishes opaque
  `GenerationTokenEventV1` token IDs through the Core writer when streaming is
  requested and completes through the same streamed terminal Response.
- The native role execution context now exposes a bounded, terminal-role-only
  `StreamEventSink`.  `ProviderRoleWorker` carries it through the existing
  queue/runtime path, so an incremental native adapter can submit each
  admitted token event without placing events in dependency tensor bundles;
  focused native worker regressions verify callback propagation and prevent
  the exact-forward cache from suppressing streamed side effects.
- `NativeModelRunner::runStreamed(...)` is now an optional incremental hook.
  The checked-in tiny causal ONNX fixture drives the real C++
  `OnnxRuntimeModelRunner` through eight persistent state epochs, emits the
  exact `4,5,6,7,8,9,10,2` event sequence, stops on EOS, and returns one final
  payload.  This direct adapter test is intentionally below formal G2 because
  it does not include the NDNSF multi-Provider Request/ACK/Selection path.
- The committed tiny CPU ONNX fixture now drives this same loop for the
  one-role, two-role, and four-role partitions. Each partition uses one real
  ORT prefill, then one incremental decode call per remaining oracle token;
  the expected `[4,5,6,7,8,9,10,2]` sequence and EOS terminal are exact, and
  activation names carry the selected Provider identity for every role edge.
- A real zero-event integration case now accepts a Response that arrives
  before End by retaining it until exact End delivery closes the cursor.

## Verification

```text
PYTHONPATH=pythonWrapper:NDNSF-DistributedInference:NDNSF-DistributedRepo/pythonWrapper \
  python3 -m pytest -q \
    tests/python/test_streamed_invocation_api.py \
    tests/python/test_spec175_streamed_generation.py
16 passed

The native sink/cache regressions pass as part of the rebuilt unit target
(`build/unit-tests --log_level=message`: 562/562).  The direct adapter
integration case passes as part of the rebuilt integration target
(`build/integration-tests --run_test=Spec175InvocationStream/NativeTinyOnnxAdapterRunsIncrementalStream`).
These are adapter-boundary evidence, not yet formal I01-I03 cases: the native
multi-Provider tiny-ONNX Request/ACK/Selection/decode path still has to be
wired before the formal G2 registry can be populated.

./build/integration-tests --run_test=Spec175InvocationStream --log_level=message
6 test cases; no errors detected
```

The native I01-I03/I15 process cases are still unregistered: the Python test
proves the real CPU ONNX loop and state continuity, but does not close the
NDN multi-Provider process requirement. Final-role ownership and native
end-to-end activation/feedback lineage remain open.
