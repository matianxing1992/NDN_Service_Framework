# Spec178 audit

## Gate results

- Model provenance and deterministic source/checkpoint/artifact hashes: pass.
- ONNX checker, native/ONNX parity, masked-slot neutrality, permutation equivalence, and CPU-only provider selection: pass.
- Real 2/4/6-view inference, explicit one-view paired baseline, duplicate-view de-duplication, wrong digest, missing artifact, and no-fallback checks: pass.
- C++ unit and integration multi-view gates: pass (targeted suites each completed without errors).
- MiniNDN real multi-process matrix: pass for nominal, provider selection, unavailable/late view, publication failure, missing model, and bad model digest.
- Paired evaluator: pass structurally; one registered synthetic target means uncertainty is `insufficient-samples`.

## Findings and claim boundary

No BLOCK finding remains for the requested execution path. The generated car images qualify the model export, CPU runtime, NDNSF delivery lifecycle, and evidence plumbing only. They do not support a scientific accuracy, flight-domain generalization, or multi-view benefit claim. The evaluator preserves negative/inconclusive outcomes and reports model-only cost separately from end-to-end materialization cost.

Model artifact: `sha256:14ec256fbc84e1c6d9d0cf593ca47ce151c641c5b3280535ca4dcbedd1a4c317`.

## Residual work

Collect and register enough independently grouped, licensed, calibrated vehicle-flight samples to make the pre-registered uncertainty analysis informative. This is the next research campaign, not a reason to relabel the current qualification result.
