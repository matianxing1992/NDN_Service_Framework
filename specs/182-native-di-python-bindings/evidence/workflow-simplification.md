# Workflow Simplification

**Date**: 2026-09-06 | **Revision**: 6 | **Scope**: documentation only

## Changes And Review

执行规则合并到contracts/pre-test-static-review.md。实现任务按实现、静态审查、相关unit和必要构建完成；全部实现后T015补审整体接线，T016统一运行完整unit、integration、MiniNDN/no-Python与既定负例。
删除固定风险数量、独立前后审查报告和逐层放行表，保留类/方法/字段契约与实际运行验收。unit与integration按真实调用边界分开，测试/harness随实现编写注册。实现任务完成不计整个PO或feature PASS。

## Checks And Evidence

比较基线为81e0f7065462db599af1954d2cd99e8be6e8015d；本轮仅修改设计工作流，不重新判断合并实现状态。

- `python3 specs/182-native-di-python-bindings/checklists/validate_design.py`：PASS；19 FR、11 SC、14 CD、16 PO、17任务、21类/模块、48方法、12类型137字段，107本地链接，依赖无环；实现任务0/17。
- 严格文档结构检查：PASS；最终补回简短Constitution Check，保留架构/安全与真实运行要求。
- 对基线执行文本比较：PO-001--014的完整条目、Negative Path Matrix、Runtime Without Python、Acceptance Cases全部逐字未变；symbol-design与value-contracts的全部表格未变。
- 技能检查：13个技能/引用文件、12个相对链接检查PASS；固定五风险、逐层放行表等旧强制字段已移除。
- `git diff --check -- specs/182-native-di-python-bindings`：PASS。人工核对任务验收范围与T016集中执行的义务一致，集成文件保留并补充独立unit入口。

plan/tasks/audit/checklist/work-units/review六份流程文档由1055行缩减为507行；技术表格与冻结运行用例保留。当前结构检查器统一为checklists/validate_design.py；旧revision检查器可从对应历史提交恢复，旧证据文件不改写。

产品构建、产品源码静态审查与unit/integration/MiniNDN均NOT_RUN；以上PASS仅为文档检查。

## Outcome And Next

实现0/17，O-001--005保留；下一步T001核对稳定合并基线、关闭设计缺口并冻结unit/integration selectors。
