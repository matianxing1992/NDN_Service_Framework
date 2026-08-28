# Qwen Runtime Route Audit (2026-08-19)

## Scope

This audit checks whether the current NDNSF-DI Qwen path has fully migrated
from PyTorch/Transformers to an ONNX Runtime-only deployed path. It is a
read-only architecture/evidence audit; no executable source, model, SIF, or
remote job was changed.

## Evidence

| Area | Current evidence | Verdict |
|---|---|---|
| Generic NDNSF-DI native backend | `NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.cpp` implements CPU and CUDA ONNX Runtime providers and rejects unavailable required providers. | ONNX backend exists. |
| Qwen Provider runtime dispatch | `examples/python/NDNSF-DistributedInference/llm_pipeline/provider.py` exposes both `qwen-transformers` and `qwen-onnx`; the former loads stage packages through the Transformers path and the latter loads ONNX sessions. | Two runtime paths remain active. |
| Qwen3.6-27B adapter | `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen/placement.py` declares `model_formats=("transformers-stage-package",)` and backends `("transformers", "cuda", "transformers-cpu")`. | The formal 27B adapter is not ONNX-only. |
| Qwen3.6 runtime lock | `packaging/ndnsf-di-container/oci/layered/locks/qwen36-overlay.lock.json` pins Transformers 5.14.1 and marks the model contract as `qwen3_5`; the lock is `CANDIDATE_UNMATERIALIZED`. | The 27B candidate still depends on Transformers. |
| ONNX artifact generation | `write_qwen_onnx_stage_artifacts()` imports `torch` and `transformers`, loads a full HF model, and exports stage graphs. | ONNX is currently a conversion output, not an independent Qwen3.6 runtime artifact pipeline. |
| ONNX generation campaign | `llm_pipeline/user.py` imports `transformers.AutoTokenizer` for generation campaigns even when the selected execution runtime is `qwen-onnx`. | Runtime still has a Transformers tokenizer dependency. |
| Existing Qwen ONNX evidence | Historical Qwen3-0.6B ONNX logs report `device=cpu cpuFallback=true` in the Provider readiness marker. | This is not valid no-fallback GPU evidence. |
| Qwen3.6 ONNX artifact | The repository has no current Qwen3.6-27B ONNX stage manifest/artifact bound to the active Spec170 candidate. | Missing. |
| Focused tests | `test_spec166_qwen_onnx_cuda.py` and `test_spec162_qwen36_stage.py` passed 28 tests in the current host run. These are contract/profile tests; they do not execute a complete Qwen3.6 ONNX request. | Useful unit evidence, not end-to-end proof. |

## Conclusion

The current system is **NDNSF-DI with a generic ONNX backend plus a separate
Qwen Transformers pipeline**. It has not completed the intended
`NDNSF-DI + Qwen3.6-27B + ONNX Runtime` migration.

The ONNX baseline in Spec170 is a real requirement for assembled ONNX
artifacts, but it does not by itself change the Qwen3.6 adapter's declared
format. Spec162's current model contract explicitly uses PyTorch,
Transformers, BF16, and three-stage Qwen3.6 packages. Therefore the current
27B lock and adapter cannot be used as evidence for an ONNX-only 27B path.

## Required closure before claiming ONNX-only Qwen3.6

1. Separate an offline exporter (which may use PyTorch/Transformers) from the
   deployed SIF runtime. The final runtime must not need PyTorch or
   Transformers model classes.
2. Export and content-address three Qwen3.6 text-stage ONNX artifacts with
   verified KV-cache, Qwen3.5 position/mask, dtype, and input/output contracts.
3. Change the Qwen3.6 adapter and manifests to an ONNX-stage format and
   `onnxruntime`/`onnxruntime-cuda` backend contract.
4. Replace the runtime tokenizer dependency with a separately locked tokenizer
   library or pre-tokenized, hash-bound input contract.
5. Build a new SIF whose deployed import census contains no PyTorch/Transformers
   model runtime, and require `cpuFallback=false` for CUDA execution.
6. Re-run exact token parity against a frozen reference, then execute the full
   NDNSF-DI lifecycle: `Request -> ACK_CLOSED -> Selection -> three ONNX
   Providers -> two dependency edges -> Response`.

Until these gates pass, protocol/negative/performance results from the generic
ONNX fixture or the Qwen Transformers path must not be presented as evidence
for the intended Qwen3.6 ONNX-only deployment.
