# Official Review Agent Installation

## Scope

D-REVIEW-AGENT；用户授权独立安装官方技能并接入逐任务静态门。产品源码、批次执行和验收不变。

## Installation And Routing

通过 skill-installer 的 `install-skill-from-github.py --repo openai/codex --path codex-rs/skills/src/assets/samples/review-agent --dest /home/tianxing/.codex/skills` 安装成功。官方原版位于 `/home/tianxing/.codex/skills/review-agent/SKILL.md`，未修改。

安装后与官方 commit `d6489472f3c15e87d2d7763a5fde033545c530f8` 的对应文件逐字节比较 PASS；SHA-256 为 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`。
来源：[pinned official skill](https://github.com/openai/codex/blob/d6489472f3c15e87d2d7763a5fde033545c530f8/codex-rs/skills/src/assets/samples/review-agent/SKILL.md)。技能将在下一用户回合可发现；本轮已直接读取全文。

共享 pre-test workflow 明确要求逐任务加载 `$review-agent`；review-agent reference 保留项目设计/批次补充规则，声明安装缺失不可以声称门禁通过。个人 speckit-code-design 中两份 reference 同步。无需自动生成子代理，也不是后台自动运行的检查器。

## Review And Validation

按已读取官方技能的只读协议，检查本轮完整文档差异、调用入口及上下文：No findings。项目额外门核对安装缺失、脏树差异身份、PARTIAL/批末测试、只读审查与实现者修复阶段分离。此结果只覆盖本轮技能/文档路由，不表示产品静态审查通过。

- 官方技能 quick_validate：PASS。
- 官方文件逐字节比较：PASS。
- Spec182 validate_design.py：PASS，errors=[]；修复上一轮抽取共享规则导致的 One Completion Record anchor 和最小具名诊断兼容缺口。
- 两份个人 reference 同步、相对链接及 git diff --check：PASS。
- 产品构建/运行测试：NOT_RUN，安装与路由修改不要求产品编译。

本地安装文件在仓库之外，不随 Git 交付；其他机器需单独安装同来源技能并核对路由。下一步在下一实际小任务静态门加载官方技能并记录审查结果；不重新执行历史任务。
