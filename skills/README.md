# Repository Skills

本目录是两台机器共享的维护版本，仅包含近期使用的五项skill及必要references。
先获取同一个明确Git版本，再使用相应skill；这些文件不修改个人配置，也不包含实验凭据。

| Skill | Use |
| --- | --- |
| [speckit-code-design](speckit-code-design/SKILL.md) | 设计文件/符号/字段/调用链、内聚任务、源码对照设计和验收 |
| [itiger-ndnsf-ops](itiger-ndnsf-ops/SKILL.md) | 路由现有SIF交付与Tiger工具，保持开发/实验分工及来源证据 |
| [ndnsf-minindn-experiment](ndnsf-minindn-experiment/SKILL.md) | 本地拓扑、应用生命周期、独立判据和实验结果 |
| [codegraph-first](codegraph-first/SKILL.md) | 有索引时优先图查询，没有时精确源码检索 |
| [review](review/SKILL.md) | 基于明确Git差异分别审查Standards和Spec |

## Immediate Use

无需安装即可在目标仓库会话明确要求：

> 请读取并使用 `skills/speckit-code-design/SKILL.md`，仅修订当前设计。

该方式要求代理显式读取文件；仅把文件放在根目录 `skills/` 不保证客户端自动发现它们。
各skill引用的仓库文件从目标 checkout 的 `git rev-parse --show-toplevel` 解析；
`references/` 等skill内相对链接从该skill文件所在目录解析。因此复制到个人目录后仍须在目标repo工作。
仓库 `AGENTS.md`、当前用户授权及活动feature决定范围，不恢复旧Spec固定实验目标。

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

后续改进提交回本目录并review；个人定制不要在更新时直接覆盖。
本地检查验证frontmatter与相对引用完整性；它们不证明依赖已安装、代码通过测试或实验已完成。
