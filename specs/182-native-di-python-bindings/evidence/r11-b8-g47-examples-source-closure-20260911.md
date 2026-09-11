# R11-B8-G47 Native Examples Source Closure 2026-09-11

## Batch allocation

本批只收敛 Spec182 的 C++ examples 构建与本地加载闭包，覆盖
`DI_NativeOnnxAssemblyWorker`、`DI_NativeRequester`、`DI_NativeArtifactAuthority`、
Provider、plan/manifest smoke 及 ONNX Runtime smoke 共 10 个 target。稳定出口是
所有 target 可链接、`ldd -r` 无未解析符号，以及 C++ smoke/check-only marker；不把
Python wrapper、跨进程请求、MiniNDN 或 SIF 部署资格混入本批。

## First failure boundaries

第一次四 target 构建在 `-j4` 下因本机可用内存降至约 0.25 GiB 且持续 swap-in/out
被终止；这属于资源边界，不是产品失败。原始输出保留在
`.codex-tmp/spec182-t016-examples-20260911/build.log` 和
`vmstat-build.log`。按项目资源规则降至 `-j2` 后，277/277 tasks 在 31m50.347s
完成，但首次链接 `di-native-provider-session-smoke` 暴露 canonical ONNX helper、
`ExecutionLeaseService` 和 framework publication symbols 未进入该 target 的 source
closure；完整 linker 输出在 `build-j2.log`。

随后以 `-j3` 批量重建。第一次全 target 尝试在
`di-native-plan-schema-smoke` 的链接阶段暴露相同的 ONNX/framework closure，原始输出在
`build-all-targets-j3.log`。加入共享 `di_native_onnx_assembly_sources`、candidate
framework/ONNX/Protobuf link closure 和 `$ORIGIN/..` RUNPATH 后，第二次全 target 尝试
推进到 608/609，最后的 `di-native-onnxruntime-smoke` 仍暴露未加入的同一 closure；
原始输出在 `build-all-targets-j3-r2.log`。修正该 target 后，当前源码增量重链为
87/87、exit 0、7m30.258s，输出在 `build-onnxruntime-j3-r3.log`。

## Source/link closure repair

`examples/wscript` 现在对以下 target 显式加入 canonical ONNX assembler/recipe/worker
源、candidate `ndn-service-framework`、ONNX/Protobuf libraries、`-pthread` 和
`$ORIGIN/..` RUNPATH：

| Target | Added closure |
| --- | --- |
| `di-native-provider-session-smoke` | ONNX assembly sources, `ExecutionLeaseService.cpp`, ONNX Runtime/ONNX and framework link closure |
| `di-native-plan-schema-smoke` | ONNX assembly sources, ONNX/Protobuf and framework link closure |
| `di-native-onnxruntime-smoke` | message crypto plus ONNX assembly sources, ONNX/Protobuf and framework link closure |

这修复的是 target 注册与真实定义 translation unit 不一致的问题；它没有声称把 host
安装路径变成可移植部署路径。

## Verification

Fresh examples-enabled configure 使用显式 NAC-ABE、NDN-SVS、ONNX 和 ONNX Runtime
prefix，并以 system-first PATH 生成当前 build tree。累计的当前源码构建结果为：

| Check | Result |
| --- | --- |
| `tests/python/test_spec170_build_closure_preflight.py` | 4/4 PASS |
| Native target links | 10/10 linked across the fresh `-j2` build, `-j3` batch and bounded relink |
| `ldd -r` | 10/10 PASS with no `undefined symbol` or `not found`; logs under `ldd-local-first-j3-r3/` |
| Final Waf target replay | all 10 target names, `-j3`, exit 0; no-op replay finished in 54.356s |
| C++ plan/schema smoke | `NDNSF_DI_NATIVE_PLAN_SCHEMA_SMOKE_OK`, 4 roles/4 dependencies |
| C++ plan/manifest smoke | `NDNSF_DI_NATIVE_PLAN_MANIFEST_SMOKE_OK`, 4 roles/4 artifacts/8 output tensors |
| C++ plan/ONNX smoke | `NDNSF_DI_NATIVE_PLAN_ONNX_SMOKE_OK`, 4 roles/4 artifacts/4 dependency objects/440 output bytes |
| C++ Provider session smoke | `NDNSF_DI_NATIVE_PROVIDER_SMOKE_OK` |
| C++ ONNX Runtime smoke | `NDNSF_DI_NATIVE_ONNXRUNTIME_SMOKE_OK 2,3,4` using generated Add-one model |
| Provider `--check-only` | `NDNSF_DI_NATIVE_PROVIDER_CHECK_OK`, 4 roles/4 artifacts/4 registered/1 worker and ORT CPU evidence |
| Requester/Authority CLI | both `--help` exit 0; Provider no-argument usage exits 2 as expected |
| `git diff --check` | PASS |
| `validate_design.py --json` | PASS after correcting an existing wrapped `[T016-A ...]` progress link that the parser treated as a row |

The final target SHA-256 manifest is retained at
`.codex-tmp/spec182-t016-examples-20260911/native-target-sha256-r3.txt`; it binds the smoke
results to the current build tree rather than to an older installed executable.

The C++ binaries were run with the candidate build directory first in
`LD_LIBRARY_PATH`. Putting the stale `/usr/local/lib` framework first reproduced an
undefined-symbol loader failure before `--help`; this is retained in
`DI_NativeRequester-help.log` and `DI_NativeArtifactAuthority-help.log` and is a deployment
identity gate. The host build also
intentionally fails `verify-runtime-closure.py --reject-prefix /home/tianxing/NDN` with
`RUNTIME_HOST_BOUND_PATH`, so this evidence is not an exact-SIF or multi-machine closure
qualification. The package must be rebuilt inside its candidate container and rechecked with
the container paths.

## Review trace and closure decision

按官方 `/home/tianxing/.codex/skills/review-agent/SKILL.md`（SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）对 Waf diff、target
调用入口、source definition map、动态 loader identity 和测试判据做只读审查；没有新的
P1/P2/P3。首次 linker 漏项保留为 `compile/link` 漏检，修复后由 source-closure test、
fresh build、`ldd -r` 和 C++ markers 复核。

本批对 examples 的本地 C++ source/link/runtime smoke closure 为
`CLOSED_FOR_VALIDATION`。T004/T005/T006/T008/T009/T010/T011/T013/T014/T015/T016/T017、
maintained callers、no-Python、跨进程终端 Response、MiniNDN/Slurm 和 exact-SIF 仍保持
原状态；本批不升级任何父任务。
