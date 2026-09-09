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

## Batch Allocation Decision

在编码前，plan/tasks 必须为每个批次写出可核对的分配依据：成员共享同一生产入口
或调用方、同一接口/状态/所有权或数据契约、同一独立 oracle 与测试 selector、同一
源码/构建 closure，以及同一验收出口。每个成员至少映射到这五项中的实际文件、符号
和命令；只因文件相邻、同属一个 user story 或可以共用一次编译，不足以进入同批。

若成员引入不同调用方、不同状态机或错误边界、不同 C++ selector、不同构建 target，
或需要另一项硬验收依赖，必须先关闭当前批次并登记新的 Batch ID。执行中发现分配依据
不成立时，保留已完成证据，记录拆分触发点，不把未接线的组件继续堆入当前批次。

## Native Test Ownership

对于 NDNSF-DI 的原生运行时、协议、状态机、并发、密码和模型行为，验收测试必须
直接调用生产 C++ target，并登记真实的 C++ suite/selector。Python 测试可以覆盖
pybind API 形状、facade 转发、离线 oracle、配置拒绝和外部设施适配，但不能以
Python focused test 的通过替代 native behavior、C++/Python parity 或跨进程资格。
若某个 native requirement 只有 Python 测试或没有 C++ target/selector，Coverage
matrix 的 `test/harness/oracle` lane 必须写 `gap`，对应任务保持 `PARTIAL`。

当 Python extension 链接到仓库内的 native shared target 时，native 源码变化必须
先重建该 shared target，再重建 extension；仅重编译或重链接 extension 不能证明
source/link closure。批次记录应检查依赖库中包含变更符号（或等价的 source/hash
身份），并把 stale-library 导致的导入或未定义符号列入 `Compile/build misses`。

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

批次还必须附带以下两项可追溯信息；它们不能只由“审查通过”或“批次完成”一句话代替：

| Field | Required content |
| --- | --- |
| `Review trace` | 每个成员的 review-agent skill 路径及 SHA-256、审查基线 commit、实际 diff 范围、覆盖查询、findings 与复审结果；批末组合审查同样记录基线和范围 |
| `Closure decision` | `CLOSED_FOR_VALIDATION` 或 `OPEN_FOR_NEXT_BATCH`、达到或未达到的稳定行为出口、触发观察、未纳入成员及下一批 ID/依赖；没有独立出口时不得启动共享构建 |

`STATIC_PASS` 只有在 `Review trace` 真实存在且所声明静态审查范围没有未解释的 `gap` 时才有效。
尚未执行的真实运行、跨进程或资格验收可以在矩阵中保留明确的 `gap`，但必须使批次保持
`PARTIAL`，不能把该 gap 包装成静态或行为通过。
`READY_FOR_BATCH_TESTS` 还要求批末组合审查和 `Closure decision` 均已记录；技能文件、
模板或任务框本身的存在不构成审查执行证据。

这些标记是证据标签，不是任务状态。测试未执行或仅局部通过时保持
`PARTIAL`；`QUALIFICATION_PASS` 只能由计划指定的完整资格门产生。

## Efficiency Measurement

构建耗时只能在相同 target/source closure、toolchain、配置和工作树条件下作对照。
一次更快或更慢的运行只记录为观测，不推导总体提速；批次记录应保留失败尝试及其
首个边界，便于比较“静态提前发现”与“编译/运行漏检”，而不是只统计通过数量。

批次粒度也必须以可观察行为为依据。若一批已经具备稳定出口、独立 oracle 和完整
接线，继续加入无关职责只为少启动一次构建属于批次膨胀；应先执行该批验证，再登记
下一批。复盘时至少分别统计静态门提前发现、编译/链接才发现、运行/测试才发现和
尚未观测的风险，并注明首次失败边界。任务数量、静态通过数量或单次构建更快都不能
单独作为流程提效结论。

## Resource-Constrained Builds

构建记录必须采用仓库 `AGENTS.md` 指定的 system-first compiler/linker 和资源预算。
在本项目 6 logical CPUs / 12 GB RAM 的开发机上，默认使用 `-j4`；用 `vmstat 1`（忽略
首行）观察后续样本，若持续 swap-in/out 或桌面停顿，下一次降到 `-j2`。不得让并行
构建争用同一个 Waf tree。`-j`、toolchain、target/source closure、配置和 elapsed 必须
写入批次记录；资源策略本身不是性能提升或资格通过的证据。

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
| `speckit-clarify` | 在需求仍可澄清时补齐入口、oracle、负例/恢复边界和 native C++ selector；不把澄清写成已实现证据 |
| `speckit-constitution` | 修改原则时同步 plan/spec/tasks 模板和相关 skill，保留本 reference 的批次、证据和 native-test 不变量 |
| `speckit-taskstoissues` | 保留任务 ID、行为出口、依赖、selector 和 evidence owner；跳过重复 issue，不创建行政拆分 |
