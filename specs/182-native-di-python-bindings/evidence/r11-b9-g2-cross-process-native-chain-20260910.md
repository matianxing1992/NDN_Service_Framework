# R11-B9-G2 Cross-Process Native Main Chain

**Date**: 2026-09-10
**Status**: `CLOSED_FOR_VALIDATION` for this bounded local process-chain revalidation
**Parent**: R11-B9 / T010 / T011 / T014 / T016

## Scope

本批使用当前 `Experimental` checkpoint `fa169698` 的 fresh `build-nac182`，重新验证独立
C++ `DI_NativeRequester`、Controller/Authority、`di-native-provider` 之间的公开请求链。
Python 仅负责启动、停止进程和保存日志；请求构造、grant 签发与验证、Provider 数据恢复、
ONNX Runtime 执行、stream/conversation 状态及数值 oracle 均由 C++ 实现。测试模型是
Spec182 的 tiny fixture，不是 Qwen3-0.6B，也不是 MiniNDN 或真实 Slurm/SIF 资格。

## Validation

| Case | Command result | Native observation |
| --- | --- | --- |
| Unary | driver exit `0`; requester rc `0` | `NATIVE_NUMERICAL_ORACLE_PASS tensor=predictions values=4,0,12` and `NATIVE_REQUEST_SUCCEEDED`; Provider logged `NDNSF_DI_GRANT_VERIFICATION` before assembly and `runnerKind=onnxruntime-cpu`, `realCompute=true`, `cpuFallbackUsed=false` |
| Stream + conversation | driver exit `0`; first/second requester rc `0`; wrong-parent rc `1` | eight ordered token events and `NATIVE_STREAM_ORACLE_PASS`; second `APPEND_DELTA` succeeded; wrong parent failed with `DI_NATIVE_CONVERSATION_PARENT_MISMATCH` |
| Alternate-provider replacement | driver exit `0`; requester rc `0` | Provider B completed attempt 2 with native stream oracle and CPU ORT evidence; Provider A produced no accepted execution evidence |
| No-backup fail-closed | driver exit `0`; requester rc `1` | one terminal `NATIVE_REQUEST_STAGE_FAILED` at `ACK_CLOSED` with `DI_NATIVE_NO_ADMITTED_PROVIDER` |

## Commands and raw evidence

```text
python3 tests/standalone/run-spec182-native-unary-process.py --build build-nac182 --run-root .codex-tmp/spec182-r11-b9-unary-20260910-r1
python3 tests/standalone/run-spec182-native-stream-process.py --build build-nac182 --run-root .codex-tmp/spec182-r11-b9-stream-conversation-20260910-r1 --conversation
python3 tests/standalone/run-spec182-native-stream-process.py --build build-nac182 --run-root .codex-tmp/spec182-r11-b9-stream-replacement-20260910-r1 --replacement
python3 tests/standalone/run-spec182-native-stream-process.py --build build-nac182 --run-root .codex-tmp/spec182-r11-b9-stream-no-backup-20260910-r2 --replacement --replacement-no-backup
```

Raw requester, Provider and authority logs are retained under the four listed run roots. The
two-process replacement case confirms the alternate Provider path in this fixture; the no-backup
case confirms the fail-closed boundary when no admitted Provider remains.

## Boundary

该证据只关闭本地独立 C++ process chain 的可观察出口。它不关闭 15 个 maintained caller、
legacy zero-use、T014 I02--I08 dependency/no-Python 反例、exact-SIF/ELF closure、真实
MiniNDN 或 Slurm/GPU 多机资格，也不提升 T010/T011/T013/T014/T015/T016/T017 父任务状态。
