---
name: speckit-code-design
description: Define reviewable code design and coherent implementation tasks, then review source against design before meaningful runtime validation. Keep one concise completion record and reuse contracts instead of duplicating rules.
---

# Spec Kit Code Design

## Repository Context

在目标仓库工作目录运行 `git rev-parse --show-toplevel` 确定 repo root。
仓库文件路径从该 root 解析；本 skill 的 `references/` 链接从本目录解析，安装到个人 skills 后仍适用。
读取 root 的 `AGENTS.md`、`.specify/feature.json`、`.specify/memory/constitution.md`
和 `docs/architecture-reading-guide.md`；活动 feature 以指针及当前用户授权为准，不固定旧 Spec。

## Outcome

让接手者知道改什么、为什么、怎么改、如何调用、怎样验收。设计覆盖受影响的接口、职责、重要字段与状态；不预写普通局部变量或每行实现。

## Scope

遵守用户当前授权。仅设计或审查时不启动实现、构建或实验；不自动回填旧 Spec。
先读仓库指令、constitution、活动 feature、现有设计和相关架构证据。
代码设计章节允许必要实现细节，但用户成果与可观察验收仍是依据。
本 skill 是所有 Spec Kit 入口的共同规则来源；新 Spec 使用的需求、计划、任务和
审查 skill 必须引用同一套 references，不在单个命令中另建批次或测试状态定义。

## Single Source Of Truth

| Artifact | Responsibility |
| --- | --- |
| spec.md | 目标、范围、FR/SC、关键决策和设计索引 |
| contracts / data-model.md | 接口、状态、调用关系和约束的唯一详细定义 |
| plan.md | 阶段、依赖、迁移、环境与验证顺序 |
| tasks.md | 连贯工作单元、依赖、契约引用和当前进度 |
| evidence | 实际命令、结果与必要日志链接；短记录可直接放 tasks.md |

同一规则定义一次，其余文档引用。不要把契约、审查表和验收清单逐任务复制。

## Design Before Coding

1. 核对当前源码、调用方、构建入口和旧实现。区分 existing、planned 与未知事实。
2. 明确成果、架构所有权、兼容/退出路径；优先复用共享 API。
3. 用 [symbol-contract.md](references/symbol-contract.md) 描述受影响类、方法、重要字段：
   责任、输入来源、返回/失败、所有权/寿命、并发、使用示例和必要英文注释。
   已有契约通过 ID 引用；纯局部编码细节由实现者决定。
4. 用 [design-template.md](references/design-template.md) 按需要组织设计；不适用的章节省略。
5. 用 [work-unit-contract.md](references/work-unit-contract.md) 拆成可独立审阅和验证的工作单元。
   明确真实入口、可观察结果、独立判据和必要负例，不以测试数量代替行为。
6. 用 [review-gate.md](references/review-gate.md) 检查设计是否可实施。
   未知项只阻塞受影响范围，不能以文档结构检查 PASS 宣称整个设计就绪。

## Implementation And Validation

所有执行者必须使用 [task progress registry](references/task-progress.md)：
tasks.md 顶部完整登记执行单元、依赖、状态、证据和剩余项，每个工作单元结束后同步。
此规则不取决于模型或 bounded-executor 模式；详细卡片与当前进度分工明确。

用户要求 Spark 或有限范围执行时，应用 [bounded executor](references/bounded-executor.md)：
设计者冻结决策，按行为提供定向阅读/精确写入范围/检查命令，执行者一次完成一张就绪卡。
已有上层任务与正式验收保持；执行卡不自动表示设计已就绪。

唯一执行规则见 [pre-test-static-review.md](references/pre-test-static-review.md)。
每个小任务编码后应用只读 review-agent profile 静态门，通过后继续同一逻辑批次；整批逻辑/流程审查通过后统一构建和相关测试。批次测试未完成保持 PARTIAL；集成与真实实验沿用最终验证阶段。
Static review PASS != Behavior PASS。

批次覆盖、稳定出口、漏检分类和构建测量统一遵循
[batch-quality-gates.md](references/batch-quality-gates.md)。该参考由所有 Spec Kit
写作、分析、审计和执行技能引用；不要在单个 Spec 或模型专用表中另建一套规则。
编码前必须记录每批的分配依据（共同入口/调用方、契约、oracle/selector、source
closure、验收出口）；任一项不一致就拆成新的批次，不以少一次构建为合批理由。
每个静态门和批末门还必须留下五 lane Coverage matrix；没有实际文件/符号和查询
命令的覆盖声称不能产生 `STATIC_PASS`。
矩阵格式统一采用 [review-agent.md](references/review-agent.md) 的 **Minimum Review Record**；
测试 lane 必须同时检查 harness/oracle 与测试注册，build lane 必须检查
target/source closure。缺少任一项时先记 `gap`，不得用 `No findings` 补齐。
build lane 还必须记录实际输出路径、source identity 和 artifact digest；runner/qualification
manifest 必须从该实际输出重生成并核对 digest。
在写 `STATIC_PASS` 前还要完成该 reference 的 **Static Gate Release Checklist**，并在
重试编译/运行漏检时记录 `Changed gate`；只重跑原命令不能关闭漏检。
测试 helper 或 fixture 若复算 production serializer 的 canonical bytes/digest/identity，
还必须对照生产字段集合、顺序、规范化和 source identity；对照不完整时保持 `gap`。
批次已经有独立可观察出口后应立即进入批末验证，不得为了少一次构建继续吸收无关职责；
复盘必须把静态、编译/链接、运行/测试漏检和可比构建耗时分开记录；每个成员和批末组合门
还要记录 review-agent 的路径、SHA、基线和实际差异范围，以及 `CLOSED_FOR_VALIDATION` /
`OPEN_FOR_NEXT_BATCH` 的关闭决定和触发条件。
批末记录必须包含四类 **Batch Retrospective**（`static`、`compile/link`、`runtime/test`、
`unobserved`）及其首个失败边界；缺少分类时保持 `PARTIAL`。
各 Spec Kit 入口还必须遵守该 reference 的 **Command Output Contract**：编辑前登记
`Batch growth decision`，审查时留下五 lane 的实际查询，批末在同一结果记录中写完整字段，
重试时登记 `Changed gate`。入口 skill 只引用规则而未生成这些字段时，不得写入
`STATIC_PASS` 或 `DONE`；技能副本同步本身不构成执行证据。
编译/链接或运行/测试漏检在重试前必须链接原始边界并登记改变的静态检查；同类漏检再次
出现时先修订共享 skill、模板或 checklist，或在证据中说明替代门禁。只重跑原命令不能
关闭漏检，单次构建耗时也不能证明流程提效。
维护中的 legacy/compatibility 回归同样必须经过真实调用方和迁移 lane：当前源码探针
发现的运行时失败要保留首个边界、原始日志和 `PARTIAL` 状态，并交给
`speckit-converge` 追加有独立出口的修复/迁移任务；在 receipt、newness、状态或兼容契约
边界明确前，不直接改共享 freshness、重试或发布逻辑。
NDNSF-DI 原生运行时和协议行为的 unit/integration/regression 测试，其断言主体、
fixture/driver 与 oracle 必须用 C++ 实现并直接调用生产 C++ target；Python 可以编排
外部设施、启动 C++ 测试 executable，或证明绑定/兼容/离线 oracle 边界，但不能替代
C++ 行为、C++/Python parity 或跨进程资格测试。
CLI `--help`、usage/schema rejection、target/link smoke 或 harness 启动只证明接线边界，
不能写成 native request/result 或 qualification PASS；没有真实生产请求与独立结果时保持
对应 production/qualification lane 的 `PARTIAL` 或 `gap`。

需求成形（`speckit-specify`、`speckit-clarify`、`speckit-checklist`）、计划与任务生成
（`speckit-plan`、`speckit-tasks`）、代码现实分析（`speckit-analyze`、`speckit-audit`、
`speckit-converge`）以及执行（`speckit-implement`）都必须使用这份 reference。
`speckit-constitution` 修改治理规则时要同步模板和依赖 skill；`speckit-taskstoissues`
只能转换已有行为任务，不能把审查、构建或证据记录拆成无行为出口的行政 issue。

接口、职责、状态或验收改变时先修订对应契约；普通局部实现选择无需新报告或批准。
完成时核对最终 diff 和实际证据，同步 tasks.md。未完成的实现/验证保持未勾选。
仅重审、重跑变化影响的范围；不能自动把旧 PASS 延续到新行为。

## Deliverable

返回修改路径、实际检查结果、未决事项和下一步。报告简短，详细证据只保存一份。
遵守仓库 checkpoint 规则，隔离预存改动。
