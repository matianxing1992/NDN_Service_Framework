# Specification Quality Checklist: Native NDNSF-DI

**Created**: 2026-09-06
**Revision**: 4
**Feature**: [spec.md](../spec.md)
**Status**: DRAFT; design-document delivery only

## Content Quality

- [x] 用户目标可观察：独立 C++ 调用、Python 可选绑定。
- [x] 中文叙述与英文 structural markers 保留。
- [x] 已定义 user stories、FR、SC、edge cases、scope 和依赖。
- [x] Code Design 细节明确纳入规范性附件。
- [x] 已记录与当前 Python planning 架构的改变和替代方案取舍。

## Requirement Completeness

- [x] 18 个 FR 均有 CD/T/PO 和成功判据。
- [x] 10 个 SC 区分 source review、运行、资格和交付。
- [x] 默认 native 策略不允许 Python trampoline。
- [x] 冷动态装配与完整 tokenizer 文本不能降级为离线预切/warm/token-only。
- [x] Core 不吸收模型差异，Provider 独立验证不因共用 C++ 而删除。
- [x] 合并基线/181承接取代旧完整资格前置；本轮不改合并修复源码。
- [x] 所有实际未执行的测试/命令和新符号标 planned。
- [ ] O-001 最终 merged baseline / 181 handoff 已冻结。
- [ ] O-002/O-003 原生依赖/字节与 tokenizer 契约已关闭。
- [ ] O-004 完整 native fields/signatures/legacy caller 迁移已关闭。
- [ ] O-005 隔离设计由 T001 冻结；T014 实现后另验反例。

- [x] input/graph/artifact preparation 和 ACK provenance/policy admission 有 CD-013/T008/PO-013。
- [x] Provider 绑定有 CD-014/T009/PO-014 原生宿主，不仅命名 Python 转发。
- [x] 本地取消、远端收束、observer overflow 与业务失败分开定义。
- [x] T014 先实现正式 harness，T015 审计，T016 只执行冻结验收。
- [x] 旧 callback、默认退出、mixed-version 和 journal 回退有明确契约。

## Feature Readiness

- [x] 每个设计批次有 DecisionBudget、AllowedDecisions、ForbiddenChanges、ExpectedDiff、
  ProofObligations、planned Commands、EscalationConditions 和 RecoveryPoint。
- [x] 证明使用独立 oracle 和语义反例，不以 unit 数量冒充真实链通过。
- [ ] T001 已将大批次、selectors 和 leaf signatures 冻结为可实施单元。
- [ ] Design Readiness 已达到 READY_FOR_IMPLEMENTATION。
- [ ] Implementation / formal qualification 完成。

## Skill Application

本 Spec 应用 speckit-specify 和 speckit-code-design。
后者明确规定通用 “No implementation details” 不适用于代码设计章节；
不为模板要求删除用户要求的签名/状态/调用/证明细节，也不改全局模板或旧 Spec。
specify CLI 未在 PATH；仓库只有一个 active spec template，直接使用其结构。
活动指针已指182；managed plan/source索引按当前活动设计核对。
上述未勾选项表示实施就绪待办，不表示本轮文档未创建。

## Symbol Documentation

- [x] 已建立类、方法、状态和137个来源字段的职责/注释/用法契约，明确existing/planned。
- [x] FR-017/SC-009与所有实施单元的SymbolContracts/Documentation/Usage同步。
- [x] Core已合并安全/撤销能力与DI artifact grant范围分开；CandidateBudget依据实际3字段修正。
- [ ] 全部嵌套schema、错误/配置/caller清单与原生ABI已冻结。
- [ ] 所有公开示例实际编译/运行通过；当前仅DESIGN_EXAMPLE / NOT_COMPILED。

## Pre-Test Static Review

- [x] S0是读代码逻辑对照设计，独立于lint/编译/结构检查。
- [x] 所有17任务有StaticReview；单元首次unit/integration前审查，T015整体审查先于T016。
- [x] 定义finding证据、控制性问题修复/复审、subject身份、AllowedTestScope、失效和分层入口。
- [x] 具名RED/mutant先审查受控缺陷，不能据此放行普通测试。
- [x] FR-018/SC-010/PO-015/SR-001--009及任务/证据映射齐全。
- [ ] 产品实现的S0报告已实际完成；当前NOT_RUN。
