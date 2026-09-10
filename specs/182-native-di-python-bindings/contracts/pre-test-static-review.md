# Source Review And Runtime Validation

**Revision**: 8 | **Status**: planned; product STATIC_REVIEW NOT_RUN

本文件统一 FR-018/019、SC-010/011、PO-015/016 的执行顺序，不新增产品 API。
2026-09-08 按用户要求改为逻辑批次；既有通过证据和任务验收保持原义。

## Workflow

唯一通用规则为 [pre-test-static-review](../../../skills/speckit-code-design/references/pre-test-static-review.md)，逐任务读取其中的 [review-agent profile](../../../skills/speckit-code-design/references/review-agent.md)。
每个小任务编码后只读静态审查，同批实现依赖满足后继续；整批逻辑/流程审查通过再统一构建与相关 C++ unit/integration/process tests，最后 Python wrapper checks。
执行前在 tasks.md 当前 checkpoint 登记批次 ID、成员、行为边界、共享 selectors/owner 及实现/验收依赖。此修订未把既有 Depends 自动转为实现依赖，也未将 T002--T014 合成一个大批次；T001 release、卡片硬前置和设计缺口仍控制执行。

## Spec182 Ownership

- T001：冻结基线、具体 unit/integration selectors 与负例归属，关闭既有 O-001--005。
- T002--T014：按 [native-first order](native-first-execution.md) 的 N1--N5 实施；N1--N3 在迁移调用方前完成定向 C++ process 出口。每小任务静态门，批末 C++ unit/integration/process，再做 wrapper checks；Python 用例数不推进 native 任务。
- T015：补审跨任务生产接线、测试/oracle/harness 和依赖；复用有效逐任务/批次审查。
- T016：全部实现完成后，执行一次最终完整 C++ unit suite → integration/process → MiniNDN/no-Python → Python wrapper checks 及既定检错用例，核对同源证据和最终 diff。
- T017：复用 T016 结果交接；SIF/Tiger 留实验机器。

## One Completion Record

逐任务调用独立官方 `$review-agent`，使用 [项目调用规则](../../../skills/speckit-code-design/references/review-agent.md) 提供设计与批次上下文。每批只维护一份结果，具体字段复用 [共享记录规则](../../../skills/speckit-code-design/references/pre-test-static-review.md#one-completion-record)。批次关闭前还要按 [Batch Quality Gates](../../../skills/speckit-code-design/references/batch-quality-gates.md#batch-retrospective) 分别记录 static、compile/link、runtime/test 与 unobserved 漏检；缺项保持 `PARTIAL`。保留此 anchor 供现有 proof-design 与校验器引用。

最小具名诊断仅用于静态无法解除的具体阻塞，范围和原因先记录；例外、失败重试规则统一见 [Exceptions And Retry](../../../skills/speckit-code-design/references/pre-test-static-review.md#exceptions-and-retry)，不降低正式验收。

## Acceptance

Static review PASS != Behavior PASS。

小任务静态通过但批次测试未运行，状态 PARTIAL、保持 [ ]，记录 STATIC_PASS / TESTS_DEFERRED 与批次。
相关实现、审查和全部单元验收实际完成才 [x]；完整 PO 和 feature 验收仍以 T016 实际结果为准。负例、counterfactual 和真实运行要求不降低；诊断例外、失败保存、证据复用均遵循通用规则。
