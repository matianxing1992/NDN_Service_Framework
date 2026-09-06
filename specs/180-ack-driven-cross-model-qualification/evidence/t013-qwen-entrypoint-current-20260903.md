# T013 QWEN-F entrypoint implementation evidence

Date: 2026-09-03
Status: `IMPLEMENTED` boundary only; T013 is not `EXECUTED` or `QUALIFIED`.

## What changed

`Experiments/NDNSF_DI_QwenAckDriven_Minindn.py --case QWEN-F` is now a
separate production entrypoint for the external Qwen3.6-27B ONNX path. Before
delegating to the maintained pipeline runner it checks the registered model
manifest, model identity and revision, CUDA ONNX Runtime with
`cpuFallback=false`, exactly three stages, graph/initializer/stage digests,
tokenizer files, prompt binding, and an output-root-contained runtime stage
manifest. File digests are computed incrementally; model objects are not
loaded into host memory by the boundary checker.

The entrypoint deliberately does not reuse the Spec175 tiny-model wrapper and
does not claim that a small fixture is a Qwen3.6-27B result. Missing or
mutated external inputs return `WAITING_EXTERNAL_INPUT` before NFD/SVS or any
Provider starts.

## Focused verification

```text
python3 -m py_compile Experiments/NDNSF_DI_QwenAckDriven_Minindn.py
pytest -q tests/python/test_spec180_qwen_entrypoint.py \
  tests/python/test_spec180_dispatcher.py
20 passed in 0.15s
```

The tests cover missing manifest/output inputs, wrong backend and CPU-fallback
mutations, model identity mutations, digest mutations, tokenizer/prompt
binding, valid fixed delegate arguments, and the exact QWEN-F case vector.

## Still required before T013/T014

An owner-supplied, signed Qwen3.6-27B ONNX manifest and complete staged
object set must be sealed into the same candidate as YOLO. The in-image path
must then produce the required two-request terminal evidence with ACK,
Selection, Provider execution, CUDA runtime identity, output/token oracle,
child exits, and cleanup. Until those artifacts exist, T013 remains partial;
this file records only the implementation boundary and focused tests.
