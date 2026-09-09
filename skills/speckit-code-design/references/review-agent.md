# Pre-Test Review Agent Profile

本项目逐任务静态门必须调用独立安装的官方 `review-agent`，来源为 OpenAI Codex 的 [review-agent sample](https://github.com/openai/codex/blob/main/codex-rs/skills/src/assets/samples/review-agent/SKILL.md)。本文件仅补充项目设计、批次和测试阶段约束，不替代官方技能。官方 `code-review` 的最终 PR 多代理流程不在本门禁范围内。

## Invocation

每个小任务编码完成后，明确使用 `$review-agent`：读取当前技能目录中的 `review-agent/SKILL.md` 并执行其只读协议，同时提供本文件、Task/Batch ID、契约、基线和完整差异范围。本机安装路径为 `/home/tianxing/.codex/skills/review-agent/SKILL.md`；其他机器按其技能目录解析，不能只凭名称假定已经加载。记录所用技能路径/版本或哈希与实际审查结果；不把模板存在视为已执行审查。

原版技能保持不修改；项目设计缺口作为额外 gate 判断，与官方 introduced-regression findings 分开。审查结束后实现者修复并复审，仍遵守批末统一构建/测试。允许当前执行者明确切换只读阶段，不因技能名称自动生成子代理。安装缺失时先报告并补齐；未实际加载时不能声称调用了官方技能或通过该门禁。

## Review Contract

审查阶段只读：读取适用 AGENTS、任务设计与完整差异，包括新文件及删除；补读足够上下文、真实调用方和相关测试。不要修改文件、构建、运行测试、提交、推送、发评论或继续委派。结束后由实现者另行修复；同一执行者也须分开审查与修复阶段。

输入包含 Task/Batch ID、契约引用、基线 commit、相关未提交差异与写入范围。脏工作树保留任务开始前快照/补丁身份，不把上一任务或他人改动归为本任务；批末使用批次开始基线。目标漂移时重新确定差异边界。分支审查使用实际 merge-base，不能只读最后一个 commit。

找到首个问题后继续审完其余改动。具体 finding 使用 `[P1/P2/P3] 标题 — file:line`，给出触发条件、源码因果与影响，定位最小相关范围；P0 只用于无条件严重阻断。缺陷须有证据、可行动，不制造风格问题、推测性故障或把旧问题说成本次引入。

设计缺失、既有阻塞和未读路径单列为 coverage/design gap，不伪造 regression finding；控制当前任务正确性时同样阻止静态门。无缺陷写 No findings，并附实际覆盖、设计符合性、待运行验证和残余不确定性，由工作流判定是否 STATIC_PASS。
实际覆盖至少包括生产调用方/默认接线、测试或 harness 及其 oracle、构建注册/source
closure；这些未读时写 coverage gap，不能仅凭 No findings 记 `STATIC_PASS`。批次漏检与
构建测量按 [batch-quality-gates.md](batch-quality-gates.md) 的结果字段归档，不在本参考另建报告。

## Minimum Review Record

每次小任务或批末组合审查都必须在唯一批次结果中留下以下五行 Coverage matrix；
不得只写 `No findings` 或“已检查目录”。每行都要有实际文件/符号和执行过的查询或
检查命令：

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` / `N/A` / `gap` | [入口、真实 caller、默认 wiring] | [CodeGraph/rg/配置检查] | [结果] |
| `implementation and wire` | `covered` / `N/A` / `gap` | [实现、头文件、wire/序列化] | [查询或静态检查] | [结果] |
| `test/harness/oracle` | `covered` / `N/A` / `gap` | [fixture、oracle、负例、注册] | [selector/注册检查] | [结果] |
| `build/source closure` | `covered` / `N/A` / `gap` | [target、source list、生成/安装入口] | [构建注册/依赖检查] | [结果] |
| `migration/evidence` | `covered` / `N/A` / `gap` | [兼容 caller、退出路径、证据] | [迁移探针/证据链接] | [结果] |

`N/A` 必须说明为什么该 lane 不适用；缺少真实 caller、测试注册或 source closure
时必须写 `gap`。任何未解释的 `gap` 都阻止 `STATIC_PASS` 和 `READY_FOR_BATCH_TESTS`。
批末记录还必须附 `Batch Retrospective`，分别写 `static`、`compile/link`、
`runtime/test`、`unobserved` 的首个失败边界；它们不是审查意见的替代品，也不是
效率百分比。

## Techniques

按受影响行为选择并实际使用，不机械填全表：

- **Call-path tracing**：从生产入口追到副作用与清理，再反查调用方；核对默认工厂、注册、配置分支，避免只审孤立 helper/mock。CodeGraph 用于定位，结论以源码为据。
- **State and decision tables**：列前置状态、输入、下一状态和返回；手推空值、边界、重复、乱序、溢出、单位/编码转换，查遗漏分支和错误优先级。若多个 publication name 共享 producer/session，必须单独推导跨名称乱序、重复和旧 session 交错；不能用一个全局 sequence frontier 代替该反事实检查，除非 wire 契约明确保证全局顺序。
- **Ownership and concurrency**：追踪 RAII、move、引用/回调捕获、异常释放；手推 cancel/complete/close 交错、锁顺序、线程归属、代次失效与一次性终态。
- **Contract and trust boundaries**：对照签名、默认值、字段来源、授权主体、request/plan/digest 绑定与验证先后；核对跨语言、wire/config、兼容别名和旧调用方迁移。
- **Build and wiring inspection**：核对声明/定义、命名空间、include、target/link、生成文件和测试注册。静态判断不能证明模板实例化、ABI 或真实链接成功。
- **Oracle review**：连接需求→生产路径→观测→断言；检查 fixture 能否进入目标分支、负例是否因目标行为失败、期望是否独立于被测实现、collector 是否误判启动失败。
- **Composition review**：批末连起跨任务接口与状态，核对读写双方、成功/失败、恢复/清理的一致性；局部 PASS 不能掩盖未接线实现、stub 或遗留双路径。

记录实际检查路径和关键发现，不以清单全绿或工具无输出替代阅读。无需为每个风险新建 mutation test，但既定负例及反事实判据必须保留。
