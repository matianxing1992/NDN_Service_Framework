# Dependency-Scoped Dispatch Revision

**Updated**: 2026-09-12 17:00 -05:00
**Status**: PASS (workflow documentation only)

## Scope

按用户要求，将全局逐任务等待改为按依赖阻塞。共同规则位于
[Dependency-Scoped Dispatch](../../../skills/speckit-code-design/references/pre-test-static-review.md#dependency-scoped-dispatch)，
Spec185 的 plan、tasks、batch-execution 及生成模板同步引用。
个人安装的共同 skill 及两份受影响 reference 与版本化来源同步。

主会话编码与只读 review-agent 子代理可异步重叠；独立任务先登记前置、写入范围、共享owner和独立性。
审查固定不可变快照，包含新增文件及足够上下文；失败阻塞依赖闭包，修复后更新快照并复审。
全部成员静态门与汇合候选组合门仍先于共享构建/测试。没有独立任务时等待，无子代理时如实串行。

## Review and Validation

- `python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py --require-entrypoints --require-personal`：PASS，11/11 本机入口及个人共同 skill 一致。
- `git diff --check`：PASS。
- 对照 HEAD 的 tasks registry 与任务勾选行：完全相同；18任务的状态、Depends、Updated 未改变。
- 文档规则审查：独立且前置满足的任务允许继续；显式静态依赖必须等待；acceptance dependency 未验收不得解锁；共享文件/接口冲突不得假称独立；旧快照不能验收后续修改；全部成员未过门不得批次测试。
- Spec185 当前 T003→T004 等显式链及 B5 的 B4 exit 原样保留。本轮未证明存在可并行任务对，没有宣称执行重叠或速度提升。
- Context Mode 在修改前 project/active health 均 PASS；权威使用实际仓库文件。产品源码审查/CodeGraph/native 构建测试不适用于本次流程文档变更，未执行。

## Design and Next Step

NO_DESIGN_CHANGE：不改API、运行时行为或验收，当前/目标PDF不重建。并行会话的源码、Design API生成物及失败日志修改均不属于本次提交。
下一执行单元先在批次记录中登记合格独立工作；若需要细分任务，先同步任务卡及依赖，再异步派发审查。没有合格工作时保留串行等待。
