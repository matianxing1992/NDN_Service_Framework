# Spec182 R10-B83 Native Conversation Configuration Loader

Date: 2026-09-10
Branch: Experimental
Source baseline: `4c248c85`
Decision: CLOSED_FOR_VALIDATION
Boundary: shared native conversation configuration loader and whole-chain static audit

## Batch contract

| Lane | Covered boundary |
| --- | --- |
| Production entry/callers | `DI_NativeRequester` and the Python binding both reach one C++ `nativeConversationCoordinatorFromConfig` entry point before constructing `NativeInferenceClient`. |
| Implementation/wire | The loader validates the schema, requester identity, path policy, owner-only key files, key length, journal configuration, and injects the coordinator into the six-argument runtime client. |
| Test/harness/oracle | **Primary:** `Spec182ConversationConfig/SharedNativeConfigLoaderEnforcesOwnerAndPathIdentity`, full `Spec182*` C++ unit selection, requester help, and exported-symbol inspection. **Secondary:** four Python wrapper/contract suites. |
| Build/source closure | Shared DI library, unit-tests, and `DI_NativeRequester` rebuilt with system-first Waf `-j2`; Python extension rebuilt against the explicit candidate DI/Core/NAC-ABE/SVS paths. |
| Migration/evidence | This closes the standalone conversation-config mismatch only. Artifact-authority separation, independent worker/process transport, maintained caller migration, no-Python, and T016/T017 remain open. |

## Implementation and static review

The standalone requester previously accepted a documented `conversation` object but silently
discarded it. The parser is now owned by `NativeConversationCoordinator.cpp`, and both the
requester and binding delegate to that implementation. The requester identity is passed as an
expected value at both entry points, so a journal cannot be attached to another `ServiceUser`.
The Python binding no longer carries a second parser or reads conversation key bytes.

The changed C++ translation units passed Cppcheck 1.90 with no actionable diagnostic. A broader
scan covered the maintained C++ source set and left only an existing final-class ONNX warmup
warning, an inconclusive `ProviderRoleWorker::failPromise` null warning whose call sites all
receive the factory-created promise, and low-value style/performance messages. Python AST
parsing covered 278 files with zero syntax errors. CodeGraph and source review still confirm:

- the requester owns a local artifact-authority private key and issuer, contrary to the
  independent authority boundary in the design;
- 16 maintained inference call sites still use compatibility or automatic-planner routes;
- no independent requester/Provider worker-process run has observed the complete unary,
  stream, continuation/recovery, cleanup, and terminal-result chain;
- current ELF RUNPATH still resolves host-bound NAC-ABE, SVS, ONNX Runtime, and NDNSF objects.

These findings are recorded as remaining production gates rather than hidden by the local
loader result. The C++ unit and integration paths are the acceptance authority; Python results
only show that the thin wrapper still reaches the shared native boundary and has no stale source
contract assertion.

## Validation

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin ./waf -o build-nac182 build --targets=unit-tests,DI_NativeRequester -j2
-> PASS, 285/285 tasks, elapsed 1m48.648s

.codex-tmp/spec182-r4-b2/build/unit-tests --run_test='Spec182*' --log_level=test_suite
-> PASS, 252 test cases, no errors

env PATH=/usr/bin:/bin:/usr/sbin:/sbin WAFLOCK=.lock-waf \
  ./waf -o .codex-tmp/spec182-r4-b2/build build --targets=integration-tests -j2
-> PASS, 118/118 tasks, Waf elapsed 49.459s

LD_LIBRARY_PATH=.codex-tmp/spec182-r4-b2/build:/home/tianxing/NDN/nac-abe-integration-182/install/lib:/home/tianxing/NDN/ndn-svs/build \
  .codex-tmp/spec182-r4-b2/build/integration-tests \
  --run_test='Spec170NdnsfDiCoreFlow/Spec182*' --log_level=test_suite
-> PASS, 9 C++ integration cases, no errors, elapsed 49.62s

PYTHONPATH=pythonWrapper python3 -m pytest -q \
  tests/python/test_spec182_native_bindings.py \
  tests/python/test_spec182_legacy_exclusion.py \
  tests/python/test_spec182_native_closure.py \
  tests/python/test_ndnsf_python_service_response_binding.py
-> PASS, 72 wrapper/contract tests (secondary; does not qualify native behavior)

.codex-tmp/spec182-r4-b2/build/examples/DI_NativeRequester --help
-> PASS

nm -D --defined-only libndnsf-distributed-inference.so | c++filt
-> shared nativeConversationCoordinatorFromConfig symbol exported

validate_design.py --json; audit_speckit_structure.py --strict;
verify-spec-kit-sync.py --require-entrypoints; git diff --check
-> PASS
```

The first Python extension rebuild exposed one compile-time type mismatch at `_ndnsf.cpp:4347`
(`ndn::Name` passed where the shared loader expects a string). The call was corrected to
`m_userIdentity.toUri()`; the retry completed successfully. The retry used the recorded
`.codex-tmp/spec182-r10-b83-conversation-loader/attempt-2.log`; the host showed swap activity
and the single translation unit reached roughly 6.16 GB RSS, so subsequent native builds stay
at `-j2` under the repository resource policy.

## Closure decision

`CLOSED_FOR_VALIDATION` applies only to the shared C++ conversation configuration and entry-point
composition boundary. No parent task is advanced. T010/T011/T013/T014/T015/T016/T017 remain
PARTIAL or UNQUALIFIED until the authority boundary, independent process transport, two-round
recovery, maintained no-Python callers, dependency closure, and qualification evidence exist.
