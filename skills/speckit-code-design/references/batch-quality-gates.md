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

若 test helper、fixture 或 oracle 会复算 production serializer 生成的 canonical bytes、
digest 或 identity，`test/harness/oracle` lane 还必须逐项对照生产符号的字段集合、字段顺序、
规范化规则和 source identity。独立 oracle 可以不调用被测 serializer，但必须记录这组对照
和一个能暴露过期 hardcode、错误排序或缺失身份字段的检查；不得仅凭测试数量或旧 fixture
摘要写 `covered`。无法完成对照时写 `gap`，批次保持 `PARTIAL`。

维护中的 legacy/compatibility 路径也属于真实调用方和迁移覆盖范围。若当前源码的
探针、回归选择器或旧 oracle 暴露出它在生产接线之后的运行时失败，必须记录首个失败
边界和原始证据，并在 `migration/evidence` lane 保持 `PARTIAL`；`speckit-converge`
应为该边界追加有独立出口的修复或迁移任务。不能因为路径来自旧 Spec、旧测试或非主
调用方就把失败标成“无关”，也不能在没有定位 receipt/newness、状态或兼容契约边界前
直接修改共享 freshness、重试或发布逻辑。涉及多个 publication name 时，静态门还必须
明确检查同一 producer/session 的跨名称乱序、同名重复和旧 session 交错；只有 wire 契约
明确保证全局顺序时才可采用单一 sequence frontier。

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

对于 NDNSF-DI 的原生运行时、协议、状态机、并发、密码和模型行为，相关 unit、
integration 和 regression 测试的断言主体、fixture/driver 与 oracle 必须用 C++ 实现，
直接调用生产 C++ target，并登记真实的 C++ suite/selector。Python 可以编排外部设施、
启动一个 C++ 测试 executable，或覆盖 pybind API 形状、facade 转发、离线 oracle 和
配置拒绝，但 Python focused test 不能实现 native behavior 的主要断言，也不能替代
C++/Python parity 或跨进程资格。
异步 native 测试还必须显式记录外部 `Face`、`io_context`、scheduler、timer service 和
回调对象的 owner 及析构顺序。若生产 executor 可 detached、延迟释放或在回调线程继续
持有 `ServiceUser`，fixture 必须以 RAII/shared owner 保持这些依赖存活，或提供可核对的
join/drain barrier；不能让栈对象在 worker 释放最后一个 production owner 前先析构。静态
门要从 selector 入口检查该 ownership map 和 cleanup path，运行门要对具名 selector 做
重复运行；析构竞态、SIGSEGV 或 UAF 首次出现在 runtime/test 时，必须保留首个 backtrace，
并把 fixture lifetime 作为下一次重试的 `Changed gate`。这项检查只约束测试边界，不授权
为迁就 fixture 改变生产 `close()`、callback 或线程语义。
若某个 native requirement 只有 Python 测试或没有 C++ target/selector，Coverage
matrix 的 `test/harness/oracle` lane 必须写 `gap`，对应任务保持 `PARTIAL`。

## Risk-Based Dynamic Validation

动态分析是批次行为验证的补充，不是静态审查或 C++ 行为测试的替代。工具不会自行判断
参数在业务语义上是否“理想”；任务必须先写出可观察的不变量（例如 IO 线程归属、请求
身份/attempt 绑定、终态后不再回调、pending/turn 创建与清理平衡），再选择适用 profile。
每个 code-backed task 和批次在开始编码前登记 `Risk class`、`Dynamic profile` 与
`Dynamic invariants`；纯文档任务写 `N/A` 及理由。

| Dynamic profile | 适用风险 | 最小验证要求 |
| --- | --- | --- |
| `asan-ubsan` | 生命周期、越界、释放后使用、未定义算术、序列化/损坏输入 | 独立 sanitizer build；具名 C++ selector；零 sanitizer 报告并有界退出 |
| `tsan` | 共享可变状态、跨线程回调、锁/IO owner、取消/替换竞态 | 独立 TSan build；具名 C++ selector 至少重复运行；零 data-race 报告、无死锁/超时 |
| `parser-fuzz` | 不可信 wire、JSON、checkpoint 或边界长度解析 | C++ parser target/seed corpus；记录迭代/时间预算、崩溃和 sanitizer 结果 |
| `none` | 纯文档、静态矩阵或没有运行时状态的工作 | 写明不适用原因；不得把未运行动态分析写成 `DYNAMIC_PASS` |

ASan/UBSan 与 TSan 使用不同的独立构建目录和结果目录，不混用同一套编译产物；profile
应复用已经配置好的 sanitizer tree，避免每个小任务重复全量配置。NDNSF-DI 的断言主体、
fixture/driver 和 oracle 仍必须是 C++，Python 只能编排外部设施或启动 C++ executable。
动态验证通过只产生 `DYNAMIC_PASS` 证据标签，不能单独产生 `FOCUSED_BEHAVIOR_PASS`、
`QUALIFICATION_PASS` 或任务 `DONE`。出现报告、无界等待、SIGSEGV、UAF、竞态或清理残留时，
保留首个 backtrace/日志，记录为 `DYNAMIC_FAIL`，并在下一次重试前登记改变的静态检查或
反事实用例；不得只重复原命令。

### Dynamic Gate Card

动态分析是**批次级**门，不是每个小任务各自追加的行政步骤。批次在
`READY_FOR_BATCH_TESTS` 后、启动 sanitizer/fuzz 前，必须在同一份 evidence 中冻结一张
`Dynamic gate card`：

1. 写明风险到 profile 的理由、共享 build/output 目录、编译器/链接器及依赖身份；成员
   共享同一状态机和 selector 时只建立一棵动态树，不按任务重复构建。
2. 把输入参数分成可变参数、边界值和故意非法值，并为每组参数登记**业务不变量**：
   例如 request/attempt 身份绑定、终态单调性、publish 前后取消规则、IO owner、创建与
   清理计数平衡。Sanitizer 只能发现内存/线程/未定义行为，不能判断参数在业务语义上
   是否“理想”；这部分必须由 C++ fixture/oracle 断言。
3. 绑定实际生产 C++ selector、最大运行时间、重复次数或 fuzz 预算，以及每个 selector
   的预期成功/拒绝/取消结果。Python 若存在，只能启动该 executable 或编排外部设施，
   不得代替上述断言。
4. 记录原始 stdout/stderr、退出码、首个报告位置和 residue/timeout 结果。报告出现时
   先保留 `DYNAMIC_FAIL`，按首个失败边界修正代码、fixture、source closure 或参数表，
   再运行受影响 selector；重复原命令本身不是改变的门禁。
5. 不得用 sanitizer 抑制选项隐藏报告来写 `DYNAMIC_PASS`。若外部库或 ABI 边界产生
   工具报告，先记录未抑制日志并保持 `DYNAMIC_FAIL`/`PARTIAL`；只有改变到可验证的
   ABI 一致 build 边界并在无抑制配置下重跑干净，才可升级为 `DYNAMIC_PASS`。仅为诊断
   而设置的抑制必须同时写明精确选项、受影响库和为何不覆盖生产代码。

这张卡把“动态工具看到了什么”和“业务参数是否满足契约”分开，避免把无报告误写成
协议通过，也避免把动态步骤拆成大量独立任务。批末应按 selector 汇总卡片结果，而不是
按任务数量统计动态通过数。

### Bounded Dynamic Parameter Matrix

每张 `Dynamic gate card` 还要附一张有界参数矩阵。矩阵按行为等价类取样，不要求为每个
字段或每个小任务单独建立动态任务；同一状态机的成员共享矩阵和动态构建。最少覆盖
`nominal`、关键最小/最大边界、故意非法输入，以及会改变生命周期或并发顺序的
`cancel/deadline/replacement` 情况；不适用的类别写明理由。

| caseId | Parameter tuple / boundary | Expected business result | C++ assertion / selector | Tool observation | Status |
| --- | --- | --- | --- | --- | --- |
| D-01 | `[case-specific values]` / `nominal` | `[success or explicit rejection]` | `[production C++ selector + invariant]` | `[sanitizer/thread/fuzz observation]` | `NOT_RUN` |

矩阵必须冻结参数来源、确定性 seed（若适用）、最大 case 数或时间预算，并把每个 case 映射
到生产 C++ fixture/oracle 的断言。动态工具只报告内存、线程、未定义行为或解析崩溃；
`Expected business result` 由 C++ 断言判定。为控制复杂度，批次应优先使用少量有代表性的
等价类；若省略某边界，必须在卡片中记录理由和转交的 qualification row。

`DYNAMIC_PASS` 只有在矩阵中所有已登记 case 的 C++ 断言、工具检查、超时和清理结果均满足
预期时才成立。某 case 未运行、只有 Python 观察、或只证明启动/`--help` 时，动态结果保持
`NOT_RUN`/`PARTIAL`，不能提升行为或 qualification 状态。动态结果发现问题后，先按首个
case 和失败边界修订参数矩阵或静态门，再重跑受影响 selector；不重复原命令冒充改变的门禁。

当 Python extension 链接到仓库内的 native shared target 时，native 源码变化必须
先重建该 shared target，再重建 extension；仅重编译或重链接 extension 不能证明
source/link closure。批次记录应检查依赖库中包含变更符号（或等价的 source/hash
身份），并把 stale-library 导致的导入或未定义符号列入 `Compile/build misses`。

## CLI And Harness Boundary

`--help`、usage/schema rejection、可执行文件存在或仅能启动 harness 的 smoke 只证明
命令解析、target/link 接线或外部设施边界。它们不能写成 native request/result behavior、
Python/C++ parity 或 qualification PASS。若没有真实生产请求进入 Core/provider 并得到
独立结果，批次最多记录受限的 `BUILD_PASS` 或 `FOCUSED_BEHAVIOR_PASS`，并在
`Evidence / remaining` 保留生产调用、跨进程或资格 lane 的 `PARTIAL`/`gap`；同一批次
不得用 CLI smoke 覆盖这些未观测边界。

## Source-Closure Feedback Gate

`build/source closure` 的覆盖不能只抄写 Waf/CMake 的 source list。对 executable、shared
library 或 Python extension 只要发生了新增入口、跨库调用或链接失败，静态门必须建立一份
可复核的 project-symbol definition map：列出目标实际编译的 translation units、每个未解析
project symbol 的定义文件，以及提供这些定义的 library/target。可用精确 `rg`/CodeGraph
查询结合 `nm -C`/`readelf` 验证；不要求在静态阶段重新构建，但不能把“源文件在相邻目录”当作
闭包证据。若依赖 shared library，还要检查选定 artifact 的导出符号、RUNPATH/加载路径和
实际输出身份。该 map、命令和结果写入同一批次记录，供 review-agent 复审。

若编译/链接随后发现 source-closure 漏项，首个失败必须保留；重试前除了补 source/link
配置，还必须把上述 definition-map 检查作为 `Changed gate`，并在下一批继续执行。重复出现
同类遗漏时，先修订本 reference、模板或 checklist，或记录等价的自动化替代门禁；仅重新
运行构建不能关闭该反馈环。

## Batch Result Record

每批只维护一份 tasks/evidence 结果记录。记录以下字段；没有发现时写 `none`
或 `not observed`，不能留空：

| Field | Required content |
| --- | --- |
| `Coverage matrix` | 五个 coverage lane 的 `covered`/`N/A`/`gap`、实际文件/符号、查询或检查命令；批末注明新增范围 |
| `Static findings` | 静态门实际发现、修复、复审范围；包括 coverage/design gap |
| `Compile/build misses` | 编译器、链接器或构建接线发现而静态门未发现的问题 |
| `Runtime/test misses` | 运行或测试才发现而静态/构建未发现的问题；注明首个边界 |
| `Dynamic validation` | `Risk class`、profile、独立 build/output、具名 C++ selector、不变量、重复次数/预算、exit code；写 `NOT_RUN`、`DYNAMIC_PASS` 或 `DYNAMIC_FAIL` 及首个边界 |
| `Build measurement` | 精确命令/target、源码或构建边界、toolchain、`-j`、elapsed、exit code、日志 |
| `Behavior result` | 成员逐项映射到 `STATIC_PASS`、`BUILD_PASS`、`FOCUSED_BEHAVIOR_PASS` 或 `QUALIFICATION_PASS` |
| `Evidence / remaining` | 持久证据链接、未执行项、硬验收依赖和下一步 |

批次还必须附带以下两项可追溯信息；它们不能只由“审查通过”或“批次完成”一句话代替：

| Field | Required content |
| --- | --- |
| `Review trace` | 每个成员的 review-agent skill 路径及 SHA-256、审查基线 commit、实际 diff 范围、覆盖查询、findings 与复审结果；批末组合审查同样记录基线和范围 |
| `Closure decision` | `CLOSED_FOR_VALIDATION` 或 `OPEN_FOR_NEXT_BATCH`、达到或未达到的稳定行为出口、触发观察、未纳入成员及下一批 ID/依赖；没有独立出口时不得启动共享构建 |

每批还要显式记录 `Batch growth decision`：说明本批在获得稳定出口前为何继续吸收成员，
以及在出口出现后是否立即停止扩张。若出口出现后仍加入不同入口、状态机、selector、
source closure 或硬验收依赖的成员，必须标记为批次膨胀并拆到新的 Batch ID；不能用一次
共享构建或相邻文件作为继续合批的理由。

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

## Miss Feedback Loop

批次复盘不是归档终点。若编译/链接或运行/测试才发现了静态门未发现的问题，结果记录
必须保留首个失败边界、原始证据和受影响的调用/target，并在重试或下一批的覆盖矩阵中
登记一项实际改变的检查（例如新增 caller、测试注册、source closure 或反事实回归）。
仅重新运行同一命令不能关闭漏检。

`Changed gate` 是重试的必填项：它必须指向一个真实新增或改变的 caller、测试注册、
source-closure、oracle 或反事实检查，并说明该检查如何覆盖上一次的首个失败边界。若
无法提出改变的门禁，应保持 `PARTIAL` 并先修订共享 skill、模板或 checklist。

同类漏检再次出现时，下一批开始前必须修订共享 skill、模板或 checklist，或者在证据中
写出不修订的明确理由和替代门禁；未完成这项反馈时保持 `PARTIAL`。仍未观测的风险必须
继续列在 `unobserved`，不得由局部通过或更短的构建耗时覆盖。

## Batch Retrospective

关闭批次时，结果记录必须各写一项 `static`、`compile/link`、`runtime/test` 和
`unobserved` 的漏检复盘；没有观察到时明确写 `none` 或 `not observed`。复盘还要说明
批次是否在获得稳定出口后仍吸收了新职责，以及这次构建耗时是否具备可比的
target/source closure、toolchain、配置和工作树条件。只有完成这四类分类、Review trace
和 Closure decision，才能将批次记为 `CLOSED_FOR_VALIDATION`；它们不是效率百分比，不能
用任务数、静态通过数或单次耗时替代。

## Resource-Constrained Builds

构建记录必须采用仓库 `AGENTS.md` 指定的 system-first compiler/linker 和资源预算。
在本项目 6 logical CPUs / 12 GB RAM 的开发机上，默认使用 `-j4`；用 `vmstat 1`（忽略
首行）观察后续样本，若持续 swap-in/out 或桌面停顿，下一次降到 `-j2`。不得让并行
构建争用同一个 Waf tree。`-j`、toolchain、target/source closure、配置和 elapsed 必须
写入批次记录；资源策略本身不是性能提升或资格通过的证据。

## Command Output Contract

入口 skill 不能只在提示词中引用本 reference；它必须把适用的结果字段写入正在维护的
Spec 文档或唯一 evidence record。每次创建或修改 code-backed artifact 时，执行者按下面
的最小产物集合核对；纯文档操作对不适用的 lane 写 `N/A` 及理由：

1. **Before editing**：确认活动 feature、当前 checkpoint、任务/批次基线和既有失败边界；
   为新增或修改的批次登记 `Batch ID`、行为出口、共同入口/调用方、实现/验收依赖、
   selector/source closure、负责人，并写出 `Batch growth decision`；对运行时风险登记
   `Risk class`、`Dynamic profile` 和 `Dynamic invariants`。没有稳定出口的组件
   只能保持 `PARTIAL`，不能通过扩大批次来掩盖缺少调用方或结果。
2. **During review**：每个小任务和批末组合审查都写 `Minimum Review Record` 的五个 lane，
   列出实际文件/符号及查询或检查命令；`test/harness/oracle` 必须包含测试注册，
   `build/source closure` 必须包含真实 target、source list/link 依赖、实际生成的输出路径、
   source identity 和可复算的 artifact digest；凡是 runner/qualification manifest 引用
   executable 或 shared library，都必须由该实际输出重生成 manifest 并核对 digest，不能用
   逻辑 target 名或旧 build alias 代替。
3. **At batch close**：在同一结果记录中写 `Static findings`、`Compile/build misses`、
   `Runtime/test misses`、`Dynamic validation`、`Build measurement`、`Behavior result`、`Evidence / remaining`、
   `Review trace`、`Closure decision`，并附 `Batch Retrospective` 的 `static`、
   `compile/link`、`runtime/test`、`unobserved` 四类；说明是否在稳定出口后继续吸收职责。
4. **On retry or convergence**：链接首个失败边界，登记真实改变的 `Changed gate` 及其
   覆盖 lane；若同类漏检再次出现，先修订 shared skill/template/checklist，或在 evidence
   中说明替代门禁。只重新运行原命令、增加测试数量或更新文字不能升级状态。

若入口 skill 不能生成这些字段，应停止写入 `STATIC_PASS`/`DONE`，保留当前状态并报告
缺失产物。skill 安装副本可以路由到本 reference，但不能以副本存在代替结果记录。

## Entrypoint Synchronization

共享 reference、模板或入口规则变化后，运行仓库内的
`skills/speckit-code-design/scripts/verify-spec-kit-sync.py`。它检查三份 Spec Kit 模板、
已安装的 11 个需求/计划/分析/审计/执行入口，以及 `CODEX_HOME` 下的共享
`speckit-code-design` 文件是否仍与版本化副本一致。只负责更新 agent context 指针的
`speckit-agent-context-update` 不生成或验收 Spec，因此不属于这 11 个入口。缺少本机安装
默认只报告 warning；已有副本过期或缺文件必须失败，必要时使用
`--require-entrypoints --require-personal` 将缺失安装提升为失败。该检查只证明工作流
同步，不产生 `STATIC_PASS`、`BUILD_PASS` 或产品资格证据。

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
