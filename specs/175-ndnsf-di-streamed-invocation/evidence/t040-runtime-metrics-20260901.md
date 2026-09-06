# T040 Runtime Metrics Seam — 2026-09-01

> **Superseded task status:** this earlier metrics-only checkpoint is retained
> for history. T040 is closed by the production opaque-handle evidence in
> [`t040-opaque-state-handle-20260901.md`](t040-opaque-state-handle-20260901.md).
> Physical CUDA measurement remains a later T025/T026 qualification concern;
> it is not a reason to reopen T040.

## Status

Focused implementation checkpoint only. T040 is closed by the newer opaque-
handle evidence; this record remains non-qualifying metrics history and the
overall design-to-code convergence verdict remains `BLOCK`.

## Production-path change

The adapter-owned `NativeRuntimeMetrics` record is now returned by the native
runner, captured by `ProviderRoleWorker`, and attached to each
`NativeEpochCoordinatorResult` observation. The ONNX adapter records metadata
only: state H2D/D2H bytes, ordinary activation input/output and host/device
bytes, bounded token/position/opaque-handle control bytes, state input
hit/miss counts, non-warmup prefill recomputes, and state-map releases. No
tensor contents, device pointers, prompts, or generated text are logged.

The CUDA path counts direct device-state rebinding as a state hit, initial
host state as H2D, terminal state export as D2H, and streamed opaque handles as
control bytes. The CPU path remains explicit and reports zero device-transfer
bytes; this is not CUDA evidence.

## Focused validation

```text
./waf build --targets=unit-tests -j1
Waf: build finished successfully (103/103)

./build/unit-tests --run_test=DiOnnxRuntimeGpuEvidence/StatefulTinyOnnxRunsTwoRolesWithPersistentState --log_level=test_suite
*** No errors detected
```

This is one focused CPU adapter regression, not a complete test suite. No
MiniNDN qualification, SIF replay, or Tiger job was run or accepted. T040
still requires a real multi-Provider CUDA process that
proves device-resident decode state, zero complete-state host round trips
between tokens, and bounded cleanup, plus the corresponding production-path
regression and fresh convergence audit `PASS`.
