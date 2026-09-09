# Logical Batch Workflow Revision

## Work Unit

D-SKILL-BATCH；用户授权修改 Spec Kit 技能。基线 5adc6688；只修改工作流技能、模板与当前 Spec 的执行约定，NO_DESIGN_CHANGE（产品 API/架构不变）。共享树中的 native-dependency-design.md、native-generation-design.md 属于其他会话，排除提交。

## Reusable Skill Quality Revision

本轮将规则从当前 Spec 的执行约定提升为可复用入口。新增
`skills/speckit-code-design/references/batch-quality-gates.md`，统一规定：静态门和
批末门必须覆盖生产入口/真实调用方、测试/harness/oracle、构建注册/source closure
及迁移接线；批次在接口稳定且有独立可观察出口后及时验证；结果记录必须分别填写
`Static findings`、`Compile/build misses`、`Runtime/test misses` 和匹配条件下的
`Build measurement`。这不改变任何 Spec182 产品契约或验收状态。

`speckit-specify`、`speckit-clarify`、`speckit-plan`、`speckit-tasks`、
`speckit-analyze`、`speckit-audit`、`speckit-implement`、`speckit-converge` 和
`speckit-checklist` 的本机入口已分别接入该共享参考；生成模板新增稳定批次计划、
批次质量结果字段及需求覆盖提示。`speckit-constitution` 等不执行批次或代码审查的
入口保持不变，避免把实现门禁误加到无关流程。

## Changes And Static Review

共享权威为 `skills/speckit-code-design/references/pre-test-static-review.md`：设计就绪、声明批次、逐小任务只读 review-agent profile、批末组合流程审查、统一构建/相关测试。PARTIAL 与硬验收依赖保留，最终集成/MiniNDN 仍由 T016 承担。当前产品批次尚未重排，执行者须先登记边界和成员。

逐项核对小任务、批次、最终阶段和进度状态的组合；修正模板残留的强制逐任务 RED 与 audit 的逐任务单测要求。审查覆盖 API/调用方、状态/并发、所有权、信任边界、构建接线与 oracle。采用官方 review-agent 示例的只读审查协议，改编为共享 reference，不声称安装了独立官方技能或启动了多代理。

本机 `.agents/skills/speckit-{specify,clarify,plan,tasks,analyze,audit,implement,converge,checklist}/` 入口已同步；这些为 Git 忽略的安装文件，不强制纳入版本库。个人 `~/.codex/skills/speckit-code-design/` 主入口与相关 references 已同步；保留其仓库权威路由。版本化权威和三份 Spec Kit 模板保证规则可核对；重新安装技能后须再次核对本机入口。本轮仓库权威 reference 与模板验证后同步到个人安装目录。

## Validation Attempts

1. skill-creator `quick_validate.py` 对仓库及个人 speckit-code-design 均 PASS。
2. 同一验证器检查既有 speckit-specify 失败：frontmatter 的既有 `compatibility` 不在其旧白名单内；退出 1，尚未检查 workflow 内容。这是验证器适用范围不匹配，不是 YAML 损坏或产品运行失败。保留安装元数据；后续用 YAML 解析、必需字段和定向规则检查，不修改系统验证器。
3. Context Mode active health 退出 4：tasks.md indexed hash 已过期；改用活动指针、真实 spec/plan/tasks 和 Git 差异作为权威。未 purge。原诊断在 `/tmp/skill-batch-context.log`，关键失败已持久记录于此。

## Results

定向 Python 复核 PASS（退出 0）：共享 reference 与模板相对链接、批次质量字段、入口技能职责引用和唯一 Execution Progress 标题均可解析。`git diff --check` PASS。仓库/个人技能 quick_validate 两项 PASS；旧 Spec Kit frontmatter 白名单不兼容明确保留，不冒称该验证器全通过。未执行产品编译、单测、集成或实验。

人工场景推演：三任务同批前两项静态通过只记 PARTIAL，不构建；第三项及整批组合门通过才共享构建/测试。控制性 finding 修复前不继续依赖任务；硬验收依赖不得静态放行；批次测试失败保持受影响任务未完成；单卡交接不扩 Write；具名 RED/ABI 阻塞只允许最小诊断。以上为文档一致性审查，不是产品执行实验或效率实测。

## Next

检查通过后本地 checkpoint；后续实现沿登记的逻辑批次和硬依赖执行。并发会话已开始登记 B-G1-YOLO-SEMANTIC，本单元不代审或提交其批次计划。产品任务、正式资格和历史证据均未改为完成。

## Follow-up Coverage Matrix Revision

2026-09-08：复盘 R4-B4/R4-B5 后发现，仅要求静态审查“覆盖调用方、测试和构建注册”
仍可能留下不可核对的 No findings。共享 `batch-quality-gates.md`、
`pre-test-static-review.md`、code-design 主入口和 tasks/plan 模板现要求同一份 evidence
提供五 lane Coverage matrix：`production entry/callers`、`implementation and wire`、
`test/harness/oracle`、`build/source closure`、`migration/evidence`。每 lane 必须标记
`covered`、附理由的 `N/A` 或 `gap`，给出实际文件/符号以及查询或检查命令；批末说明
新增成员是否扩大覆盖。矩阵缺失或只有泛称目录时不得记录 `STATIC_PASS` 或
`READY_FOR_BATCH_TESTS`。当前 Spec182 的 R4-B5 矩阵见其 evidence；历史批次不回填，
不改变已有状态或资格结论。

共享模板与仓库 skill 的定向链接、字段和 frontmatter 检查通过；本次规则修订仍为文档
工作，不运行产品构建或实验。使用中的本机 Spec Kit 入口在同步后需再次核对 hash。
按 `/home/tianxing/.codex/skills/review-agent/SKILL.md` 的只读协议检查本次共享 reference、
模板、当前 Spec 记录及入口同步差异，结果为 `No findings`；产品源码与资格结论未被审查
或改判。
仓库与本机 `/home/tianxing/.codex/skills/speckit-code-design/` 的主入口及 references
已逐文件同步；`SKILL.md` 与 `batch-quality-gates.md` SHA-256 均分别为
`03247969c688a52c3b519fe91e656b4cf947097170b246ca5199e2f3542fbdb5` 和
`37981efddc80a4f2288c10d542fbfd9e9affe482156a9d0d973816314857668e`。

## Follow-up Documentation Sync

2026-09-08 文档复核确认当前开发机执行政策已统一为 `-j4`：
`AGENTS.md`、`CLAUDE.md`、`docs/build-and-test.md`、
`docs/native-build-parallelism.md` 和 Spec182 `plan.md` 均将 `-j4` 作为默认，
仅在观察到持续换页或桌面卡顿后对下一次构建降为 `-j2`。修正
`docs/NDNSF-DI-deployment-candidate.md` 的当前构建示例为 `-j4`，并明确历史
evidence 中的旧并行度和耗时保持原事实。该同步不改产品契约或资格状态。

只读审查按 `/home/tianxing/.codex/skills/review-agent/SKILL.md` 执行，覆盖该文档的
命令上下文及历史/当前边界，结果为 `No findings`。`git diff --check` 和
`specs/182-native-di-python-bindings/checklists/validate_design.py` 均通过；未执行
产品构建或运行测试。

## Follow-up Native Test Ownership

2026-09-08：为保持 NDNSF-DI 的 C++ ownership，`batch-quality-gates.md` 新增
Native Test Ownership：原生运行时、协议、状态机、并发、密码和模型行为必须由直接
调用生产 C++ target 的 suite/selector 验收；Python 只覆盖 pybind 形状、facade 转发、
离线 oracle、配置拒绝或外部设施边界。`speckit-plan`、`speckit-tasks`、
`speckit-implement`、`speckit-analyze` 和 `speckit-audit` 的本机入口均同步该检查。
这样 future Spec 的 Python focused test 不会被误记为 native parity 或 qualification。

本次定向核对共享 reference、五个入口文件、Spec182 plan/tasks 与 R5-B2 evidence 的
规则和链接一致，`git diff --check` 通过；这是流程文档修订，不改变产品验收状态。

## Follow-up Batch Boundary And Miss Taxonomy

2026-09-08：根据 R4-B4/R4-B6 的实际执行记录，批次规则再补一条可复用约束：当同一
行为已经有独立可观察出口时立即进入批末验证，不为了减少一次构建继续吸收无关职责；
编译、链接和运行时发现分别记录，不能用任务数量、`STATIC_PASS` 或较短的单次构建时间
推断效率收益。该规则及配套参考已在本地 checkpoint `db479981` 提交，个人安装入口
同步完成；当前 R4-B6 的正向两轮结果同时验证了该出口规则，恢复/replacement 仍作为
独立缺口保留。

## Follow-up Requirements And Convergence Coverage

2026-09-08：复核发现需求形成与收敛入口仍需显式继承同一条 native test ownership
边界。已同步本机 `.agents/skills/speckit-{specify,clarify,checklist,converge}/SKILL.md`：
需求中的 native NDNSF-DI 运行时、协议、状态、并发、密码或模型行为必须指定生产
C++ target/selector 及 evidence owner；Python 仅可声明 binding/facade、离线 oracle 或
外部设施边界。`speckit-converge` 追加任务时还要在 append-only Convergence section
提供 `Convergence Progress Delta`，列出状态、依赖、证据/剩余项和批次结果，之后由
tasks/implement 合并回顶部 `Execution Progress`，不把新任务预标完成。

静态复核按 `/home/tianxing/.codex/skills/review-agent/SKILL.md` 的只读协议完成：逐文件
检查四个入口的适用范围、append-only 约束、共享 reference 路由、Python/C++ 证据边界，
结果为 `No findings`。定向检查确认四个入口均保留原有 frontmatter，且新增规则只影响
代码接线/原生行为需求；旧 Spec Kit frontmatter compatibility 白名单仍是验证器限制，
不改变安装文件。未运行产品构建、单测或实验；本次修改是流程文档层更新。

## Follow-up Skill Contract

2026-09-08：根据批次复盘，规则不再只停留在当前 Spec。版本化的
`skills/speckit-code-design/SKILL.md` 与 `references/batch-quality-gates.md` 现明确列出
全部 Spec Kit 入口的职责：需求形成要写真实入口、可观察结果、独立 oracle、负例/恢复
边界和 evidence owner；plan 要冻结稳定出口、五 lane Coverage matrix、selector 与
结果责任人；tasks/implement 要保持 `Execution Progress`、C++ native test ownership
和静态/编译/运行漏检分类；constitution 修改必须同步模板/skill；taskstoissues 不得
产生行政拆分。资源受限的本机 native build 统一记录 system-first toolchain、默认
`-j4`、`vmstat` 观察和降档条件，不能把资源策略当成性能或资格证据。

`.specify/templates/spec-template.md` 新增结构化 Acceptance Evidence Contract，并要求
引用 plan/tasks 的 Batch/Coverage 结果；`plan-template.md` 的批次表新增 Coverage
matrix scope 和 result/evidence owner；`tasks-template.md` 明确 native NDNSF-DI 行为
必须由生产 C++ target/selector 验收，Python 仅作 binding/facade、oracle 或外部设施
边界。当前 `.agents/skills/` 本机副本以及个人
`/home/tianxing/.codex/skills/speckit-code-design/` 已同步；个人安装目录保持未跟踪，
版本化规则仍以仓库 `skills/` 和 `.specify/templates/` 为准。

定向审查覆盖共享 skill、三份模板、入口职责引用和当前 Spec 进度记录；结果
`No findings`。`python3 specs/182-native-di-python-bindings/checklists/validate_design.py`
返回 `ok: true`（17 parent tasks、36 execution cards、36 progress units、0 errors），
`git diff --check` 返回 0；未运行产品构建、单测、集成或实验，产品任务仍保持原有
`PARTIAL`/`DONE` 状态。

## Follow-up Review Trace And Closure Contract

2026-09-08：R4-B4/R4-B6 的批次复盘显示，虽然已经要求五 lane Coverage matrix，仍可能只
留下无法核对的 `No findings`。本轮将共享规则进一步收紧：每个小任务和批末组合门必须在
同一 evidence 记录 `Review trace`（官方 review-agent 路径/SHA、基线、完整 diff 范围、
覆盖查询、findings 与复审），并记录 `Closure decision`（`CLOSED_FOR_VALIDATION` 或
`OPEN_FOR_NEXT_BATCH`、稳定出口、触发条件和下一批依赖）。只有声明的静态范围没有未解释
gap 时才能写 `STATIC_PASS`；延后的真实运行或资格范围可以保留 gap，但必须保持 `PARTIAL`。

版本化的 `batch-quality-gates.md`、`pre-test-static-review.md`、code-design 主入口和
`.specify/templates/plan-template.md`、`tasks-template.md` 已同步。当前计划升至 Revision 14，
tasks 升至 Revision 24；R5-B8 evidence 已补齐实际 review-agent SHA、`54b85951` 基线、
`04be23ed` 实现差异和 `OPEN_FOR_NEXT_BATCH` 决定。历史批次不回填不可核对的身份，重开时
才采用新字段。

定向检查结果：仓库与个人安装的 code-design 三文件逐字同步；11 个本机 Spec Kit 入口均
包含追踪说明；模板表头/分隔符、frontmatter、相对路径与 `git diff --check` 通过；
`specs/182-native-di-python-bindings/checklists/validate_design.py` 返回 `ok: true`
（17 parent tasks、38 execution cards、38 progress units、0 errors）。官方
`skill-creator quick_validate.py` 对仓库和个人 code-design skill 均返回 `Skill is valid!`。
本轮仍未运行产品构建、单测、集成或实验；产品任务状态保持原有 `DONE`/`PARTIAL`/`NOT_STARTED`。

## Follow-up Batch Allocation Basis

2026-09-08：R4-B4/R4-B6/R5-B10 的复盘进一步说明，仅有“同一 user story”或“可共用一次
构建”不足以判断批次合理。版本化 `batch-quality-gates` 现要求在编码前为每批登记五项
分配依据：共同生产入口/调用方、接口/状态/所有权或数据契约、独立 oracle/测试 selector、
源码/构建 closure、验收出口。每个成员必须映射到实际文件、符号和查询/命令；引入不同
调用方、状态/错误边界、C++ selector、构建 target 或硬验收依赖时，先关闭当前批次并登记
新的 Batch ID。

该规则已同步版本化 `skills/speckit-code-design` 的主入口、`batch-quality-gates`、
`work-unit-contract`、`bounded-executor`、`skills/README.md`，以及
`.specify/templates/plan-template.md` 和 `tasks-template.md`；个人安装副本随后按文件
同步。当前 Spec182 `plan.md` Revision 16、`tasks.md` Revision 28 已把分配依据写入
调度约定，保留既有任务状态和验收边界。

定向检查：共享 skill 与模板相对引用、frontmatter、批次字段、Spec182 validator 和
`git diff --check` 均 PASS；未运行产品构建、单测、集成或实验。本轮只是流程约束增强，
不能把现有 `PARTIAL` 任务提升为完成，也不改变 T016 的最终资格门。

Review trace：按 `/home/tianxing/.codex/skills/review-agent/SKILL.md`（SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）只读检查，基线为
`ea7fc019`，差异范围为共享 code-design skill 的主入口及三个 references、两份 Spec Kit
模板、`skills/README.md`、Spec182 `plan.md`/`tasks.md` 和本记录；覆盖查询核对了所有
入口对 `batch-quality-gates` 的引用、分配依据字段、状态/链接校验和未暂存的设计文档。
发现为 none，复核结果为 No findings。Closure decision：`CLOSED_FOR_VALIDATION`（流程
文档出口已形成；触发条件是后续批次若共同入口、契约、selector、source closure 或验收
出口不一致，必须登记新的 Batch ID）；产品实现与 T016 继续 `OPEN_FOR_NEXT_BATCH`。
