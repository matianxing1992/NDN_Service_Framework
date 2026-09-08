# Batch Quality Gates

本参考规定 Spec Kit 在设计、拆任务、执行和复盘时如何判断一个逻辑批次
是否真正闭合。它补充 [pre-test-static-review.md](pre-test-static-review.md)
的执行顺序；不把静态审查、编译成功或局部测试改写成产品验收。

## Coverage Contract

每个小任务的只读静态门，以及每批的组合审查，都必须按实际影响范围覆盖。纯文档
或不涉及某项的任务记录 `N/A` 及理由，不把不适用的代码门禁强加给它：

- 生产入口、真实调用方、默认工厂/注册和有效配置；
- 被修改的实现、头文件/导出、序列化或 wire 代码；
- 测试和 harness 的 fixture、oracle、负例及测试注册；
- 构建脚本、target/link/source closure、生成文件和安装/打包入口；
- 迁移、兼容路径、旧路径退出和证据采集路径（适用时）。

设计阶段可以把尚未存在的路径标为 `planned`，但实现批次的静态门必须给出
具体文件/符号或明确的 coverage gap。漏读真实调用方、测试接线或构建注册时，
不能写 `STATIC_PASS`；No findings 也不能替代覆盖说明。

每次静态门必须留下一个简短的 **Coverage matrix**（可放在同一份批次 evidence
中，不为每个小任务另建报告）。矩阵至少列出以下五个 lane，并为每个 lane 标记
`covered`、`N/A`（附理由）或 `gap`，同时给出实际文件/符号和查询或检查命令：
`production entry/callers`、`implementation and wire`、`test/harness/oracle`、
`build/source closure`、`migration/evidence`。批末矩阵还要说明新增成员是否扩大了
覆盖范围。没有矩阵、只有泛称目录，或矩阵与实际差异不符，均属于 coverage gap，
不能记录 `STATIC_PASS` 或 `READY_FOR_BATCH_TESTS`。

## Stable Batch Exit

逻辑批次以行为边界结束，而不是以文件数或任务数结束。批次可以继续加入成员，
仅当它们共享同一行为出口、负责人、实现依赖和构建/测试选择器，且没有引入新的
验收依赖。满足下面条件时应停止扩张并验证：

1. 接口、状态、所有权和错误契约已稳定；
2. 生产调用路径及必要接线已包含在批次范围；
3. 有独立可观察结果、独立 oracle 和必要负例；
4. 逐任务静态门及整批组合审查均无控制性缺陷；
5. 构建范围、测试选择器、负责人和未执行的硬前置已登记。

尚未达到独立出口的组件可以留在同批，但必须在 `Evidence / Remaining` 中说明
缺口。不能为了减少一次编译无限加入新的职责；已经闭合的批次应及时执行批末验证。

## Batch Result Record

每批只维护一份 tasks/evidence 结果记录。记录以下字段；没有发现时写 `none`
或 `not observed`，不能留空：

| Field | Required content |
| --- | --- |
| `Coverage matrix` | 五个 coverage lane 的 `covered`/`N/A`/`gap`、实际文件/符号、查询或检查命令；批末注明新增范围 |
| `Static findings` | 静态门实际发现、修复、复审范围；包括 coverage/design gap |
| `Compile/build misses` | 编译器、链接器或构建接线发现而静态门未发现的问题 |
| `Runtime/test misses` | 运行或测试才发现而静态/构建未发现的问题；注明首个边界 |
| `Build measurement` | 精确命令/target、源码或构建边界、toolchain、`-j`、elapsed、exit code、日志 |
| `Behavior result` | 成员逐项映射到 `STATIC_PASS`、`BUILD_PASS`、`FOCUSED_BEHAVIOR_PASS` 或 `QUALIFICATION_PASS` |
| `Evidence / remaining` | 持久证据链接、未执行项、硬验收依赖和下一步 |

这些标记是证据标签，不是任务状态。测试未执行或仅局部通过时保持
`PARTIAL`；`QUALIFICATION_PASS` 只能由计划指定的完整资格门产生。

## Efficiency Measurement

构建耗时只能在相同 target/source closure、toolchain、配置和工作树条件下作对照。
一次更快或更慢的运行只记录为观测，不推导总体提速；批次记录应保留失败尝试及其
首个边界，便于比较“静态提前发现”与“编译/运行漏检”，而不是只统计通过数量。

## Skill Responsibilities

| Skill | Required use of this reference |
| --- | --- |
| `speckit-specify` | 为每个故事/FR/SC 写明真实入口或参与者、可观察结果、独立判据、必要负例/恢复路径；未知项保持显式 |
| `speckit-plan` | 为每个逻辑批次冻结行为边界、成员、实现/验收依赖、共享选择器、负责人和稳定出口 |
| `speckit-tasks` | 生成批次表、逐单元 `Execution Progress` 和结果记录字段；任务必须覆盖实现、调用方、测试/harness、构建接线的适用范围 |
| `speckit-analyze` | 把缺少入口、调用方、测试注册、构建注册、独立 oracle 或批次出口列为一致性发现；保持只读 |
| `speckit-audit` | 在代码现实和验证设计维度检查上述覆盖，并区分静态、构建、运行与资格证据 |
| `speckit-implement` | 小任务静态门后继续同批；稳定出口后执行批末组合审查、统一构建/测试并填写全部结果字段 |
| `speckit-converge` | 对生产接线、测试/harness、构建 closure 的遗漏追加有独立出口的修复任务，不用大而泛的补漏卡掩盖边界 |
| `speckit-checklist` | 把入口、可观察结果、独立 oracle、负例、依赖和可测指标作为需求质量检查项，而不是检查实现是否运行 |
