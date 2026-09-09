# R10-B13 Integration Recipe Oracle Repair Evidence

**Date**: 2026-09-09
**Batch**: R10-B13
**Baseline**: `daa1470f` (R10-B12 audit checkpoint)
**Scope**: integration test recipe oracle alignment; no production code or wire behavior change.

## First failure boundary

The first full integration run is retained at `.codex-tmp/spec182-t016-r3/` and exited `201`
after four `Spec175NativeAssembly` cases threw `DI_NATIVE_ONNX_RECIPE` before ORT execution.
The production `canonicalNativeOnnxRecipeJson` serializes `inputNames` and `outputNames` in
contract order, but `tests/integration-tests/ndnsf-di-native-assembly.t.cpp::recipeDigestFor`
sorted those vectors. The helper therefore issued a digest for bytes that production correctly
rejects. The full unit suite in the same run passed; this was a test-oracle mismatch, not a
protocol or runtime qualification result.

## Correction and review

`recipeDigestFor` now preserves `role.expectedInputs` and `role.expectedOutputs` order and has an
inline comment tying the helper to the production serializer. All helper call sites continue to
use the same fixture fields; no fallback or digest relaxation was introduced.

The read-only review used `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`. It checked production entry
and all helper call sites, canonical field ordering, worker digest validation, selector
registration and unchanged fixture inputs; no actionable issue remained.

## Validation and status

The first retry must use the system-first `-j2` build because the full run showed sustained
swap-in on this 6-core/12-GB host. The affected selectors are:

```text
Spec175NativeAssembly/AssignmentBoundRootSourceAndCachePath
Spec175NativeAssembly/RegisteredOneProviderAssemblyLoadsOrt
Spec175NativeAssembly/RegisteredTwoProviderAssemblyLoadsOrt
Spec175NativeAssembly/RegisteredFourProviderAssemblyLoadsOrt
```

The corrected retry used the lower-concurrency native build required after the first full run:

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin WAFLOCK=.lock-waf \
  /usr/bin/python3 ./waf -o build-nac182 build --targets=integration-tests -j2
  exit 0; Waf 18.592s; 118/118

./build-nac182/integration-tests --run_test=Spec175NativeAssembly/* --log_level=test_suite
  exit 0; 7/7 cases; no errors detected
```

The four previously failing cases now pass, together with the three unaffected cases. `git
diff --check` and `validate_design.py` also pass. The complete integration suite must still be
rerun after this repair, and the later T016 unit → integration → MiniNDN/no-Python matrix remains
open.

## Closure decision

`CLOSED_FOR_VALIDATION` for this bounded test-oracle repair and the affected integration suite.
No parent task or qualification status is promoted.
