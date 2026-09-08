# R5-B4 Native Runtime Config Fixture

**Date**: 2026-09-08
**Status**: FOCUSED_BEHAVIOR_PASS / PARTIAL
**Source checkpoint**: working tree after `20b65c70` (not yet checkpointed)
**Owner**: current executor
**Scope**: operator-pinned native runtime configuration for maintained caller migration

## Boundary

R5-B4 fixes the configuration boundary required by `APPClient.request_native()`:
catalog/source identity, native preparation, authenticated grant owner, offer admission,
runtime contract/security/budget/state mapping, and the C++ split/placement selector. It does
not migrate a maintained caller, retire the Python provider, or claim network qualification.

The fixture must be source checked and deterministic. Runtime JSON may carry public policy
metadata and references to operator-owned key files, but Python must not construct a plan,
verify an offer, or provide a strategy callback. The request selector must be a bound C++
`NativeModelSplitStrategy` plus C++ `NativePlacementStrategy`.

## Initial findings

- `NativeRequestCatalog::load` already owns source/digest/splitter validation and exposes the
  native splitter and preparation catalog.
- `NativeServiceUser::nativeGrantClientFromConfig` already owns private-key and content-key
  loading; requester identity is checked against the Core `ServiceUser` identity.
- `NativeInferenceClient` already executes the complete configured path and fails closed when
  runtime/preparation/admission/grants are absent.
- No maintained caller currently supplies one complete operator runtime configuration. The
  existing C++ integration scenarios are valid production-target tests but are not yet a
  reusable operator fixture.

## Planned fixture contract

The native runtime configuration schema is `ndnsf-di-native-request-runtime-v1`. Its required
fields are `contract`, `requester_identity`, `protection_epoch`, `input_layout_digest`,
`security`, `budget`, `state_mapping`, `no_progress_ms`, and `max_segments`. Catalog and grant
owners are supplied as native objects by the composition root; the parser must reject unknown
schema/version, incomplete contract/security/budget values, identity mismatch, plaintext epoch,
zero limits, and catalog/grant bindings that do not match the runtime.

## Implemented and reviewed

- `nativeRequestRuntimeFromJson` now performs native JSON size/schema/exact-key checks, adapter
  descriptor binding, requester-to-grant identity binding, protected-policy checks, candidate
  budget validation, catalog state-mapping equality and bounded progress/segment limits.
  Catalog source bytes and grant key material remain owned by their existing native loaders.
- The single pybind entry exports `native_request_runtime_from_json`; `ServiceUser` exposes only
  a thin `native_runtime_from_config` pass-through. No Python planner or strategy callback was
  added.
- `Spec182NativePlanning/NativeRequestRuntimeLoadsPinnedPolicyAndRejectsDrift` covers a valid
  YOLO catalog plus native grant owner and nine configuration mutations; the binding suite checks
  the exported symbol and facade shape.

## Coverage matrix

| Lane | Result | Evidence |
| --- | --- | --- |
| production entry/callers | covered for composition entry; maintained caller migration remains a gap | `nativeRequestRuntimeFromJson`, `ServiceUser::native_runtime_from_config`, `NativeServiceUser::nativeInferenceClientConfigured`; no YOLO/Qwen maintained caller changed |
| implementation/wire | covered | `NativeRequestPlanner.cpp/.hpp`, `NativeAuthenticatedGrantClient.hpp`, `di_bindings.cpp`; exact schema and owner binding are native |
| test/harness/oracle | covered for config boundary | C++ `Spec182NativePlanning/NativeRequestRuntimeLoadsPinnedPolicyAndRejectsDrift`; Python `test_spec182_native_bindings.py`; no real Provider request in this batch |
| build/source closure | covered after repair | shared `ndnsf-distributed-inference` target and extension rebuilt with system-first compiler; first extension import exposed stale shared library and was repaired before final checks |
| migration/evidence | gap | T013 caller migration, T016 qualification and cross-process/provider evidence remain open |

## Review and build record

The read-only `review-agent` pass found no actionable defect after checking the complete diff,
catalog/grant ownership, binding lifetime, exact callers and test registration. The first
extension build linked against an older shared native library because only `unit-tests` had been
rebuilt; import failed with an undefined `nativeRequestRuntimeFromJson` symbol. The shared target
was then rebuilt before the corrected extension build.

```text
./waf build --out=build-nac182 --targets=unit-tests -j4
result: PASS; candidate output .codex-tmp/spec182-r4-b2/build; initial full rebuild 2m54.491s;
final source/fixture rebuild 49.992s

./.codex-tmp/spec182-r4-b2/build/unit-tests \
  --run_test=Spec182NativePlanning/NativeRequestRuntimeLoadsPinnedPolicyAndRejectsDrift
result: PASS; 1 case

./.codex-tmp/spec182-r4-b2/build/unit-tests --run_test=Spec182NativePlanning
result: PASS; 30 cases

./waf build --out=build-nac182 --targets=ndnsf-distributed-inference -j4
result: PASS; 20.746s

python3 setup.py build_ext --inplace --force
result: PASS after shared-target repair; one extension target; approximately 206s wall on the
first corrected build and approximately 160s on the final source-closure relink

PYTHONPATH=pythonWrapper:NDNSF-DistributedInference python3 -m pytest -q \
  tests/python/test_spec182_native_bindings.py tests/python/test_spec182_legacy_exclusion.py \
  tests/python/test_spec182_native_closure.py tests/python/test_ndnsf_python_service_response_binding.py \
  tests/python/test_ndnsf_di_app_sdk_compatibility.py
result: 39 passed in 1.12s
```

The final native toolchain was `/usr/bin/g++` 9.4.0 and GNU ld 2.34. The rebuilt shared
library exports `ndnsf::di::nativeRequestRuntimeFromJson`; this symbol check closed the
source/link boundary before the final Python import tests.

## Verification record

| Gate | Result | Evidence |
| --- | --- | --- |
| Static design/source review | IN_PROGRESS | runtime schema and binding owner are being added before caller edits |
| C++ build/test | NOT_RUN | no source implementation has been built in this batch |
| Maintained caller behavior | NOT_RUN | follows R5-B5/R5-B6 |
| Qualification | NOT_RUN | follows T016 |

## Remaining

1. Use this runtime parser to migrate a maintained YOLO requester in R5-B5.
2. Migrate Qwen/streaming callers and retire the Python provider path in R5-B6/R6-B1.
3. Run T016 qualification only after real Provider parity, rollback and cross-process evidence.
