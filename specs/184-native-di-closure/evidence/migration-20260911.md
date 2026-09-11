# Spec184 Migration Record

**Status**: CLOSED_FOR_VALIDATION (documentation only) / product NOT_RUN | **Date**: 2026-09-11
**Source checkpoint**: `94c1e644`；[transfer matrix](../contracts/transfer-matrix.md)。

## Decision and Scope

用户要求将182未完成部分拆到184，避免旧 Spec 继续膨胀。本单元仅新建精简文档、迁移执行归属、
更新 active feature 和 Design 记录；产品无修改，旧 checkbox/原始证据保持。
原有 failure-log 和 integration fixture 修改属并行工作，未纳入本提交。
已核对最新失败为错误 Waf tree/trace/marker/fixture 首边界，未重试运行。

## Review Trace

沿用刚完成请求链审计的 source baseline、独立 review-agent 只读结论，未重复声称产品静态门通过。
本轮文档审查范围为新184文件、182移交标记、feature pointer、Design ledger；核对14/14未完成父任务、
5/5审计批次、3个已完成 checkbox、6 FR/5 SC/8新任务及继承验收语义。
无 API/源码变化，不需要为拆分执行 CodeGraph 全库扫描、编译或实验。

## Coverage Matrix

| Lane | Result |
| --- | --- |
| Production entry/callers | 引用当前审计与 spec evidence 表；无入口变化 |
| Implementation/wire | N/A 文档迁移不改产品；四项 finding 保持 OPEN |
| Test/harness/oracle | 原 C++ 证据/负例义务继承；B1–B4 当前 selector 已有 focused 记录，未运行项继续保持 OPEN/PARTIAL |
| Build/source closure | N/A 本轮不构建；旧 build 首失败保留 |
| Migration/evidence | 14个 OPEN 父任务和5批映射；旧历史冻结，新状态单点维护 |

## Closure Decision

产品 OPEN_FOR_NEXT_BATCH，稳定退出是迁移无丢项、指针指向184、文档结构/链接一致。
下一步 B1/T001；Batch growth decision 为拆分调度而非新增产品范围。
Miss retrospective：本轮仅文档 `static` 检查，compile-link/runtime-test NOT_RUN。
实际校验结果在本文件 Validation 段记录，不能据此勾选新实现任务。

## Validation

- 严格结构检查最终 PASS：6 FR、5 SC、3 stories、8 tasks，完成0；6 FR 全部追踪。
- 首轮检查将编号说明里的泛化 T 编号误判为模板占位符，并因中文“测试”提示缺 validation；
  改为实际编号例子、明确英文 Validation 标题后复验通过。没有产品测试失败。
- 原 OPEN 任务集合与迁移表集合相等：14/14；原17个 checkbox 全部保持原值，其中3个已完成。
- 新文档全部本地链接目标存在；`git diff --check` PASS；workflow entrypoint 11/11 PASS。
- `.specify/feature.json` 与本机 AGENTS managed plan pointer 已指184；没有必需的 extension hook。
  可选 agent-context 更新使用已配置的 AGENTS.md 指针手动同步；该本机文件不纳入 Git。
- authority index 重建后 project/active health 均 `ok=true`，当时 active feature 为 `specs/184-native-di-closure`；该结果是迁移时记录，当前 active health 仍须按 guard 重新核对。
- 未改产品代码、未编译/运行测试、未启动外部实验。原 failure-log 与 integration fixture 修改保持未提交。
