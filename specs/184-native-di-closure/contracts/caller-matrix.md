# Spec184 Maintained Caller Matrix

**Status**: PLANNED / refresh required at T005
**Source baseline**: `72b9e388cc3920b0bdcd4c36d302d63c71e7f15a`

本表把维护入口按共享 runtime、owner 和验收出口分成五组。它不是 token inventory，也不
把 Python 文件存在当成 native closure。T005 必须在最终源码基线上重生成每一行，并为每
一行补 `source symbol`、有效配置、默认路由、C++ selector、compatibility/removed 决定、
零调用观察和 rollback evidence。

| Group | Authoritative source(s) | Current entry / owner | Required closure | Initial status |
| --- | --- | --- | --- | --- |
| C1 YOLO requester | `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py` | `APPClient.from_config()` → `request_task()` / `distributed_inference()`；目标 owner 为 `APPClient.request_native()` → `NativeInferenceClient` | operator-pinned catalog、preparation、grant/admission、真实 C++ request/result、失败回滚 | OPEN |
| C2 YOLO provider | `examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py` | `APPProvider.from_config()` 与 Python ONNX role handlers；目标 owner 为 native Provider host | native provider registration/execution/stop、模型与权限契约、旧 Python handler retirement | OPEN |
| C3 Qwen/LLM requester | `examples/python/NDNSF-DistributedInference/llm_pipeline/user.py` | `request_streaming()`、`request()`、`distributed_inference()`；目标 owner 为 native unary/stream/conversation facade | generation/tokenizer identity、C++ observer、两轮 continuation、terminal ordering | OPEN |
| C4 Qwen/LLM provider | `examples/python/NDNSF-DistributedInference/llm_pipeline/provider.py` | `APPProvider.from_config()` 与 Python Qwen/stream/conversation handlers；目标 owner 为 native Provider host | native generation/stream execution、provider lifecycle、旧 Python runtime retirement | OPEN |
| C5 Harness and collectors | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`; `Experiments/NDNSF_DI_QwenAckDriven_Minindn.py`; `Experiments/NDNSF_DI_StreamedGeneration_Minindn.py` | launcher/collector 负责启动、隔离、trace 和 cleanup，不拥有 native request behavior | delegated caller 的真实 C++ owner、子进程 exit/trace pairing、no-Python boundary、rollback | OPEN |

`examples/DI_NativeRequester.cpp`、`examples/DI_NativeProviderExecutable.cpp` 和
`NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py` 是生产入口/owner
参考，不另算维护 caller 组；它们必须在对应行的 production-entry lane 中登记。

## Required Row Fields

每个最终 caller row 必须包含：

`groupId`, `sourcePath`, `symbol/line`, `mode`, `effectiveConfig`, `defaultRoute`,
`nativeOwner`, `compatibilityOrRemoved`, `C++ target/selector`, `build/import closure`,
`riskClass`, `dynamicProfile`, `dynamicInvariant`, `zeroUseCheck`, `rollbackEvidence`,
`candidateId`, `status`。

T005 使用下面的源检查作为起点，并对发现的新增入口追加独立 row；命令结果本身不能替代
语义判断或 C++ 运行结果：

```text
rg -n "APPClient|request_task|request_streaming|request_native|distributed_inference|APPProvider|execvpe|di-native-provider" \
  examples/python/NDNSF-DistributedInference Experiments NDNSF-DistributedInference/ndnsf_distributed_inference
```

在至少一个 caller 获得真实 native result、其余组有明确 `OPEN`/`TRANSFERRED`/`REMOVED`
理由并完成零调用观察前，C3/SC-003 不得标记 `PASS`。
