# Spec182 Revision 2 Audit and Repair Evidence

**Date**: 2026-09-06
**Evidence layer**: source review / document validation
**DesignRevision**: 2
**ImplementationStatus**: NOT_STARTED
**VerificationStatus**: PASS (document checks only; audit-revision2-checks.json)
**AcceptanceStatus**: NOT_ACCEPTED
**SourceIdentity**: reviewed HEAD 0dff7339a6df244dfe0a83c7abc9c77602452aa8 plus workspace source
**Task**: user-authorized full audit and document repairs；不是实现任务完成

## Findings and Actual Repairs

见 [audit](../audit.md) A182-09--17：输入/模型/工件/ACK policy 缺口、
Provider native host、取消语义、harness 审计顺序、迁移/回退、
O-005 自依赖、T002 分层验收/无效行数模板、过时基线、observer overflow。
本轮只改 specs/182-native-di-python-bindings/；未激活182或编辑181/生产源码。
17 个实现任务全部未勾选。新增两个内聚设计批次，原 T008--015 改为 T010--017；
旧 revision 1 evidence/design-review.md、document-checks.json、design-baseline.json 保留原值。

## Current Source Evidence

| Source | Location / observation | Design consequence |
| --- | --- | --- |
| NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/base.py | GraphAdapter:201，TaskAdapter:220，encode_input:224，decode_result:231 | native adapter 须有 inspect/encode/decode，不只 splitter |
| NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py | _request_v3:3019；ACK 后 graph/candidates，:3231 起 canonical ensure，之后 seal/grant | 保持 post-ACK 决策与 ensure→seal 顺序，原生拥有 I/O |
| NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/provider.py | ProviderOfferTrustVerifier:92 / verify_ack:245；InferenceProvider:695 / serve:715 | Core trust 与 DI candidate policy 两个边界；Python serving 需原生 counterpart |
| NDNSF-DistributedInference/ndnsf_distributed_inference/artifact_deployment.py | CanonicalCatalogEnsurer:78 | 当前 runtime 工件准备有真实 owner，不能变成 harness 前置 |
| ndn-service-framework/ServiceUser.hpp | :444 cancelStreamRequest；:603 publishSignedAppData | 复用实际 API，不虚构全功能 cancel/authority 接口 |
| ndn-service-framework/ServiceUser.cpp | :11150 cancelStreamRequest 只改变本地 stream | 远端 cleanup 需独立 deadline/control 证据 |
| NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp | NativeProviderHandlerConfig:26；makeNativeProviderCollaborationRuntime:296 | native host 复用现有 handler config/runtime |
| examples/DI_NativeProviderExecutable.cpp | :1335 Provider；:1663 tokenizer factory；:1798 runtime construction | 抽取共享 host 接线，非新增 Provider 执行引擎 |

当前 commit 不包含所有 workspace-existing 生产变化，不能当作 clean build/release。
本次来源摘要与只读检查记录见 [checks](audit-revision2-checks.json)；
下一实现基线仍由 T001 从 Spec181 最终交付刷新。

## Latest Failure Boundary Read

读取 docs/failure-log.md 的最新 failed 条目 exact-tensor R4 及后续 R6 focused PASS，
并读取 [wire repair](../../181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-repair-r3-and-negative-probe-r4)。
原始 ignored workspace 目录 spec181-exact-tensor-20260906-r4/test.log 为
239/240 assertions，旧 fixture signed packet 超界；r6/test.log 为 270/270 assertions。
这些日志本轮只读取，不重跑实验。R18 是历史首边界，不再称“最新”；
R6 focused PASS 也不是181 formal matrix/closure。

## Tooling and Validation

- 使用用户的 speckit-audit 与 speckit-code-design；用户已明确授权修正，
  所以不按只读默认停在发现清单，也不要求二次确认。
- Context Mode stats 只作 anomaly screen；project/active health 均通过。
  project guard 首次缺 require identifier，补 exact project path 后通过并重新查询。
  检索仅用于项目背景；182 authority 直接读目标目录，活动指针仍181。
- CodeGraph status 显示索引可用；广义 explore 混入临时副本后按精确主工作区
  node/源码/AST 核对。临时副本不作为代码事实。
- prerequisites 使用显式 SPECIFY_FEATURE_DIRECTORY，解析为182且未修改活动指针。
- 结构审计使用本地 speckit-audit 技能的 audit_speckit_structure.py，
  参数 specs/182-native-di-python-bindings --strict --json。
- 附加可复现检查：python3 specs/182-native-di-python-bindings/checklists/validate_revision2.py。
  验证任务连续/依赖无环、FR/CD/PO 追踪、Markdown 链接/锚点和关键 gate 顺序。
- 首次调查中的 shell 文件猜测缺失已用真实 artifact_deployment.py 路径纠正；
  一次工具编排语法错误未执行任何命令，纠正后完成。均非协议/实验结果。
- 首遍链接检查指出本次 checks JSON 尚未落盘；补齐后通过。
  diff 复审还纠正了任务重编号后的两个 gate 范围与新增表行的空行断裂；
  将对应约束加入文档检查，最终结构审计和100项链接/锚点检查通过。
- 本轮无 runtime build/unit/integration/MiniNDN/SIF/Tiger 执行。文档检查
  不生成或修改181运行失败记录，不将 source review 晋升为 behavioral PASS。

## Completion Boundary

**DesignClausesImplemented**: document corrections CD-001/007/008/011/013/014，PO-007/013/014。
**FilesActuallyChanged**: Git checkpoint 中仅本 Spec 的9个规范/清单文件、
runtime-boundaries.md、validate_revision2.py 与两份本次审计 evidence。
**SymbolsActuallyChanged**: 无生产符号。
**BehavioralProofs**: NOT_RUN；本次仅可执行文档约束检查。
**FailuresEncountered**: 上述工具查询纠正；无 runtime attempt。
**DesignDeviations**: 增加缺失 owner 和任务前置，明确 capability-preserving 的接口迁移。
**RemainingRisks**: O-001--005 和较大设计批次叶子细化仍 OPEN。
**RecoveryState**:181继续；原始文档基线可由前一提交读取，历史 evidence 不覆盖。
**NextAction**: 继续181；关闭后 T001，再 readiness audit；不启动 runtime 实现。
