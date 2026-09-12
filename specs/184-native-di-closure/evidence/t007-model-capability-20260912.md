# Spec184 T007 Model Capability Boundary

**Date**: 2026-09-12  
**Status**: `PARTIAL` / environment boundary recorded; no qualification status is promoted

## Verified local boundary

The current development host reports six logical CPUs and 11 GiB RAM (`nproc`,
`free -h`). The user-confirmed model capability for this host is Qwen3-0.6B;
Qwen3.6-27B cannot be executed here. The Qwen process entrypoint intentionally
binds the exact `Qwen/Qwen3.6-27B` model family, revision, CUDA provider and
three-stage manifest, and rejects a different model or CPU fallback. Therefore
Qwen3-0.6B can provide a local C++ smoke/ABI fixture only; it cannot close the
Spec184 Qwen3.6-27B qualification row.

The repository contains a source-bound small Qwen ONNX fixture and native C++
selectors, but this is not evidence that the 27B model is present. The exact
27B manifest, tokenizer, CUDA runtime and staged model objects remain an
external experiment-owner input.

## Failed retry boundary preserved

To avoid an unbounded or misleading full rebuild, the current r4 candidate was
checked for a registered C++ Qwen selector. The candidate was configured with
`--with-examples` and has no `integration-tests` task generator. The bounded
command stopped before compilation:

```text
WAFDIR=.waf3-2.0.24-c88b74123ce8b9d1a27999f7cf96dff0 \
WAFLOCK=.lock-spec184-b5-r4 \
./waf -o build-spec184-b5-candidate-r4 build --targets=integration-tests -j4
-> Could not find a task generator for the name 'integration-tests'
-> exit=1
```

Raw output: `.codex-tmp/spec184-qwen-smoke-20260912/waf-build.log`  
SHA-256: `1f49b7cd938da12e8169c4248501b832b85b8fcdb66b2fc1352092dd413ece62`

This is a Waf configuration boundary, not a model, protocol, or C++ runtime
failure. The candidate was not reconfigured merely to force a test binary, and
no 27B run was attempted.

An additional affected-target build lookup used the output filename
(`DI_NativeOnnxAssemblyWorker`) instead of the registered Waf task name
(`di-native-assembly-worker`). Waf rejected that lookup before compiling; the
preserved raw output is `.codex-tmp/spec184-a4-target-build-20260912.log`.
This is a command-target naming error, not a product or model result. The next
retry uses the registered task name and the same candidate tree.

The first attempt to regenerate the receipt through
`scripts/spec180_native_build.py build` omitted the candidate Waf directory and
lock environment, so the helper stopped at `The project was not configured`.
Raw output is `.codex-tmp/spec184-a4-native-receipt-build-20260912.log`.
This is a build-environment invocation error; the next attempt supplies the
same `WAFDIR`/`WAFLOCK` pair used by the successful direct build.

With the Waf environment supplied, the native targets rebuilt, but the receipt
probe then failed while importing the binding because the host loader selected
an incompatible `/usr/local/lib/libnac-abe.so`; the expected
`Consumer::clearCache` symbol exists in the candidate NAC-ABE prefix. Raw
output is `.codex-tmp/spec184-a4-native-receipt-build-20260912-r2.log`.
This is a dynamic-library search-order boundary, not a C++ selector or model
result. The retry must put the candidate NAC-ABE and NDN-SVS directories first
in `LD_LIBRARY_PATH` and re-run the probe.

## Mixed-binary retry boundary

For a bounded smoke probe, the pre-existing `build-spec184-b5-candidate/integration-tests`
binary was launched with r4 libraries placed first in `LD_LIBRARY_PATH`. It
reported `SPEC182_NATIVE_DI_REQUEST_RESULT_OK` and then aborted with a SIGSEGV
at address `0x00000080` (exit `201`, 5 of 6 assertions observed). This is an
ABI/build-boundary failure caused by mixing a test executable from the older
candidate with r4 libraries; it is not evidence against the Qwen selector or
the native model path. The mixed combination is rejected and will not be
reused.

Raw output: `.codex-tmp/spec184-qwen-smoke-20260912/integration-qwen.log`
SHA-256: `33c155fb5124cd7249551586e6b5ee1b5658334503d05ac09d4ec8f4d0a85cde`

To obtain a valid local 0.6B smoke result, a future run must build the test
executable and native libraries from the same configured tree, then issue a
fresh receipt. That work is separate from the exact 27B A3 qualification.

## Disposition

| Gate | Current status | Reason |
| --- | --- | --- |
| `T007-A3 Qwen3.6-27B` | `WAITING_EXTERNAL_INPUT` | exact model/runtime/manifest must run on the experiment host |
| Local Qwen3-0.6B smoke | `AVAILABLE_AS_SMOKE_ONLY` | may exercise a named C++ selector or ABI boundary; never substitutes for A3 |
| `T007-A4 inherited negative/retirement` | `PARTIAL` | local rows can continue independently; I05 and retirement evidence remain open |
| `T008 native handoff` | `BLOCKED_BY_T007` | final handoff waits for T007 qualification and explicit external transfer |

## Next action

Continue only with A4 rows that have a current C++ selector and complete
candidate-bound evidence. When the exact 27B bundle is available on the
experiment host, transfer the current candidate receipt and run A3 there;
do not rename the 0.6B smoke result.

## Binding identity boundary and current local result (2026-09-12)

The first Y-A retry after the framework/DI refresh stopped before MiniNDN with
`LOCAL_NATIVE_BUILD_REJECTED:RUNTIME_IDENTITY_CHANGED`: the receipt still described the old
`_ndnsf.so`. This is recorded in `.codex-tmp/spec184-yolo-Y-A-run-20260912-r57.log` and is a
binding identity boundary, not a model result. The extension was rebuilt from the same candidate
NAC-ABE/NDN-SVS/DI libraries and the receipt was regenerated and verified; the rebuild log is
`.codex-tmp/spec184-python-binding-rebuild-20260912.log` (SHA-256
`bd74baa072ab553711bd2a5e03626ee18e704ee459594868ddc4e0f2675b5d02`).

The corrected current-candidate Y-A run `r58` passed with a C++ YOLO numerical oracle and is
recorded as T007-A1 evidence. It does not execute or qualify Qwen3.6-27B. The host boundary and
A3 disposition are unchanged: Qwen3-0.6B is smoke-only and A3 remains `WAITING_EXTERNAL_INPUT`.

## Same-tree C++ smoke and test-build repair (2026-09-12)

The local C++ test tree was rebuilt from the same configured Spec184 candidate after the
mixed-binary boundary above. The first same-tree attempt reached
`controller-revocation-flow.t.cpp` but exposed an incompatibility in the installed NAC-ABE
headers: the package's historical quoted `common.hpp` include was not covered by the exported
parent include path, and the current header intentionally keeps `KpAttributeAuthority::m_tokens`
private. The test had been reading that private dependency field directly.

The repair is bounded to the test/build boundary: `tests/wscript` adds an existing nested
`nac-abe` include directory when present, and the test's friend shim reconstructs the expected
policy through `ServiceController::effectiveAttributesFor` instead of reaching into NAC-ABE
private state. No production API or external dependency source was changed. The corrected
`integration-tests` build completed with Waf `-j4` in 46.811 seconds, exit `0`:

```text
/usr/bin/python3 ./waf -o build-spec184-b6-candidate-tests \
  build --targets=integration-tests -j4
```

Build log SHA-256: `019646eca1c73f5a25d428f9c92ea60fb8053ead5da4955a5b530fbfddb7d48d`.
The resulting binary SHA-256 is
`b35c698f4b88ebe12a752b96f4c7dd9b39af2d88553fd2ba6d3b91f3cde9a33b`.

The same-tree C++ selectors then passed with `*** No errors detected`:

* `Spec182R10B73NativeConfigQwenRealProviderStream` (3.884 s)
* `Spec182R10B80NativeConfigQwenRealProviderConversation` (7.262 s)
* `Spec184DurableOutcome` (7.191 s)
* `Spec184NativeCheckpoint/*` (3 cases), `Spec182NativeAssembly/*` (5 cases),
  `Spec182NativeInferenceClient/*` (2 cases), and `Spec182NativeRequestIdentity/*` (1 case)

These are native C++ transport/state/assembly/fixture checks. The two native-config Qwen
selectors use the repository's source-bound small Qwen ONNX fixture and CPU runtime contract;
they do not load the actual Qwen3-0.6B weights and do not qualify the required
`Qwen/Qwen3.6-27B` CUDA bundle. Raw selector logs are retained under
`.codex-tmp/spec184-b6-qwen-0.6b-smoke-20260912-r1.log`,
`.codex-tmp/spec184-b6-qwen-0.6b-smoke-20260912-r2.log`,
`.codex-tmp/spec184-b6-cpp-native-smoke-20260912.log`, and the four
`.codex-tmp/spec184-b6-cpp-unit-smoke-*.log` files. This strengthens local C++ smoke evidence
only; A3 remains `WAITING_EXTERNAL_INPUT`, and T007/A4 remains `PARTIAL`.

The same-tree test closure then built the subprocess dependencies that the integration and unit
harnesses discover at runtime. The registered Waf target `di-native-assembly-worker` built in
31.764 seconds (exit `0`), and the five `spec182-worker-tool-*` fault-injection tools built in
0.491 seconds (exit `0`). Their raw build logs are
`.codex-tmp/spec184-b6-assembly-worker-build-20260912.log` (SHA-256
`31a741173761c696df7727732c8794953b88f5bbf3cd434a4e791a909d43696c`) and
`.codex-tmp/spec184-b6-worker-tools-build-20260912.log` (SHA-256
`1f6ff47d7627f0e70167237d1a2357cc24cb8ada2e398daea0e3336fc10afa95`).

With `NDNSF_SPEC182_BIN_DIR=build-spec184-b6-candidate-tests`, the full same-tree
`integration-tests` run completed with exit `0` and `*** No errors detected`; its raw log is
`.codex-tmp/spec184-b6-integration-full-20260912-r2.log` (SHA-256
`130175e11c669f905936844610e163c62eaba0361621982ee3bb73a7b4e15d7c`). The full same-tree
`unit-tests` run completed with exit `0`, with 1034/1034 test cases and 71072/71072 assertions
passing; its raw log is `.codex-tmp/spec184-b6-unit-full-20260912-r3.log` (SHA-256
`95b99c2965b3a4c64e96b028238400f363de45153f731b868ca044152ab3c2d8`). The first full-run
failure without this environment and worker closure is retained as a harness-discovery boundary;
the corrected run exercises the same candidate tree and does not change the model qualification
boundary. These results close the local C++ build and test gate, but they remain fixture/CPU
evidence: this host cannot execute the required 27B bundle, and no exact Qwen3-0.6B-weight run was
performed. A3 therefore remains `WAITING_EXTERNAL_INPUT`, while T007 and the inherited A4 rows
remain `PARTIAL`.
