# Shared Runtime Reuse Boundary

**Status**: IN_PROGRESS
**Evidence layer**: implemented / wired / executed (focused checks only)

## Decision

按用户要求，YOLO 与 Qwen 应共用 DI 执行机制；模型差异由已有
adapter/runner 扩展点表达。复用 Qwen 案例已实现的公共机制属于
Spec181 源码收口，不把 Qwen 模型运行、跨模型资格或性能结论加入
本次验收。代码共用比例未经测量，不用百分比描述完成程度。

## Current Source Map

| Concern | Current owner / entry | Status and boundary |
|---|---|---|
| Adapter contracts | Python `adapters/base.py::ModelFamilyAdapter`；`adapters/qwen/placement.py::build_qwen_three_stage_adapter`；`adapters/yolo/adapter.py::build_yolo26n_adapter` | existing：两者组装同一组 graph/split/task/state/runner 端口，任务编码与状态契约不同 |
| Request / ACK / Selection | Core `ServiceUser` / `ServiceProvider`；DI `app_sdk/placement.py`、`NativeProviderOfferV3`、`NativeProviderHandler` | existing：模型配置和角色数据进入既有协议；不得为第二模型复制一套调用协议或 Provider 主循环 |
| Protected artifacts | `provider.py`、`NativeProtectedProvider`、`ProtectedRuntime`、`NativeCanonicalOnnxAssembler` | existing：共用 grant 验证、精确名获取、受保护装配和租约清理；Python/native 为语言边界，固定 parity 保持语义一致 |
| Role execution / transport | `NativeProviderRuntime`、`ProviderRoleWorker`、`DependencyIo`、tensor codec / group runtime | existing：普通角色与生成 epoch 使用同一角色执行基础；角色名称、张量与依赖是契约输入 |
| Model execution | `NativeModelRunner` / `RegistryNativeModelRunnerFactory`、`cpp/adapters/onnx/OnnxRuntimeModelRunner` | existing：ONNX 加载、设备选择与执行证据复用；按 adapter/backend 注册实际计算实现 |
| Iterative generation | `NativeEpochCoordinator`、runner 的可选 streaming/state 接口 | existing：多轮调度基于同一 runtime；token 停止条件、tokenizer、KV/混合状态由 generation 与模型 adapter 提供，不要求无状态 YOLO 实现这些能力 |
| YOLO semantics | Python `adapters/yolo`；native `cpp/adapters/yolo/NativeYoloMergeRunner` | executed：算法已移入 adapter，旧 runtime 头文件只保留兼容 include；统一生产构建及隔离 P-256 正向控制 PASS |
| Qwen semantics | Python `adapters/qwen`；`cpp/adapters/qwen/QwenGenerationSession` | existing：tokenizer、模型输入/输出、prefill/decode、完整 attention KV 与 recurrent/convolution 状态；具体状态内容归 adapter 所有 |

本表的 native 路径均相对 `NDNSF-DistributedInference/cpp/`，Python
路径相对其 `ndnsf_distributed_inference/`。CodeGraph 查询后按当前
文件复核；索引含旧临时快照，索引行号不作为当前实现证明。

## Closing Work

1. T002（implemented + focused PASS）：收拢现有 native 准备 factory，公共 grant/context、证据绑定、
   profile/资源生命周期初始化只保留一个 owner；模型分支只产生
   runner spec，不复制上述公共准备逻辑。
2. T002（implemented + focused PASS）：native YOLO 后处理算法归入 adapter，保留已有公开入口的
   兼容性并同步 Waf 生产/定向测试源清单；不新建第二套 Provider。
3. T007：复核普通执行、缓存及可选生成 epoch 都经过公共授权、
   取消/截止和清理边界；发现旁路时修复公共 owner，并用真实 runtime
   加最小 stateless/stateful 扩展点用例验证，不能仅以符号相同判 PASS。
4. 公共代码变化必须运行受影响的既有 Qwen/生成接口定向回归；不
   启动新的 Qwen 模型/集群资格任务。YOLO 正式资格仍归 T005/T008--T012。

不预先创建覆盖所有模型的大型基类，不合并无状态与会话状态所有权，
不复制 Qwen 实现再改名。已有公共实现直接复用；只有确认重复且
语义一致的片段才提取小接口。

## Document Validation

公共准备提取与 adapter 迁移的当前证据见
[T002 shared preparation](t002-shared-preparation-20260905.md)。12 项
定向检查、统一 native 构建和隔离 P-256 正向控制通过；generation
worker 授权/取消传递审查尚未完成，T007 仍 BLOCK。

`audit_speckit_structure.py --strict` PASS：15 FR、6 SC、4 user stories、
12 tasks（4 complete）、15 FR traced。`git diff --check` PASS。
这证明文档结构和映射完整，不证明待收口实现或跨模型资格已通过。
