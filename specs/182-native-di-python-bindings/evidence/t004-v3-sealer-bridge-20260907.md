# T004 Admitted V3 Plan Sealing

## Design and Status

PARTIAL。新增 sealCore 重载直接消费 inspected model、split candidate、完整 V3 proposal、
实际 execution plan、admitted offers、Core owner 的原始 ACK_CLOSED digest 与 sealing inputs。
不构造旧 NativeProviderPlanningView，也不从 policy 填充资源。prepareRoles 与 sealer 共用
validateRoles；V3 策略与 proposal 校验共用 feasibleChoices，校验允许可行的自定义分配，
不强制重新运行默认排序。最终角色除了 backend/device 选择，必须与准备契约完全一致。

core 的 assembly 来自通过校验的选定角色，仍复用既有 canonicalCore/core.validate 与
finalizeSecurity/project/encode。新 grantView 接受 admitted offer，允许合法 exact-reuse
disposition，不再要求 preparationAccepted=true；两个 grantView 共用身份与 policy 校验。

真实 catalog/Repo 的 ACTIVE root 含 origin/transformation attestations 与模型/profile
身份，不能用简单 fetch 或 callback 成功替代它们的验证。本轮没有新增假 catalog owner；
该端口实现和 NativeInferenceClient 真实 Core Begin/Commit 主链仍未完成。

## Validation Plan

增加 CPU、GPU exact reuse、多 rank 的真实 SDK core digest oracle，offer 使用固定公开测试
key 签名和长期有效的 fixture expiry（不是实验资格证据）。原有 placement 输入经 preparation
和 admission 后直接 sealCore/grantView；拒绝 artifact、device、offer digest 与 ACK_CLOSED
替换，接受合法的非默认 Provider 选择。源码复核后使用上一轮新 ABI 树增量构建；
此次只增方法/函数，无已有 DTO layout 变更。raw：`.codex-tmp/spec182-t004-v3-bridge-r1/`。

r1 build PASS（25.794s），相关 55 cases/481 assertions PASS。源码复核补上 execution
role 顺序必须与 proposal role 顺序相同：SDK core 保留角色 tuple 顺序，不能让另一个
executionPlan 顺序悄悄改变 core identity。增加反序拒绝后独立 r2 重验。

## Focused PASS

`.codex-tmp/spec182-t004-v3-bridge-r2/build.log`：同一新 ABI consumer 树增量 -j4 build
PASS（21.173s），system PATH 与既定依赖闭包；无新的 DTO layout 修改。
focused.log：Spec182V3Placement、Spec182Preparation、Spec182OfferAdmission、
Spec182NativePlanning、Spec182PlanSealer、Spec182GrantClient，55/55 cases、482/482 assertions PASS。
三个真实 SDK core digest 与原生完整 proposal→sealCore 一致；admitted grantView 可用于
GPU exact-reuse 与多 rank，错误工件/设备/offer/ACK 身份及 role 顺序拒绝。
合法自定义 Provider 选择通过同一可行性验证，不要求等同默认策略输出。
Context Mode project health 仍为 NO_REAL_SESSION_EVENTS，使用仓库与 CodeGraph 回退。

这不是完整请求验收：实际 catalog/Repo、策略基类的 V3 调用接口、requester Begin/Commit、
dataflow/device-binding 生成与网络资格仍未闭合。T004-A 保持 PARTIAL，没有新增 DONE。
