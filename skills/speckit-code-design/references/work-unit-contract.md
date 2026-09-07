# Implementation Boundary And Proof Contract

## Coherent Work Unit

一项任务完成一个能独立审阅和验证的成果，避免按文件机械拆分。
公共规则只引用，不逐任务复制。

| Item | Required content |
| --- | --- |
| Outcome / dependencies | 完成后的行为；实际前置任务与尚未关闭的设计缺口 |
| Scope / design | 具体文件或符号、增改删移及目的；引用 CD/接口/字段契约 |
| Constraints | 该任务特有的架构边界、兼容/删除路径和恢复要求 |
| Verification | 需求/PO、真实入口、独立判据、必要负例及具体命令或 planned 工具 |
| Result | tasks.md 中简短结果或一份 evidence 链接 |

不要求每个任务重复类/字段说明、文档义务、审查规则和报告模板。
行数或文件数可用于估计，不作为批准门槛。
局部 helper、算法表示等不改变契约的选择可直接决定；
新增公开 API、职责/状态、依赖或行为时先修订受影响设计。

## Behavioral Proof

每个关键成果说明输入、生产调用路径、可观察结果及检错断言。
覆盖适用的失败、重试、重复、寿命和清理路径；明确已有测试与 planned 测试。
独立 oracle 来自协议、需求或参考结果，不能直接复用被测函数制造期望值。
必要的真实 integration/system/MiniNDN 不得由 mock、导入成功或结构扫描替代。
保留明确要求的 counterfactual；其失败必须来自目标行为断言，而非启动/编译错误。
无需为了填表对每个风险再新增一套 mutation。

## Execution And Completion

执行顺序、失败处理和唯一结果记录见
[pre-test-static-review.md](pre-test-static-review.md)。
已有审查和测试证据仍适用于当前源码及运行条件时可以复用。
变化时说明影响到的 PO/调用方及需要重跑的范围，不默认重跑全部测试。
实现任务按实现、静态审查、相关单测验收；尚待真实集成/实验的 PO 明确转交最终验证任务。
最终验证任务保留全部真实运行要求，通过后才声明 feature 验收；不把局部完成写成完整 PO PASS。
