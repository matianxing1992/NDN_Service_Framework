# T003 Local Closure Audit

## Scope and Authority

基线 e3640279。核对 T003 work-unit、T003-A/B/C execution cards、重排 RC-03、
实际 splitter/placement 与独立 oracle 消费测试。原 work-unit 明确 LocalChecks
属于 T003，FinalProof PO-002 由 T016 统一产生；本审查不改变产品支持范围或最终门。
此前进度行将 T010 requester 接线作为 T003 前置卡的 DONE 条件，与既定依赖形成
循环。本记录以逐项证据修正局部卡状态，保留后续生产工作，不以重排本身证明完成。

## Requirement Evidence

| Card / requirement | Current implementation and proof | Boundary |
| --- | --- | --- |
| T003-A strategy/shared contracts and Qwen cover | NativeQwenLayerSplit::inspectGraph/enumerateFromMetadata/enumerate；NativePlanning 的完整 model/candidate/resource 类型；QwenLayerSplitProducesCanonicalRankOneCandidate、QwenLayerSplitRejectsInvalidRankAndGraph、QwenLayerSplitEnforcesBudgetBoundaries、QwenMetadataBuildsMaintainedGraphAndCandidate | 维护 rank-one splitter 支持范围不扩展；实际 ONNX export/state/rank source 归 preparation |
| T003-A complete candidate/resource identity | CompleteCandidateIdentityMatchesMaintainedPython、CandidateIdentityRejectsStaleDigestForValidContractChanges、HybridCandidateRejectsIncompleteRankAndRedistributionContracts、ResourceBudgetsMatchMaintainedPythonContract、PlacementAccountsForKvAndRejectsUnknownPeak | 冻结 Python 生成器调用维护 splitter/contract；C++ 比较完整规范字节和摘要，不仅比较注册 ID |
| T003-A owned ONNX inspection | OwnedOnnxGraphMatchesMaintainedPlanningAndCanonicalIdentities、OwnedOnnxGraphRejectsForeignAdapterSourceAndExpiredControl | 实际 ONNX fixtures；源解析不承担网络认证，semantic graph 不冒充 source indices |
| T003-B real catalog/component/interface/order | NativeYoloComponentSplit::fromOnnxCatalog；YoloCatalogConsumesActualOnnxSemanticPartition、YoloFragmentMatchesMaintainedSplitterAndBindsRegistration、其余 YoloComponentSplit/YoloUsesRealBranchTensors 用例 | 实际源图→完整 semantic partition→绑定 splitter；错误接口/依赖/切点、外模型、覆盖、预算与排序拒绝/对照；catalog 网络取得留 T008 |
| T003-C compatible deterministic placement | NativePreSplitFirstPlacement、NativeV3Placement；PreSplitPlacement/Placement 系列与 Spec182V3Placement 全部 4 cases | 固定 snapshot；容量、设备、lease、exact reuse、rank cover、摘要/offer 篡改及顺序独立；actual SDK placement oracle |

独立数据来源及既有失败修正详见 [candidate](t003-candidate-identity-20260908.md)、
[Qwen metadata](r1-b1-qwen-metadata-20260908.md)、
[YOLO semantic](t003-yolo-semantic-batch-20260908.md)。不重新生成 expected 来适配实现。

## Current Validation

复用 e3640279 收录源码对应的 [R1-B5 focused log](../../../.codex-tmp/spec182-r1-b5/focused.log)：
Spec182NativePlanning **28/28 cases、817/817 assertions**；Spec182V3Placement
**4/4 cases、348/348 assertions**。同轮总 69/69 cases、1560/1560 assertions PASS，
还覆盖 preparation/publisher 与 ONNX extraction；该日志并非另一套 ABI 树。
本轮不改产品源码，不因开始审查重跑构建或相同单测。

case-manifest 的 T003-A/B 使用 selector alias→existingSuite 映射，不能直接运行
不存在的 Spec182QwenSplit/Spec182YoloSplit suite。本次补登记已实现的 Qwen rank/budget
负例、完整 placement 用例与 V3 suite，清除这些已实现项的 planned 标记。
提交前验证 registered case 实际存在、原始日志确有对应成功、文档验证与 diff。

## Remaining Ownership

T003-A/B/C 局部验收通过；T003 的 PO-002 和 feature 资格仍由 T016 实测。
T008-A 必须组合真实 adapter、catalog inspection、source/role producer 和 publisher，
完成 Qwen semantic-to-source/state 适配及完整两模型输入/结果；R1-B5 generic owner
不能充当这个组合入口。T004-A 仍须产生 dataflow/deviceBinding，T005 仍须重新核对
真实 grantView 消费；T010-A/B 拥有默认 requester 全链与终态。
R1 阶段的实际 source/role 映射出口仍未关闭；T003 卡通过仅使已满足的局部前置
可被下游消费，不把阶段、T016 或 Spec 整体标为完成。

Context Mode project health 0、active health 4（索引过期）；CodeGraph 命中历史 staging
副本，拒用该副本并读取 canonical 源码。均属检索边界，不是产品执行失败。

## Audit Result

**PASS (local acceptance only)**。测试注册交叉检查逐一要求 existingSuite/Case 出现在
实际 C++ 源码，并在 R1-B5 原始日志中有同名 passed 记录；所有 T003 注册项通过，见
[registry-check.json](../../../.codex-tmp/spec182-t003-local-closure/registry-check.json)。
`validate_design.py` exit 0、errors=[]，见
[design.json](../../../.codex-tmp/spec182-t003-local-closure/design.json)；`git diff --check` exit 0。
本轮只修订状态、验收责任说明与测试注册，不重编、不重跑产品测试，不改冻结 oracle。
其他会话的 native-dependency-design.md/native-generation-design.md 不纳入 checkpoint。
