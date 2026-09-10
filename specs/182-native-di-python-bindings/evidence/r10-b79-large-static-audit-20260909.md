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
| Cppcheck | Follow-up scanned 99 non-vendor production `.cpp` files, exit `0`; 321 diagnostics (317 style/info, two analyzer false positives, two warnings) |
| R10-B78 requester/Provider targets | Waf exit `0`, `-j2`, 14.00s elapsed, RSS 1.13 GB, swaps 0; Provider SHA recorded in R10-B78 evidence |
| R10-B78 finite serve and requester help | requester `--help` exit `0`; controller-assisted Provider `SERVE_READY` → `RUN_LIMIT_REACHED` → `PERMISSION_WAIT_CANCELLED`, Provider/Controller rc `0` |
| Frozen registration manifest through canonical runner | direct invocation is intentionally rejected because the owner must supply a separate transient runner manifest; prior owner-to-runner evidence uses `--runner-manifest` |

### 2026-09-10 follow-up audit

The broad source pass was repeated after the Provider lifetime change. CodeGraph still reports an
up-to-date index (`8,175` files, `276,300` nodes). The maintained-caller AST scan found 17 legacy
API calls in the maintained user examples (10 `distributed_inference`, 2 `request_streaming`, 3
`async_distributed_inference`, 1 `request`, 1 `generate`). Cppcheck 1.90 scanned 99 non-vendor
`.cpp` files and returned 321 diagnostics: 317 style/info items, two analyzer errors, and two
warnings. The analyzer errors are a known Cppcheck 1.90 false positive for the iterator-copy
return in `NativeEpochCoordinator::makeFinalPayload` and an `internalAstError` on the valid lambda
initializer in `DI_NativeRequester`; neither is a confirmed product defect. The two warnings are
the existing redundant-condition observations. `validate_design.py --json` remains structurally
valid but reports `runtime_tests=NOT_RUN` and `product_static_review=NOT_RUN`.

The follow-up also found that the requester `--help` text contradicted the documented path
contract. The text was corrected, the focused closure suite passed, and the fresh requester target
build plus `--help` probe both passed; this finding is closed for the CLI boundary. A controller-
assisted finite Provider probe also passed after the run-limit fence repair.

The Cppcheck result is useful as a tool-health signal, not as evidence that the whole request chain
is correct. The confirmed findings below came from contract and call-path review.

## Findings

| ID | Severity | Static finding and first boundary | Evidence |
| --- | --- | --- | --- |
| SA-01 | MEDIUM / CLOSED_FOCUSED | The requester contract and implementation define `--input/--output` relative to the current working directory, while the old `--help` text said all paths were relative to the config file. The corrected help output now states both path rules; the focused closure suite, fresh target build, and `--help` probe passed. | `specs/182-native-di-python-bindings/contracts/native-requester-configuration.md:14-18`; `examples/DI_NativeRequester.cpp:97-102,126-187,225-228`; `.codex-tmp/spec182-r10-b80-requester-help.log` |
| SA-02 | HIGH | Client accepts `maxGeneratedTokens` up to `1,048,576`, while the execution-plan JSON contract rejects values over `64`; stream options also cap `maxEvents` at `4096` and callback capacity at `1024`. The requester derives those fields from the token count, so valid client input can fail later at Selection or stream validation. | `NativeInferenceClient.cpp:1760-1767`; `NativeExecutionPlanJson.cpp:948-964`; `InvocationStream.cpp:251-265`; `DI_NativeRequester.cpp:208-214` |
| SA-03 | HIGH / CLOSED_FOCUSED | R10-B78's original finite serve loop joined an installation thread that could wait the full permission deadline. The repair publishes an atomic run-limit fence and the installer exits its permission wait on that fence; a controller-assisted finite serve probe reached `SERVE_READY`, emitted both run-limit/cancellation markers, and exited Provider rc=0. | `examples/DI_NativeProviderExecutable.cpp:1864-1882,1970-1999`; [R10-B78 evidence](r10-b78-provider-run-limit-20260909.md); `.codex-tmp/spec182-r10-b80-provider-run-limit-controller/` |
| SA-04 | RETRACTED / PROCESS GAP | The two schemas are intentional: the frozen file is registration-only and the canonical runner consumes a separate transient case manifest. The direct `--manifest case-manifest.json --case I01` probe was an invalid command, not a product failure. The remaining process gap is that no tracked generator/validator currently binds every registered ID to a fresh runner manifest; prior PO-001 owner evidence demonstrates the explicit handoff path. | `tests/standalone/run-spec182-native-closure.py:26,139-157`; `tests/fixtures/spec182/case-manifest.json:2,89+`; `Experiments/NDNSF_DI_NativeClosure_Minindn.py:26-81,176-180`; [R10-B71 evidence](r10-b71-po001-stream-owner-pass-20260909.md) |
| SA-05 | HIGH | Maintained examples still call legacy/default Python APIs: `distributed_inference` 10 sites, `request_streaming` 2, `async_distributed_inference` 3, `request` 1, and `generate` 1. Native helpers coexist as opt-in paths, so SC-006/P5 zero-legacy-use is open. | `examples/python/NDNSF-DistributedInference/*/user.py`; caller inventory from the follow-up AST audit |
| SA-06 | HIGH | The freshly built Provider contains `$ORIGIN/..` but its transitive `NEEDED` libraries still resolve through host paths (`/opt/onnxruntime`, `/home/tianxing/...`, `/usr/local/lib`, `/lib`). This is an unresolved staged/container deployment boundary, not a source-level protocol result. | `readelf -d .codex-tmp/spec182-r10-b78-provider/build/examples/di-native-provider`; matching `ldd` output |
| SA-07 | CLOSED_FOCUSED | `commitConversationTurn` previously fell back from an empty generation tokenizer digest to `model.semanticsDigest`. The repair requires the runtime contract and derived generation digests to agree, then persists only the operator-pinned runtime digest; the native-config Qwen conversation selector verifies the transcript. | `NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.cpp:833-872`; [R10-B80 evidence](r10-b80-native-config-qwen-conversation-20260910.md) |

## What static review did and did not catch

Static review found real contract and lifecycle blockers that syntax checks and component tests do not
exercise: SA-01, SA-02, SA-03, SA-05 and SA-06. It exposed the R10-B78 bounded-lifetime hole before
a misleading finite-process qualification run, and the follow-up exposed a contradictory CLI help
contract; all three source repairs now pass focused behavior checks. SA-04 remains a process gap rather than a product defect: registration and transient runner
manifests are intentionally separate, but the handoff is not yet generated and checked by one tracked
tool. Cppcheck found no confirmed critical defect in the production DI library; its false positives
and low-severity warnings show why tool output cannot replace a contract-aware review.

The following remain runtime or deployment observations rather than static conclusions: an
independent requester→Core→Provider transport result, Provider worker/cross-process behavior,
two-round continuation/recovery, maintained no-Python execution, I02–I08 and PO-002–PO-014,
T016/T017 qualification, and a sealed container dependency closure.

## Closure decision and next executable slice

Local C++ components and the native-config in-process Provider stream are real and tested, but the
whole Spec182 production chain is still open. No percentage is derived from task-card counts. The
next bounded sequence is:

1. make the registration→transient-runner handoff explicit and generate one fresh independent case;
2. run one independent requester→Core→Provider case and preserve its raw evidence;
3. resolve SA-02 budget bounds and stage the complete deployment dependency closure;
4. only then expand to continuation/recovery, maintained caller migration and legacy retirement,
   I02–I08, and deployment/T016/T017 closure.
