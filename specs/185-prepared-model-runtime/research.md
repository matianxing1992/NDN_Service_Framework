# Research Decisions

## Decision 1 - Reuse Native Catalog

采用 NativeRequestCatalog / NativeCanonicalPreparationCatalog，封装准备 owner，不新建第二套模型解析器。
依据见 audit A01/A02。替代方案“每次 request 全量重新解析”和“完全信任 caller inspected model”均拒绝。

## Decision 2 - Explicit Immutable Identity

本期要求显式 pinned model/catalog/source；不实现 mutable latest 目录发现。理由是已有 native load
不验证远程 catalog signer，补完整远程模型注册协议会使本 Spec 膨胀。网络 source 只复用可信 fetch。

## Decision 3 - Facade Before Retirement

NativeInferenceClient 降为内部执行责任，但过渡期保留其旧签名与导出。不在同批重写 request 状态机。
新 API 保留类型明确的扩展选项，普通用户不传 grant/plan；诊断/操作员配置仍显式存在。

## Decision 4 - Two Distinct Cache Owners

requester preparation cache 与 Provider artifact/template cache 分批实现和验收。
共享 live mutable runner/KV 拒绝；完全无缓存是可靠回退，但不能据此关闭 cache 的验收任务。
若某 backend 不支持安全 template 复用，支持矩阵只声称 artifact reuse。

## Decision 5 - No New Distributed Barrier

不添加全局 ASSEMBLING/READY 握手；保留 Provider 局部准备与 authenticated data 驱动执行。
会话的 durable commit 是独立机制，不能被 RequestHandle facade 简化掉。

## API and Native Independence Revision

[API review](api-review.md)补齐全表面盘点及ONNX Runtime、Ray Serve、Triton、vLLM官方API比较；采纳模型对象、元数据、可靠结果/流和独立管理面，不宣称实测可用性优势。
[C-05](contracts/api-usability.md)与[C-06](contracts/cpp-first.md)明确独立C++入口：安装SDK/ABI→原生能力→完整C++验收→Python包装。

本轮完整性审计选择统一Subscription和native有限resultAsync，补可退订read/observer和明确析构；目的是避免Python为了实现async语法另造取消/状态owner。
易用性目标以[C-07](contracts/api-catalog.md)常规调用层及逐行消费检查证明，不以“类似其他框架”推导全部接口已合理。
