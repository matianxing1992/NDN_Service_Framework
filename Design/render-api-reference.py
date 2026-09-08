#!/usr/bin/env python3
"""Render readable references from one captured inventory, never live source."""
from pathlib import Path
import json
import sys

design = Path(__file__).resolve().parent
dest = design / 'api'
target = '--target' in sys.argv
inventory = json.loads((dest / ('target-inventory.json' if target else 'inventory.json')).read_text())
for module in ('Core', 'Repo', 'DI', 'UAV'):
    parts = ['# ' + module + ' API 参考\n\n声明从源码语法树提取。保留准确类型、参数、默认值、限定符和原始注释；注释不代替运行证据。中文语义契约见开发者指南。protected 扩展点、测试 helper、应用内部接口各自标注。\n']
    if target:
        parts.append('\n本文件来自冻结目标清单；路径/行号属于该快照，不指向当前工作树。'
                     '还原方法见 [目标源码身份](../target-source-baseline.json) 与独立补丁。'
                     'PLANNED 修改另见目标 PDF，冻结声明不冒充已完成目标。\n')
    for f in inventory['files']:
        if f['module'] != module:
            continue
        parts.append('\n## ' + f['file'] + '\n\n源码 SHA-256：`' + f['sha256'] + '`。\n')
        if f.get('exports'):
            parts.append('\n显式导出：`' + '`, `'.join(f['exports']) + '`。\n')
        if f['parse_errors']:
            parts.append('\n解析边界：' + json.dumps(f['parse_errors'], ensure_ascii=False) + '\n')
        for e in f['entries']:
            location = ('冻结源码：`' + f['file'] + '`，第 ' + str(e['line']) + ' 行。' if target
                        else '[源码](../../' + f['file'] + '#L' + str(e['line']) + ')')
            parts.append('\n### ' + e['id'] + ' · ' + e['name'].replace('\n', ' ') + '\n\n'
                         + e['access'] + ' / ' + e['surface'] + '；' + location + '\n\n```'
                         + ('python' if f['file'].endswith('.py') else 'cpp') + '\n'
                         + e['signature'] + '\n```\n')
            if e.get('documentation'):
                parts.append('\n原始接口说明：\n\n```text\n' + e['documentation'] + '\n```\n')
    name = ('target-' if target else '') + module.lower() + '-reference.md'
    (dest / name).write_text('\n'.join(line.rstrip() for line in ''.join(parts).splitlines()) + '\n')
print('Rendered readable references:', 'frozen target' if target else 'current')
