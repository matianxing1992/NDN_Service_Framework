# T013 CPU cache-effectiveness checkpoint - 2026-08-25

**Status:** `PASS` for the registered CPU semantic gate; CUDA residency,
real-Qwen qualification, and timing remain open.

## Subject

- Production path: `NativeEpochCoordinator -> NativeProviderRuntime ->
  OnnxRuntimeModelRunner`.
- Artifact: the checked-in one-role Spec175 tiny stateful ONNX model.
- Prompt/oracle: token `3`, greedy output `4,5,6,7,8,9,10,2`.
- Cached branch: one prefill followed by exact Provider-owned predecessor lookup
  and seven incremental state transitions.
- Reference branch: the same persistent ORT runner and artifact, but no
  predecessor state; every epoch receives the complete logical prefix and the
  certified zero initial state.

## Machine-derived result

| Observation | Cached branch | Full-prefix reference |
|---|---:|---:|
| Output tokens | `4,5,6,7,8,9,10,2` | `4,5,6,7,8,9,10,2` |
| Actual input extents | `1,1,1,1,1,1,1,1` | `1,2,3,4,5,6,7,8` |
| Prefix represented by state | `1,2,3,4,5,6,7,8` | not cached |
| Per-epoch prefix work avoided | `0,1,2,3,4,5,6,7` | `0` |
| Total prefix work avoided | `28` tokens | `0` |

The cached observations are emitted by `NativeEpochCoordinator` after a
successful production runner call and before state publication/commit. The
actual new-input extent is derived from the encoded model input, not copied
from a hit counter or expected test value.

## RED and correction

The first reference implementation returned correct output only for its first
epoch and then produced zeros. Instrumentation showed impossible logits shapes,
which localized the defect to the test oracle: it bound a `NamedTensor`
reference to an element of the temporary vector returned by
`decodeTensorBundle(...)`. The vector was destroyed before the reference was
read. The oracle now keeps the decoded vector alive through logits validation
and sampling. No production runner, state identity, or cache transition was
changed to obtain the passing result.

## Verification

```text
./waf build --targets=integration-tests -j2
  PASS

./build/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI01OneProvider \
  --log_level=message
  PASS: No errors detected
  cached=4,5,6,7,8,9,10,2
  fullPrefix=4,5,6,7,8,9,10,2
```

## Boundary

This evidence proves effective incremental reuse semantics for the registered
CPU fixture. It does not claim lower CPU latency, persistent CUDA I/O binding,
zero device-host-device state transfers, or Qwen3.6-27B behavior. Those remain
T013/T025 qualification work.
