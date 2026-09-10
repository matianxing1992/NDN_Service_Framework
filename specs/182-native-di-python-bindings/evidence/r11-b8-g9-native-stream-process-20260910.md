# R11-B8-G9 Native Stream Process Revalidation

## Boundary

2026-09-10，使用当前 `Experimental` 工作区的
`build-nac182/examples/{App_ServiceController,DI_NativeArtifactAuthority,DI_NativeRequester,di-native-provider}`，
运行独立 C++ requester、Controller、grant authority、Provider 和私有 NFD
夹具。夹具只负责进程生命周期与隔离目录；请求、授权、Provider 执行、stream
事件和 token oracle 均由 C++ native owner 完成。

## Command and result

```text
python3 tests/standalone/run-spec182-native-stream-process.py \
  --build build-nac182 \
  --run-root /tmp/s182r16-stream-1789060395
```

结果：`STREAM_RC=0`，requester `rc=0`。requester 日志包含：

```text
NATIVE_STREAM_ORACLE_PASS tokens=4,5,6,7,8,9,10,2 events=8
NATIVE_REQUEST_SUCCEEDED request=/NDNSF/DI/REQUEST/...-1 plan=sha256:...
```

Provider 日志包含 `NDNSF_DI_GRANT_VERIFICATION`、
`NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED`，其中 `realCompute=true`、
`runnerKind=onnxruntime-cpu`、`cpuFallbackUsed=false`、
`executionCompleted=true`。完整原始日志保留在上述 run root，不纳入 Git。

## Closure decision

本批关闭独立 C++ stream process 的当前验证边界，证明 native requester → Core
→ Provider → stream result 的真实过程和 token oracle 可达。它不关闭
`T013-D` 的维护 Qwen caller 迁移、其余 maintained caller groups、legacy
zero-use、no-Python 或 `T016` 完整资格；这些仍保持 `PARTIAL`/`UNQUALIFIED`。

## Static and build lanes

- **Static review**：本批只新增 evidence 与任务同步，无生产源码变更；共享
  process driver 的 socket guard 已在 G8 通过静态审查。
- **Build**：复用已验证的 `build-nac182`，本批未重编译。
- **Focused behavior**：独立 C++ stream process exit 0，8/8 token oracle
  events，Provider real-compute evidence present。
- **Qualification**：仅为 R11-B8 native stream process 边界；不是 T016 qualification。
