# B189 ONNX identity and resource-boundary checkpoint

## Scope

本记录只覆盖 canonical ONNX source identity、native assembly 的重复解析修复、
以及当前主机上的资源边界。它不是 `QWEN_TWO_PROVIDER_PASS`，也不关闭 T003、
T006 或 T009。

## Static review

`NativeOnnxRecipeAssembler.cpp` 的 `assembleCertifiedOnnxChain` 现在只通过
`ownedSourceModel(source, sourceControl)` 完成一次严格 source parse/外部数据展开，
随后从同一已拥有的 `ModelProto` 调用 `canonicalOnnxModelIdentity`。因此重复的
1.5 GiB initializer/protobuf 分配被移除，同时保留 duplicate/location/function/nested
Tensor 校验、`length=0` 到 EOF 规则、source/assembled byte caps 和取消检查。公开的
`canonicalOnnxSourceIdentity` 签名及其行为保持兼容。

官方只读 `review-agent` 复审结果为 `STATIC_PASS`。峰值 RSS、取消时序和完整协议
运行仍必须由后续 runtime evidence 证明。

Python canonical identity 层的静态审查也已通过：外部文件采用 rooted/no-follow
路径校验、流式 digest、重复元数据拒绝和稳定 regular-file 检查；对应 Spec170
定向测试为 `17 passed in 7.40s`。真实 Qwen identity scan 为 5.61 秒、峰值
98,896 kB、swap 0，得到 graph/initializer/tensor digests 和 311 个 tensors。

## Native build and selectors

在已配置的 `build-spec189-b189-3-global-r3` 中，使用系统工具链和 `-j2` 完成
`ndnsf-distributed-inference,di-native-assembly-worker,unit-tests` 构建，耗时
`1m52.201s`。随后构建并运行了缺失的五个 Spec182 worker tool targets。

最终独立 selector 结果：

- `Spec182OnnxIdentity*`: 11/11 cases, 658 assertions;
- `Spec182NativeAssembly*`: 5/5 cases, 27 assertions;
- `Spec182OnnxActivation*`: 9/9 cases, 76 assertions;
- `Spec182OnnxExtraction*`: 11/11 cases, 62 assertions;
- `Spec182OnnxWorkerProtocol*`: 30/30 cases, 397 assertions。

第一次运行 worker protocol selector 因五个 worker tool 尚未生成而失败；补建这些
测试目标后重跑通过。一次把所有 selector 合并到单进程的尝试因约 6.3 GB RSS
和持续 swap 被中止，不能计为全批通过；独立 selector 结果也不替代 Spec189
真实两 provider 资格。

## Qwen run boundary

`two-provider-global-r30` 在 canonical identity 修复后仍在 MiniNDN workload 前触发
`RESOURCE_BOUNDARY:swapIo`；model/candidate/bundle preparation 通过，workload
为 `NOT_EVALUATED`。该次采样最高 RSS 约 4.75 GB、swap-I/O 增量约 396 MB。

随后 `two-provider-global-r31` 使用新 candidate digest
`sha256:8de570336fa5fb73d84b99247b48b7261d4bb71a3db5ae4a485b02070b11f74d` 重跑。
它在 MiniNDN 启动前触发同一 `RESOURCE_BOUNDARY:swapIo`，最高采样 RSS 约
403 MB、swap-I/O 增量约 293 MB；machine/candidate/model/bundle/cleanup 的可见
状态已记录，MiniNDN、workload 和双 provider 协议结果仍为 `NOT_EVALUATED`。
这两次运行不能作为模型执行失败或成功的证据；下一次运行必须使用新的 run-id，
并先取得稳定的主机内存/swap 基线。

在主机完成一次可逆 swap reset 后，r32 使用相同的新候选和正确的 global profile
再次运行，但仍在 MiniNDN 启动前触发 `RESOURCE_BOUNDARY:swapIo`；13 个样本的
swap-I/O 增量约 280 MB，`minindn` 和 workload 均为 `NOT_EVALUATED`，cleanup 为
`PASS`。该次结果确认当前阻断首先是主机资源基线/守门器边界，不能据此判断 native
assembly 或两 provider 协议是否成功。

随后 r33 在再次降低 swap 压力后成功完成 prepare，并进入 `admission/running/drained`
采样；但仍在 MiniNDN 启动前触发 `RESOURCE_BOUNDARY:swapIo`。21 个样本的最高
RSS 约 310 MB、swap-I/O 增量约 275 MB，cleanup 为 `PASS`，MiniNDN/workload
仍为 `NOT_EVALUATED`。这证明守门器正在捕获主机换页，而不是模型执行结果；在
获得稳定的低换页主机基线前不再重复相同 run。

原始 run records 和 samples 保留在 `.codex-tmp/spec189-qwen-two-provider-20260918/`
下，不入 Git。详见 [tasks checkpoint](../tasks.md) 和 [failure log](../../../docs/failure-log.md)。
