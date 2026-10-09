# Implementation Plan: DistributedRepo Convergence

**Branch**: `Repo` | **Updated**: 2026-10-08 | **Spec**: [spec.md](spec.md)
**Status**: IMPLEMENTATION_IN_PROGRESS; qualification incomplete

实现现有native Repo的已接受目标，修复实际缺陷，完成全部资格。Spec192/193保持停止。当前状态唯一维护在[tasks.md](tasks.md)，差距在[gap-audit.md](gap-audit.md)；不在本计划复制实时任务状态。

## Owners and Constraints

| Owner | Work |
| --- | --- |
| Native Repo | API/store/worker/importer及其真实C++回归；保持publicABI/schema及mode边界 |
| Core | 现有grant/request/response安全事实；不为fixture修改授权或SVS时间 |
| Binding/DI | native typed projection、strong owner、真实跨语言/授权复用；无Python业务状态机 |
| Build/runtime | 真实源码、SDK headers/library、产物和profile闭包；实际加载哈希及mutation拒绝 |
| Harness/oracle | 共享192/193文件名保留；独立storage/wire/consumer/cleanup证据 |

本机Ubuntu20.04、C++17、system Boost1.71；单build tree、最多j4，当前资源敏感构建用j1/j2。无Tiger/SIF、模型benchmark或push。保留用户overlay及原始失败证据，不以HEAD或旧binary声称当前候选可复现。

## Execution Order

1. T001：固定现状、入口与owner，复用现有API清单和验证矩阵。
2. T002–T004：localized destination/ledger/import修复，真实red→patch→closing；不扩展公共接口。
3. T005–T008：核对metadata/object/oracle/binding/DI；已正确实现直接跑conformance，发现违约才修正确owner。
4. T009：收口真实source/build/SDK/profile，重建受影响consumer，完成native/Python/CLI及适用dynamic，fresh convergence关闭controlling gaps。
5. T010：同一冻结common-source与声明profile完成59case全部子场景两轮、cross-run持久性和必要复现说明。

## Review and Validation

详细行为只由[architecture-and-api](contracts/architecture-and-api.md)维护；候选、动态和失效规则只由[candidate-and-validation](contracts/candidate-and-validation.md)维护。static review先于受影响运行；未解决HIGH允许focused诊断/修复，禁止完整资格。静态PASS不替代行为或动态PASS；未观测项不计完成。

执行Spec的[文档预算](spec.md#implementation-and-documentation-budget)：不新增batch/report/state文件；已有contract覆盖时直接实现。审查五lane在固定快照执行，结果及必要dynamic参数放既有evidence JSON/log，tasks只维护一行状态与链接，不复制模板和retrospective。既有batch文档是历史参考，不再形成并行状态入口。

## Rollback and Completion

按owner显式路径checkpoint，禁止broad staging；不自动push。恢复失败保留可定位材料；import重试不删除已完整发布目标；不丢用户source或原始证据。无需求不新增ABI/schema、全局事务、Python catalog或隐式replication。

完成条件唯一见Spec成功标准及validation-matrix；行政文档、局部PASS或单轮成功均不能关闭任务。
