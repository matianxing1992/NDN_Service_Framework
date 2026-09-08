# 设计覆盖矩阵

“全部内容”以四模块的完整子系统设计视图组织，涵盖职责、入口、状态和边界；不表示逐行复述所有文件或完成所有运行资格验收。

| 章节 | 范围 | 主要核对入口 |
|---|---|---|
| 1–2 | 总体、模块边界与源码身份 | docs/architecture.md、模块目录与绑定入口 |
| 3–8 | Core 角色、普通/Targeted 调用、协作事务、对象/流、并发与请求保密 | ServiceUser、ServiceProvider、ServiceContainer、StreamFacade、RequestConfidentiality |
| 9–13 | 在线授权/撤权、版本、发现、取钥与失效范围 | ServiceController、PolicyRefreshCoordinator、ControllerVersion、User/Provider 状态安装 |
| 14–17 | Repo 对象/packet、SQLite、制品后端、会话、目录、副本与恢复 | RepoCore、RepoNode、RepoClient、artifact_api、network_artifact_backend |
| 18–23 | DI 应用 API、规划/报价、封存/授权、角色执行、token/KV、部署扩展 | app_sdk、NativeRequestPreparation、NativeV3Placement、NativePlanSealer、NativeInferenceClient、ProviderSession/Runtime、NativeEpochCoordinator |
| 24–28 | UAV 容器、飞控/权限、巡逻补偿、视频/录像、检测/多视角 | UavDroneApp、GroundStationApp、UavNames、FlightControllerBackend、MissionSession、VideoPublisher、识别工具 |
| 29–31 | 跨模块流程、构建配置、运行边界、测试入口与实现状态 | 架构文档、构建入口、当前 Spec182、实验入口 |
| 32–35 | 三组源码索引和后续维护规则 | source-baseline.json、module-inventory.json |
| 36–58 | 23 组 API 阅读/行为/扩展契约，53 个准确签名示例 | api-contracts.json、api/contract-map.json、api/inventory.json |

## 清单口径

模块清单登记 Core 56、Core Python 18、UAV 98、DI 391、Repo 43 个文件，共 606 个。
清单包含代码、配置、文档与资产；不把测试存在或清单完整视为运行通过。
R0 有 94 个关键文件；R1 扩展为 350 个源码快照文件，覆盖 295 个 API 清单源文件。主要流程经源码核对，其余支持文件不声称逐行审计。
API 清单包括 16558 个声明（5738 个函数条目，其余为类型/字段/枚举等），不是公开 API 数或资格通过数。范围与解析边界见 api/README.md。

## 特别保留的实现边界

- NativeInferenceClient 的原生 dispatch 仍返回 NATIVE_REQUEST_PIPELINE_NOT_READY；C++ Provider 组件存在不表示默认应用链已完成原生迁移。
- grant 的局部 DKEY 刷新优化不等于无关节点无需任何状态更新；服务版本推进仍可失效运行缓存。
- Repo 网络整制品发布目前要求 Collaboration；公开选项不意味着 TARGETED 发布实现可用。
- Core 已含通用不透明字节 FEC，UAV 保有视频编码和媒体策略。
- UAV 模型工具、模拟夹具与真实飞控/模型入口分别说明，不据此声明硬件或准确率资格。
