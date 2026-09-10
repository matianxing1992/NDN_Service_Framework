# Spec182 R10-B79 Large Static Audit

**Date:** 2026-09-09
**Branch:** `Experimental`
**Decision:** `OPEN_FOR_NEXT_BATCH`
**Scope:** read-only audit of the current implementation, maintained callers, qualification harness,
build registration, and deployment closure. This record does not claim runtime qualification.

## Method and validation

The audit traced `NativeInferenceClient`, `DI_NativeRequester`, `NativeInferenceProvider`,
`ServiceProvider::serve`, `APPClient` native and legacy entry points, and the Spec182 runner with
CodeGraph, then confirmed the relevant source with line-numbered reads. It also ran:

| Check | Result |
| --- | --- |
| Spec Kit structure audit (`--strict`) | `PASS`; 19 FR, 11 SC, 17 parent tasks; 3 parent tasks complete; execution cards 16 `DONE`, 23 `PARTIAL`, 1 `NOT_STARTED` |
| Design validator and Spec Kit sync | `PASS`; validator still reports `runtime_tests=NOT_RUN`, `product_static_review=NOT_RUN` |
| Maintained Python AST scan | 730 files, 0 syntax errors |
| Native closure regression | `41 passed` |
| Cppcheck | 61 production files, exit `0`; three non-blocking diagnostics (one lifetime false positive, one redundant condition, one pass-by-value performance warning) |
| R10-B78 Provider target | 90/90 link steps, exit `0`, `-j2`, 13:42.07, RSS 2.86 GB, swaps 0 |
| Frozen I01 manifest through canonical runner | `UNQUALIFIED`: runner reports `manifest schema mismatch` before process start |

The Cppcheck result is useful as a tool-health signal, not as evidence that the whole request chain
is correct. The confirmed findings below came from contract and call-path review.

## Findings

| ID | Severity | Static finding and first boundary | Evidence |
| --- | --- | --- | --- |
| SA-01 | MEDIUM | Standalone requester advertises config-relative paths, resolves catalog/key/options under `base`, but reads `--input` and writes `--output` from process CWD. This makes the public CLI contract inconsistent and can select different files under a changed working directory. | `examples/DI_NativeRequester.cpp:97-112,126-187,225-228`; Python config helper confines relative paths in `ndnsf_distributed_inference/app_sdk/client.py:358-373` |
| SA-02 | HIGH | Client accepts `maxGeneratedTokens` up to `1,048,576`, while the execution-plan JSON contract rejects values over `64`; stream options also cap `maxEvents` at `4096` and callback capacity at `1024`. The requester derives those fields from the token count, so valid client input can fail later at Selection or stream validation. | `NativeInferenceClient.cpp:1760-1767`; `NativeExecutionPlanJson.cpp:948-964`; `InvocationStream.cpp:251-265`; `DI_NativeRequester.cpp:208-214` |
| SA-03 | HIGH | R10-B78's finite serve loop reaches the run-limit marker but joins an installation thread that may wait the full permission deadline. A `--run-for-ms N` process is therefore not deterministically bounded when permission is absent. | `examples/DI_NativeProviderExecutable.cpp:1864-1874,1962-1997`; detailed boundary in [R10-B78 evidence](r10-b78-provider-run-limit-20260909.md) |
| SA-04 | HIGH | The canonical runner expects schema `spec182-native-case-manifest-v1` with root `cases`; the frozen fixture and MiniNDN owner register schema `spec182-case-manifest-v1` under `qualification.cases`. Running I01 returns `UNQUALIFIED` with `manifest schema mismatch` before any protocol process starts. | `tests/standalone/run-spec182-native-closure.py:26,139-157`; `tests/fixtures/spec182/case-manifest.json:2,89+`; `Experiments/NDNSF_DI_NativeClosure_Minindn.py:26-81,176-180` |
| SA-05 | HIGH | Maintained examples still call legacy/default Python APIs: `distributed_inference` 10 sites, `request_streaming` 2, `async_distributed_inference` 3, `request` 1, and `generate` 2. Native helpers coexist as opt-in paths, so SC-006/P5 zero-legacy-use is open. | `examples/python/NDNSF-DistributedInference/*/user.py`; caller inventory from the AST audit |
| SA-06 | HIGH | The freshly built Provider contains `$ORIGIN/..` but its transitive `NEEDED` libraries still resolve through host paths (`/opt/onnxruntime`, `/home/tianxing/...`, `/usr/local/lib`, `/lib`). This is an unresolved staged/container deployment boundary, not a source-level protocol result. | `readelf -d .codex-tmp/spec182-r10-b78-provider/build/examples/di-native-provider`; matching `ldd` output |
| SA-07 | REVIEW | `commitConversationTurn` falls back from an empty operator tokenizer digest to `model.semanticsDigest`. The two authorities are distinct in the R10-B73 contract; direct conversation/legacy entry points need an explicit fail-closed test or a proof that the fallback is unreachable. | `NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.cpp:853-858` |

## What static review did and did not catch

Static review found real contract and harness blockers that syntax checks and component tests do not
exercise: SA-01 through SA-06. It also exposed the R10-B78 bounded-lifetime hole before a misleading
finite-process qualification run. Cppcheck found no confirmed critical defect in the production DI
library, but its false positive and low-severity warnings show why tool output cannot replace a
contract-aware review.

The following remain runtime or deployment observations rather than static conclusions: an
independent requester→Core→Provider transport result, Provider worker/cross-process behavior,
two-round continuation/recovery, maintained no-Python execution, I02–I08 and PO-002–PO-014,
T016/T017 qualification, and a sealed container dependency closure.

## Closure decision and next executable slice

Local C++ components and the native-config in-process Provider stream are real and tested, but the
whole Spec182 production chain is still open. No percentage is derived from task-card counts. The
next bounded sequence is:

1. repair SA-04 so one manifest has one canonical schema and the runner can launch I01;
2. make SA-03 cancellation-aware and prove a finite Provider exit without permission;
3. run one independent requester→Core→Provider case and preserve its raw evidence;
4. only then expand to continuation/recovery, maintained caller migration and legacy retirement,
   I02–I08, and deployment/T016/T017 closure.
