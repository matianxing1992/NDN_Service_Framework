# Research: MVCNN ONNX CPU Execution for UAV Multi-View Recognition

**Research question**: How can Spec 177's functional multi-view contract be exercised by a real MVCNN-family model on a CPU host while preserving fair comparison, model provenance, and NDNSF data/security semantics?

**Research date**: 2026-08-29

## Decision 1: Qualify a Real MVCNN Checkpoint, Then Export It

The first subject will be a legally usable MVCNN-family checkpoint with a documented native evaluation path. It will be exported reproducibly to ONNX rather than treating a hand-written pooling fallback or an arbitrary YOLO artifact as MVCNN.

The original MVCNN project is MIT-licensed and documents multi-view shape recognition plus ModelNet/ShapeNet rendered-view datasets. Spec 178 uses a locally authored, MIT-licensed MVCNN-family checkpoint and exports it to a project-owned ONNX artifact. Checkpoint selection, license verification, native reproduction, ONNX export, and export parity remain one qualification gate; the resulting artifact and checkpoint digests are recorded in the registry. This does not imply UAV-domain accuracy.

**Alternatives considered**:

- Keep the current deterministic CPU adapter: rejected as the real-model subject; retained as a unit/integration test double.
- Use YOLO ONNX independently per view: rejected as MVCNN because it does not jointly pool the view set.
- Download an undocumented `.onnx` file: rejected because source, license, preprocessing, and native parity cannot be audited.

## Decision 2: Use One Six-Slot Graph With an Explicit View Mask

The preferred graph input is a bounded tensor representing up to six images plus a mask identifying valid slots. The model applies one shared view encoder, masks unused slots before symmetric pooling, and classifies the pooled representation once. One valid slot supports the paired single-view baseline; operational multi-view jobs require at least two.

This contract prevents separate artifacts for different view counts from changing learned parameters across treatments. It also makes padding auditable. If the selected checkpoint cannot be exported with correct mask semantics, it fails qualification rather than silently using zero-padded views as real evidence.

**Alternatives considered**:

- Dynamic view dimension: feasible but more sensitive to exporter/runtime shape behavior and harder to constrain in a first gate.
- Separate 1/2/4/6-view artifacts: rejected because artifact differences confound the view-count comparison.
- Always require six views: rejected because it prevents paired sensitivity analysis and reduces availability under partial UAV coverage.

## Decision 3: Expose Fused Logits and Pooled Features

The exported graph must return both class logits and the joint pooled representation. The worker records their digests, the view mask, and the exact reference-to-slot mapping. This proves that the registered graph ran on the admitted view set; it does not by itself prove an accuracy advantage.

Permutation of valid views should preserve logits within a registered tolerance for max-pooling MVCNN. Removing or replacing a non-duplicate view should change the pooled representation in at least one sensitivity fixture; otherwise the implementation must investigate whether the graph ignores views.

## Decision 4: Fix ONNX Runtime to CPU Only

The reference Python environment currently provides ONNX Runtime 1.19.2 with `AzureExecutionProvider` and `CPUExecutionProvider`. The real-model session will request only `CPUExecutionProvider`, disable provider fallback when supported, and verify the active provider list and profiling/runtime evidence. An accelerator must not be selected implicitly.

ONNX Runtime's official execution-provider documentation confirms that provider lists are priority ordered and may be set explicitly; a CPU-only subject should therefore name only `CPUExecutionProvider` and verify the resulting session.

## Decision 5: Separate Export Qualification From UAV-Domain Evaluation

The first artifact gate should reproduce the selected checkpoint on its native rendered-view dataset, such as ModelNet40, and compare native-framework versus ONNX outputs. That establishes export correctness. It does not establish performance on UAV imagery.

The generated six-view vehicle fixture then proves the NDNSF and annotation path but remains functional-only. Any vehicle/UAV recognition claim requires a registered synchronized vehicle-oriented dataset, fixed crops or target regions, ground truth, deterministic 1/2/4/6 subsets, and paired uncertainty analysis. A standard MVCNN checkpoint trained on rendered 3D shapes may require vehicle-domain fine-tuning before it is suitable for that gate.

## Decision 6: Use a Paired, Claim-Neutral Analysis

For every target, evaluate one-view and 2/4/6-view treatments with the same artifact and preprocessing. Report top-1 accuracy and macro-F1 where class support permits, completion, confidence, CPU inference latency, end-to-end latency, bytes, accepted views, peak RSS, and failure stage. Use paired accuracy deltas with confidence intervals; use McNemar's test where binary paired correctness counts are adequate.

The implementation succeeds when it produces correct, reproducible evidence. A positive multi-view result is not a release requirement. Benefit language is allowed only when the registered effect and uncertainty support it.

## Source Evidence

1. Su, H., Maji, S., Kalogerakis, E., & Learned-Miller, E. (2015). *Multi-view convolutional neural networks for 3D shape recognition*. ICCV. The paper establishes shared per-view processing and view pooling for one joint shape representation.
2. Original MVCNN project: `https://github.com/suhangpro/mvcnn`. The repository is MIT-licensed, documents ModelNet/ShapeNet view datasets, and links PyTorch implementations; it does not make a ready-to-use UAV ONNX model claim.
3. ONNX Runtime execution providers: `https://onnxruntime.ai/docs/execution-providers/`. The official documentation defines explicit provider ordering and CPU provider selection.
4. ONNX Runtime Python API: `https://onnxruntime.ai/docs/api/python/api_summary.html`. The API exposes session provider configuration, input/output metadata, CPU tensor execution, and runtime profiling controls.

## Residual Risks

- A native MVCNN checkpoint may classify rendered ModelNet objects but fail on UAV vehicle images because of domain shift.
- Some third-party checkpoint downloads may have unclear redistribution terms even when the source code is MIT-licensed; code and weights require separate provenance checks.
- Masked variable-view export can be numerically correct in the native framework yet diverge after ONNX optimization; parity and mask-ablation tests are mandatory.
- CPU inference may exceed the five-second deadline for six high-resolution views; the measured result must drive input resolution or model choice rather than being hidden.
- A positive point estimate on a small dataset is not a defensible advantage claim without paired uncertainty and transparent exclusions.
