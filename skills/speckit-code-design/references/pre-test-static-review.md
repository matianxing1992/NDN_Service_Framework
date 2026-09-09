# Source Review And Runtime Validation

## Logical Batch

默认：设计就绪 → 确定逻辑批次 → 逐任务编码及只读静态审查 → 整批逻辑与流程审查 → 统一构建和相关测试 → 批次验收。不得因为一个小改动或下一任务开始而自动重复构建、测试。
批次覆盖、稳定出口和结果记录字段见 [batch-quality-gates.md](batch-quality-gates.md)，本节与该参考共同构成共享执行规则。

在 plan/tasks 一处登记 Batch ID、成员任务、行为边界、依赖、共享构建/测试选择器及负责人。围绕同一调用链、状态机或接口迁移分批；不按文件数机械分批，也不默认把整个 Spec 作为一批。每批须能独立构建并验证连贯成果，设计缺口先解决。

区分 implementation dependency 与 acceptance dependency：同批前项静态通过后，只有明确登记的实现依赖可继续；要求已测试、ABI 验证或正式资格的硬前置仍须实际满足。既有 Depends 不自动降级。批次边界变化先更新计划与受影响审查范围。

## Per-Task Static Gate

1. 实现一个可审阅的小任务，同时编写或调整必要测试、注册与调用方。
2. 每个小任务明确加载独立官方 `$review-agent`，按 [调用及项目补充规则](review-agent.md) 只读审查完整差异、足够上下文和设计契约。无需每次启动多个代理。
3. 在同一审查结果中填写 [batch-quality-gates.md](batch-quality-gates.md) 的 Coverage matrix：逐项核对真实生产入口/调用方、实现与 wire、测试/harness/oracle、构建注册/source closure、迁移/证据路径；不适用项写理由，无法确认项写 `gap`。至少给出实际文件/符号和查询或检查命令，不能用“目录已看”代替。
4. 发现具体缺陷或必要设计/接线遗漏，由实现者修复，再重审受影响范围；不能带着已知控制性缺陷继续依赖它的任务。
5. 覆盖充分且没有控制性问题，记录 `STATIC_PASS / TESTS_DEFERRED / Batch ID`，并在同一结果
   记录中写入 `Review trace`（skill 路径/SHA、基线、diff 范围、查询和复审结论）；继续同批
   下一任务，不启动常规构建/测试。状态仍为 PARTIAL，保持未勾选并登记待执行选择器。

静态无法确认的运行假设留给批次测试；缺少明确接口、必要源码或关键路径覆盖时不能写 STATIC_PASS。No findings 不自动表示审查充分或验收完成。

## Batch Static Gate And Tests

整批成员编码及逐任务静态门完成后，审查整批完整差异和组合流程：入口→校验→状态更新→副作用→回调/终态→清理。核对跨任务 API、字段、错误、身份和并发约束一致；生产调用方、默认注册、构建配置、序列化、迁移删除、测试/harness 均已接线，无占位实现或遗漏路径。批末必须更新同一份 Coverage matrix，明确成员新增或未覆盖的 lane；漏掉调用方、测试注册或 source closure 属于 coverage gap，不能以 No findings 放行。

因此，只有阅读了真实 caller/default wiring、测试注册及 harness/oracle、构建 target/source
closure 后，官方 review-agent 的 `No findings` 才能成为静态门结果；只审生产函数或只看
测试文件的 clean review 必须记为 coverage gap。

无已知控制性缺陷且验证命令、独立判据和必要负例明确，记录 `READY_FOR_BATCH_TESTS`，并填写
批末 `Review trace` 与 `Closure decision: CLOSED_FOR_VALIDATION`，再统一执行必要构建、相关
单测及计划内静态工具。共享构建和重叠选择器合并执行，结果逐项映射成员；不按任务数重复命令。
若尚未达到稳定出口，必须记录 `OPEN_FOR_NEXT_BATCH` 及触发条件，不得以 READY 或共享构建掩盖缺口。
不得把后续批次全部写完才测试当前已闭合批次。

全部实现、调用方迁移和测试/harness 编写完成后，补审最终跨批接线，再执行约定的 final unit regression → integration → system/MiniNDN → feature acceptance。跨组件/跨进程用例随实现编写，默认留最终阶段运行；不能重命名为 unit/smoke 提前执行或由静态扫描替代。

## Exceptions And Retry

仅在具体实现阻塞必须运行才能判断时，允许批内最小具名诊断，例如 ABI、模板实例化、链接或生成代码不确定性。先记录阻塞、静态不足的原因、命令和范围；不恢复默认逐任务构建或启动完整集成/实验。廉价链接、语法、格式检查可随修改执行；需要编译数据库或构建的分析工具如实计入构建成本。

明确要求的 TDD RED / counterfactual 保留：先审查检测路径及预期失败，运行具名受控缺陷用例，再修复、重审；RED 不算验收。不因风格或无证据猜测反复重构。

失败时保留原始结果并识别首个失败边界，重读相关源码，修复后重审和重跑受影响范围。启动失败与协议结果分开。源码、配置、依赖变化只使受影响证据失效；既有有效 PASS 可复用。后续测试层级无新变化时无需重复全量审查。

若失败属于编译/链接或运行/测试漏检，重试前必须在同一结果记录中链接首个失败证据，
并写明新增或改变的静态检查及其覆盖 lane；重复同类漏检时先更新共享 skill、模板或
checklist，或记录明确的替代门禁。只重跑原命令不构成漏检修复，也不能提升任务状态。

## One Completion Record

每批复用 tasks.md 或一份 evidence：记录源码基线、任务/整批差异边界、Coverage matrix、成员静态覆盖与 findings/修复、
`Review trace`、`Closure decision`、编译/链接漏检、运行/测试漏检、批次流程结论、实际命令/target/source closure/`-j`/elapsed/退出码/日志、
未执行项及下一步。每任务只需一行引用；不增加每小段一个报告或行政审查任务。字段定义见 [batch-quality-gates.md](batch-quality-gates.md)。

批次关闭前还要完成该 reference 的 `Batch Retrospective`，把静态提前发现、编译/链接才
发现、运行/测试才发现和仍未观测风险分开记录；未完成分类时保持 `PARTIAL`，不因局部
测试通过而升级。

`STATIC_PASS`、`READY_FOR_BATCH_TESTS` 是证据标记，不新增进度状态。测试待运行使用 PARTIAL；失败/阻塞如实记录，不能据静态通过勾选。该任务全部验收实际通过才 DONE/[x]；最终集成/实验义务须有明确负责的验证任务。阶段性交接可记录静态进展，checkpoint 遵守仓库规定。

## Acceptance

Static review PASS != Behavior PASS。

整体验收要求符合设计、无未解决控制性缺陷，且约定构建、unit、integration、system/MiniNDN 和必要负例全部通过。文档检查不能证明源码逻辑或运行行为；不追溯性改变冻结证据或降低验收要求。
