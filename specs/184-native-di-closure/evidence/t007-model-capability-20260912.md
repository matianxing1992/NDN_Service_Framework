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
