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
