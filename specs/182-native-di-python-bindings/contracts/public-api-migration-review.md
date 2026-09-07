# Public API Migration Review

**Status**: INVENTORY_CAPTURED / O-004 OPEN / T001 IN_PROGRESS

## Source Inventory

[Public export snapshot](public-export-inventory.json)由[AST extractor](../checklists/export_inventory.py)读取源码生成，不import NDNSF/Python runtime。覆盖正式api显式27项、SDK显式76项、root compatibility manifest174项，共277项导出；264项定位到定义、10项为assignment、3项为外部owner。每项migration初值UNREVIEWED，不是迁移完成计数。

外部owner为GenericRepoClient→py_repoclient.RepoClient，以及encode_ack_metadata/parse_ack_metadata→ndnsf.runtime_telemetry。它们不能因不在DI包源码内就删除或假定已原生；须检查既有依赖/绑定。snapshot记录定义文件SHA256、类公开方法/构造/校验方法、property/classmethod等decorator及annotated字段；不覆盖动态app_sdk wildcard exports、继承方法、实例赋值字段、assignment alias的最终展开或逐参数语义。这些限制是剩余工作，不能拿277条JSON行证明O-004关闭。

复现：`python3 specs/182-native-di-python-bindings/checklists/export_inventory.py --namespace api`，sdk/root同理；无namespace输出全部。按(namespace,export)键比较结果，JSON对象/条目顺序不构成API语义。分namespace读取避免工具输出截断；初次全集输出被工具长度限制截断，不是源码扫描失败，不能将截断内容当完整snapshot。

## Current API Mapping Gaps

以code-design.md、symbol-design.md、value-contracts.md、runtime-boundaries.md四份主契约检查正式api导出：23个导出名称尚无精确名称映射。仅名称缺失不是功能必然不存在，但说明当前48个planned方法/137个字段表不能证明完整Python兼容。下表冻结下一轮逐行为核对入口，不增加新的产品目标或直接宣布全部旧导出都必须重写C++。

| Existing surface | Required mapping decision / existing task |
| --- | --- |
| InferenceApplication、InferenceClient | define/publish_definition/deploy、deployments/requests查询、request_model/request_task/request_preplanned/request、run/run_async分别对应原生owner或既有明确退出路径；不能只给request一个binding就声称整个facade兼容。T010/T012/T013 |
| InferenceRequestHandle、InferenceResult | ref/status/deployment_status/events/wait/result/result_async/cancel；结果的status/payload/error/request_id/data_name/signer_certificate/wire_digest需保留来源及语义，特别是provenance，不能压成成功bool。T010/T012 |
| DeploymentDefinition/Ref/ActivationRecord、DeploymentHandle/HandleRef、DeploymentRef/Status/Summary/Progress、RequestableDeployment、RequestRef | 有签名、版本、journal locator、activation及网络查询行为；先展开alias及真实调用方，再映射已有sealer/client/conversation或其他保留端口。禁止用仅离线JSON转换代替部署/查询运行能力，也不假定conversation journal覆盖部署catalog。T001决定，挂回对应现有任务 |
| ArtifactReference、DeploymentConstraints、ModelIntent、OptimizationObjective、RequestContract、InferenceOptions | 检查字段/default/validation、NDN名字、schema及返回转换，映射已有native value契约；纯值binding不得偷偷执行网络或策略。T003/T010/T012 |
| GenerationConfig、GenerationInput、ModelRef | to_task_options/to_task_value与现有模型/生成配置对应；不可忽略stop、timeout、adapter_name、prompt metadata或digest差异。T003/T007/T010/T012 |
| ProviderDeploymentOffer/Offers、InferenceProvider | 认证/激活offer与V3规划offer是否不同契约须区分；serve的Python callable退出按既定182规则，服务关闭新行为见生命周期契约。不能把ProviderAdminPort生命周期权限塞入serving facade。T005/T008/T009/T012 |

ProviderAdminPort不在正式api.__all__中，但在app_sdk.provider.__all__可达，stage/activate/drain/delete/report_progress/report_checkpoint必须进入动态层后续清单。sdk的Python policy/plugin执行面按已有“native策略、Python观察/离线保留”边界分类，不能机械为76项都加Python回调至C++。root的legacy runtime、llama、simulation/evidence等按当前声明范围逐项保留、重定向或显式退出；不凭模块名自动删除。

## Closure Criteria

在snapshot之外为每个受影响导出补上disposition、native/retained owner、参数/字段语义、实际消费者和验证selector；精确展开formal api的部署与请求catalog行为，补动态app_sdk exports及外部依赖，核对T001已冻结的流式/注册设计在主表中有对应项。未涉及182运行目标的离线/历史能力可明确保留，但必须有调用证据。不得把所有UNREVIEWED批量改成BOUND或因文档结构PASS关闭T001。
