# Spec182 R11-B2 Native Unary Process

## Result

**Batch**: R11-B2 Native Unary Process  
**Date**: 2026-09-10  
**Source checkpoint before this batch**: `a4af317ef978c4bc7255d43528eaa9306487a889`  
**Status**: `CLOSED_FOR_VALIDATION` (N2 local process exit only)

本批按 `native-first-execution.md` 将真实 protected unary 接到独立 C++
`DI_NativeRequester`、Core、`di-native-provider` 和独立 artifact authority。Python
driver 只创建私有 NFD、PIB/TPM、fixture 文件并收集进程结果；请求、grant 验证、Provider
执行、Response 解码和数值 oracle 都由 C++ production executable 完成。

## Positive process result

The retained raw run is `/tmp/spec182-r11-b2-probe-4io2_va8/`. The driver records the
following independent process IDs in `*.pid` files:

| Process | PID | Role / identity |
| --- | ---: | --- |
| NFD | 2215656 | private Unix face for this run |
| Controller | 2215661 | `/example/hello/controller` |
| Artifact authority | 2215665 | `/example/hello/authority` |
| Provider | 2215676 | `/example/hello/provider` |
| Requester | 2215703 | `/example/hello/user` |

The request completed with one bound identity across the logs:

```text
requestId=/NDNSF/DI/REQUEST/15cb4c52e8fe435592879d4aae68fd1f-1
attemptId=attempt-1
planDigest=sha256:5a6bdc0145ffd3f1db93b3d6c8d435fc0b550d0215067a7fb0117d5db3bd6628
```

The requester log shows a signed ACK from `/example/hello/provider`, one provider selection,
the final signed Response from the same provider, and:

```text
NATIVE_NUMERICAL_ORACLE_PASS tensor=predictions values=4,0,12
NATIVE_REQUEST_SUCCEEDED request=/NDNSF/DI/REQUEST/15cb4c52e8fe435592879d4aae68fd1f-1 plan=sha256:5a6bdc0145ffd3f1db93b3d6c8d435fc0b550d0215067a7fb0117d5db3bd6628
```

The Provider log shows the real selection callback, grant verification before assembly, and
ONNX Runtime execution evidence:

```text
Received Service Selection Message: .../NDNSF/SELECTION/.../NDNSF/DI/REQUEST/15cb4c52e8fe435592879d4aae68fd1f-1
NDNSF_DI_GRANT_VERIFICATION status=VERIFIED boundary=BEFORE_ASSEMBLY attemptId=attempt-1
NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED runnerKind=onnxruntime-cpu realCompute=true device=cpu0
loadCompleted=true warmupCompleted=true processId=2215676
```

The C++ output bundle is 71 bytes, SHA-256
`0991096bb533317fa9eb2e9d73767b4400a9cfcacb8f8c855a6868c43e8ee828`, and decodes to the
expected Float32 tensor `predictions=[4.0, 0.0, 12.0]` for the frozen input `[1.0, -2.0, 3.0]`.
The provider's final Response wire digest is
`sha256:69cf3046cefc601604b0b4934d796a89bb8d8e09ed850caac99c6893690213b8`.

The signed material is role-separated. The run records public-key hashes for the authority
`3d6264ac0705e8ec21f3085eb606bbb6eb1bd55dd7695fd049d610fce76e9742`, Provider offer
`9475d62a72e846a11ea087ebb3ec0a4c984c7d28e6a47f85269fd30d9ec33049`, and Provider NDN key
`5877b3ffd89be30d2377cbe7cb0053a81797100c97d6505c1e9fd4d339966b22`. The requester has no
authority signing key or content key in its store.

## Negative process result

The retained role-authorization counterexample is `/tmp/spec182-r11-b2-probe-kzmdusr4/`.
It reaches the same Provider selection callback, but the Provider policy omits the
`FullModel` collaboration role. The Provider emits:

```text
NDNSF_SELECTION_STATUS state=5 ... requestId=/NDNSF/DI/REQUEST/cf380ad311555ff7c659410011516146-1 message=Provider lacks controller-authorized collaboration role FullModel
```

The requester emits `NATIVE_PROVIDER_FAILED boundary=response message=selected Provider returned
a failure`; that run has no `NATIVE_REQUEST_SUCCEEDED` or numerical-oracle pass marker. This is a
fail-closed authorization refusal rather than a pseudo-success or local fallback.

## Build and focused C++ validation

All native builds used the system-first toolchain and `-j2` because the host showed sustained
swap pressure during this batch. The build directory was
`.codex-tmp/spec182-r11-b2-fresh-20260910/build`.

```text
./waf -o .codex-tmp/spec182-r11-b2-fresh-20260910/build build \
  --targets=DI_NativeRequester,di-native-provider,DI_NativeArtifactAuthority -j2
-> PASS; 191/191 tasks, 20m4.399s

./waf -o .codex-tmp/spec182-r11-b2-fresh-20260910/build build \
  --targets=App_ServiceController,di-native-assembly-worker -j2
-> PASS; worker target uses NativePlanning.cpp and section garbage collection

./waf -o .codex-tmp/spec182-r11-b2-fresh-20260910/build build \
  --targets=unit-tests,integration-tests -j2
-> PASS; 309/309 tasks, 16m8.544s

unit-tests --run_test='Spec182*' --report_level=short
-> PASS; 256 cases, 7077 assertions

integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec182*' --report_level=short
-> PASS; 9 cases, 55 assertions

python3 -m py_compile tests/standalone/run-spec182-native-unary-process.py
-> PASS

python3 tests/standalone/run-spec182-native-unary-process.py \
  --build .codex-tmp/spec182-r11-b2-fresh-20260910/build
-> PASS; requester rc=0, C++ numerical oracle and final response markers present
```

The executable hashes used by the retained positive run are:

```text
DI_NativeRequester         af88ba25660b2c8749aaa0342272c1df8d18adf0cb8502f84afb02487abf9294
di-native-provider          38d0c2bd6d4fc28b399e367a890ebe48802f9993e65e5978ce64669e981c44f5
DI_NativeArtifactAuthority  da6e4dcf965f03f2380f5bdf421d5b94ab3db366cea80973e9877da049349579
DI_NativeOnnxAssemblyWorker 9aa888bf23d0bc5b0b951e5ad923897f02d59d1e8dadb200e059d5646a44285c
App_ServiceController       22026b146cda6afa4f117cde348dd9c81641e901d1de38395ab9f0857a1f965d
```

## Static review and scope

The read-only `review-agent` pass covered the changed C++ call paths, service subscription
registration, Provider candidate binding, ControllerVersion preparation, timeout/cancel ownership,
Waf source registration, and the C++ oracle. No actionable P1/P2/P3 finding remained. `git
diff --check` and Python syntax checks passed. Compiler warnings were existing third-party or
aggregate-initializer warnings; they did not produce an error.

This closes only the R11-B2/N2 local unary process exit. T010 remains `PARTIAL`; R11-B3 stream,
R11-B4--B7 stateful process exits, maintained caller migration, no-Python/closure gates and T016/
T017 qualification remain open. The fixture is deliberately deterministic and does not replace
the later YOLO/Qwen and full qualification coverage.
