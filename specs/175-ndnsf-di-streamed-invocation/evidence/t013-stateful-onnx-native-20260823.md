# T013 native stateful ONNX checkpoint (2026-08-23)

## Scope

This checkpoint closes the first native boundary for the stateful ONNX
contract. It is deliberately a small CPU regression, not a claim that the
Qwen3.6-27B CUDA bundle or the multi-Provider G2 cases are complete.

## Test

`DiOnnxRuntimeGpuEvidence/StatefulTinyOnnxRunsTwoRolesWithPersistentState`
loads the checked-in `tiny-causal-lm-v1/two-role` fixture through the real
`OnnxRuntimeModelRunner`. Role 0 consumes `input_ids` and its recurrent state;
its `hidden_out` is passed to role 1. Role 1 produces logits and updated state.
All state outputs are fed back into the next decode step, so the test exercises
persistent state across eight incremental steps rather than eight independent
sessions.

The expected greedy sequence is `4,5,6,7,8,9,10,2`. The test also binds the
declared state input/output names and checks the argmax at every step.

## Reproduction

```text
./waf build -j8
build/unit-tests --run_test=DiOnnxRuntimeGpuEvidence/StatefulTinyOnnxRunsTwoRolesWithPersistentState --log_level=all
```

Observed result: build succeeded; the focused test exited 0 and passed all
eight token checks. ONNX Runtime emitted only its unused-initializer warning.

## Boundary and remaining work

This evidence proves native ORT state persistence and the two-role activation
handoff only. It does not register I01-I03/I15 in the formal G2 manifest, since
those cases still require the real NDNSF request/ACK/Selection/Response stream,
the declared one-/two-/four-Provider role maps, and process-level evidence.
The Qwen3.6-27B exporter, complete full-attention plus recurrent/convolution
state binding, CUDA execution, SIF sealing, and TigerCluster gates remain open.
