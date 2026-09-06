# T026 Provider lazy ONNX-load correction — 2026-08-29

The current source had a deployment option named `--lazy-qwen-load`, but the
`qwen-onnx` branch ignored it and always called
`_preload_qwen_onnx_sessions()` immediately after constructing `APPProvider`.
Tiger job `206918` showed all three Providers segfaulting after
`constructor_done` and before the ONNX stage-ready marker. Exact-SIF ORT
probes `206919` and `206920` independently passed one-stage and concurrent
three-stage session construction, narrowing the failure to this NDNSF
initialization/preload boundary.

## Source correction

The current worktree now:

1. honors `--lazy-qwen-load` by leaving the startup session cache empty;
2. advertises Selection preparation only when an eager cache, explicit local
   artifact, or late-bound repository registration exists;
3. creates and warms the selected ONNX session during Selection preparation;
4. records the stage-ready marker after that successful warmup; and
5. shares one runtime cache between Selection and the request handler so a
   lazy selection cannot create a second CUDA session.

The focused ONNX deployment tests pass (`8` tests), and the related Spec175
contract/bundle tests pass (`27` tests in the combined focused invocation).
The current exact SIF predates this source correction and is therefore no
longer eligible for a Tiger rerun. A new source seal, local SIF, G3 replay,
and exact-SIF replay are required before submitting T026 again.

## Boundary

This is a source/test checkpoint, not a T026 functional result. No model
request, prefill, decode, or distributed throughput claim is made from the
failed jobs or the isolated probes. The next functional attempt must use the
new candidate and retain all Provider startup markers and native exit codes.
