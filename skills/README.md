# Repository Skills

本目录是两台机器共享的维护版本，仅包含近期使用的五项skill及必要references。
先获取同一个明确Git版本，再使用相应skill；这些文件不修改个人配置，也不包含实验凭据。

| Skill | Use |
| --- | --- |
| [speckit-code-design](speckit-code-design/SKILL.md) | 设计文件/符号/字段/调用链、内聚任务、源码对照设计和验收；所有 Spec Kit 入口共用其五 lane Coverage matrix、批次质量与漏检记录 |
| [itiger-ndnsf-ops](itiger-ndnsf-ops/SKILL.md) | 路由现有SIF交付与Tiger工具，保持开发/实验分工及来源证据 |
| [ndnsf-minindn-experiment](ndnsf-minindn-experiment/SKILL.md) | 本地拓扑、应用生命周期、独立判据和实验结果 |
| [codegraph-first](codegraph-first/SKILL.md) | 有索引时优先图查询，没有时精确源码检索 |
| [review](review/SKILL.md) | 基于明确Git差异分别审查Standards和Spec |

`speckit-agent-context-update` 是本机安装的运维入口：它只维护 `AGENTS.md` 等上下文文件中的
托管 Spec Kit plan 指针，不属于行为批次或产品验收。更新后仍须运行 `git diff --check`，并在
可用时执行共享入口同步检查；plan 或活动 feature 指针变化还要刷新 Context Mode authority
index。它不能把上下文指针、skill 同步或文档状态写成 `STATIC_PASS`、`BUILD_PASS` 或 `DONE`。

## Spec Kit Command Contract

项目内的 `speckit-specify`、`speckit-clarify`、`speckit-plan`、`speckit-tasks`、
`speckit-analyze`、`speckit-audit`、`speckit-implement`、`speckit-converge`、
`speckit-checklist`、`speckit-constitution` 和 `speckit-taskstoissues` 使用同一份
[batch-quality-gates](speckit-code-design/references/batch-quality-gates.md)。需求和计划
必须写明真实入口、可观察结果、独立 oracle、负例/恢复边界及证据责任；任务和执行必须
维护 `tasks.md` 的 `Execution Progress`，并把静态、编译/链接、运行/测试漏检分开记录。
每个批次还要记录共同入口/调用方、契约、oracle/selector、source closure 和验收出口；
这些依据不一致时拆批，不能只为减少一次构建继续合并职责。
静态审查的唯一记录格式是
[review-agent Minimum Review Record](speckit-code-design/references/review-agent.md)：
五个 lane 必须列出实际文件/符号、查询命令和 findings；测试 lane 包含 harness/oracle
及注册，build lane 包含 target/source closure、实际输出路径、source identity 和 artifact
digest；runner/qualification manifest 必须从该实际输出重生成并核对 digest。缺行或未解释的 `gap` 不得产生
`STATIC_PASS`，批末还必须记录四类 `Batch Retrospective`。
目标新增入口、跨库调用或链接重试时，build lane 还必须附 project-symbol definition map
（符号→定义 translation unit→target/library）以及 `rg`/CodeGraph、`nm -C`/`readelf`
核对；该 map 是重试的 `Changed gate`，不能只重跑构建。
写入 `STATIC_PASS` 前还必须完成 `Static Gate Release Checklist`；发生编译/链接或运行/测试
漏检时，重试记录 `Changed gate`，说明新增的 caller、测试注册、source-closure、oracle
或反事实检查。每批另写 `Batch growth decision`，在稳定出口出现后停止吸收不同入口或验收
依赖的成员并建立新的 Batch ID。
所有会创建或更新 feature artifact 的 Spec Kit 入口，开始前都必须运行
`python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py --require-entrypoints`。
若本机个人共享副本也属于执行环境，再加 `--require-personal`；检查失败时停止生成或
记录 workflow `gap`，不得产生 `STATIC_PASS`、`BUILD_PASS` 或 `DONE`。该预检只证明
工作流副本同步，不构成产品实现、构建或资格证据。
所有 Spec Kit 入口还遵守
[Command Output Contract](speckit-code-design/references/batch-quality-gates.md#command-output-contract)：
编辑前登记批次分配依据，审查时写实际五 lane 查询，批末在同一记录中写四类漏检复盘、
构建测量和关闭决定；缺少这些字段时保留 `PARTIAL`，不把 skill/template 同步当作执行证据。
若 fixture/oracle 复算 canonical bytes、digest 或 identity，还必须对照 production serializer
的字段集合、顺序、规范化和 source identity；不完整对照保持 `gap`。
若编译/链接或运行/测试才发现漏检，重试/下一批必须链接首个失败边界并登记改变的静态
检查；同类漏检再次发生时先修订共享 skill、模板或 checklist，或记录替代门禁。
NDNSF-DI 原生行为的 unit/integration/regression 测试由 C++ fixture/driver/oracle
直接调用生产 C++ target/selector 验收；Python 可编排外部设施或启动 C++ executable，
也可覆盖 binding/facade、offline oracle 边界，但不能替代 native behavior、parity 或
跨进程资格断言。
CLI `--help`、usage/schema rejection、target/link smoke 或 harness 启动只证明接线，
不能替代真实 native request/result、parity 或 qualification 证据。
维护中的 legacy/compatibility 探针若暴露运行时回归，必须保留首个失败边界和原始证据，
在 migration/evidence lane 维持 `PARTIAL`，并由 `speckit-converge` 追加独立出口任务；
不能因测试属于旧 Spec 就忽略，也不能在边界明确前修改共享 freshness 或重试逻辑。

`.agents/skills/` 是本机 Spec Kit 命令安装副本并按仓库策略保持未跟踪；它们只引用这份
受版本控制的共享契约。修改治理原则或模板时，同时检查本机副本是否仍包含对应入口说明，
但不要把个人安装路径写入提交。

共享 reference 或模板变化后，使用中的 `.agents/skills/` 和个人安装副本必须重新同步并逐文件
核对 SHA-256；入口副本只负责路由，版本化的 `skills/speckit-code-design/` 才是规则权威。
同步检查要确认所有 Spec Kit 入口仍引用同一份 `batch-quality-gates.md`，而不是复制旧门禁。
同步成功不等于 review-agent 已执行，也不改变当前 Spec 的任务或验收状态。

可从仓库根运行共享同步检查器；它会检查三份模板、已安装的 11 个 Spec Kit 入口以及
`CODEX_HOME`（默认 `~/.codex`）中的共享 skill。缺少本机安装时只报告 warning；已有副本
内容不一致会失败。需要把安装缺失也作为门禁时加 `--require-entrypoints --require-personal`：

```bash
python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py
```

该检查器只验证规则与副本同步，不产生 `STATIC_PASS`、`BUILD_PASS` 或产品资格证据。

## Immediate Use

无需安装即可在目标仓库会话明确要求：

> 请读取并使用 `skills/speckit-code-design/SKILL.md`，仅修订当前设计。

该方式要求代理显式读取文件；仅把文件放在根目录 `skills/` 不保证客户端自动发现它们。
各skill引用的仓库文件从目标 checkout 的 `git rev-parse --show-toplevel` 解析；
`references/` 等skill内相对链接从该skill文件所在目录解析。因此复制到个人目录后仍须在目标repo工作。
仓库 `AGENTS.md`、当前用户授权及活动feature决定范围，不恢复旧Spec固定实验目标。
若干净clone没有本机专用的`AGENTS.md`，按当前用户要求及仓库constitution、架构文档执行；不要求复制另一台机器的个人规则或路径。

## Optional Installation Without Overwrite

需要客户端发现时，可由使用者从仓库根执行以下可选命令；已有同名目录或symlink一律跳过，先比较再决定更新方式。
命令只复制这五项及其完整附属文件，不创建指向另一台机器的绝对链接。
完成后按客户端方式刷新/重启会话使其重新发现skills。

```bash
python3 - <<'PY'
import os
from pathlib import Path
import shutil
import subprocess

repo = Path(subprocess.check_output(['git', 'rev-parse', '--show-toplevel'], text=True).strip())
destination = Path(os.environ.get('CODEX_HOME', str(Path.home() / '.codex'))) / 'skills'
destination.mkdir(parents=True, exist_ok=True)
for name in ('speckit-code-design', 'itiger-ndnsf-ops', 'ndnsf-minindn-experiment', 'codegraph-first', 'review'):
    target = destination / name
    if target.exists() or target.is_symlink():
        print('SKIP existing:', target)
        continue
    shutil.copytree(repo / 'skills' / name, target)
    print('INSTALLED:', target)
PY
```

后续改进提交回本目录并review；个人定制不要在更新时直接覆盖。更新后需同步
使用中的本机入口，并核对入口引用的共享 reference 版本；只更新一份 reference，
不要在各个 Spec Kit 命令中复制另一套门禁。
本地检查验证frontmatter与相对引用完整性；它们不证明依赖已安装、代码通过测试或实验已完成。
