# Implementation Boundary And Proof Contract

## Coherent Work Unit

一项任务完成一个能独立审阅和验证的成果，避免按文件机械拆分。
公共规则只引用，不逐任务复制。

| Item | Required content |
| --- | --- |
| Outcome / dependencies | 完成后的行为；实际前置任务与尚未关闭的设计缺口 |
| Scope / design | 具体文件或符号、增改删移及目的；引用 CD/接口/字段契约 |
| Batch | 逻辑批次 ID、成员与行为边界；写明共同入口/调用方、契约、oracle/selector、source closure 和验收出口；区分 implementation / acceptance dependency；共享构建/测试选择器及负责人只定义一次 |
| Constraints | 该任务特有的架构边界、兼容/删除路径和恢复要求 |
| Verification | 需求/PO、真实入口、独立判据、必要负例及具体命令或 planned 工具；适用时包含调用方、测试/harness 与构建注册 |
| Result | tasks.md 中简短结果或一份 evidence 链接；批次记录按 [batch-quality-gates.md](batch-quality-gates.md) 分类静态/编译/运行漏检并记录构建边界与耗时 |

不要求每个任务重复类/字段说明、文档义务、审查规则和报告模板。
行数或文件数可用于估计，不作为批准门槛。
需要 Spark 等有限范围执行者时，按 [bounded executor](bounded-executor.md) 补充执行卡；
将 Read 与 Write 分开，冻结实际分派基线和选择器，不机械增加上层任务。
局部 helper、算法表示等不改变契约的选择可直接决定；
新增公开 API、职责/状态、依赖或行为时先修订受影响设计。

## Behavioral Proof

每个关键成果说明输入、生产调用路径、可观察结果及检错断言。
覆盖适用的失败、重试、重复、寿命和清理路径；明确已有测试与 planned 测试。
独立 oracle 来自协议、需求或参考结果，不能直接复用被测函数制造期望值。若它复算生产
serializer 的 canonical bytes/digest/identity，必须记录字段集合、顺序、规范化和 source
identity 的对照；过期 hardcode 或错误排序应有明确负例。
必要的真实 integration/system/MiniNDN 不得由 mock、导入成功或结构扫描替代。
保留明确要求的 counterfactual；其失败必须来自目标行为断言，而非启动/编译错误。
无需为了填表对每个风险再新增一套 mutation。

## Execution And Completion

执行顺序、失败处理和唯一结果记录见
[pre-test-static-review.md](pre-test-static-review.md)。
已有审查和测试证据仍适用于当前源码及运行条件时可以复用。
变化时说明影响到的 PO/调用方及需要重跑的范围，不默认重跑全部测试。
小任务编码后逐项静态审查，同批实现依赖满足后继续编码；批末审查组合流程并统一构建/测试。测试待运行保持 PARTIAL，验收依赖不能被静态通过替代；尚待真实集成/实验的 PO 明确转交最终验证任务。
最终验证任务保留全部真实运行要求，通过后才声明 feature 验收；不把局部完成写成完整 PO PASS。
