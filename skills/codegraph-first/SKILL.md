---
name: codegraph-first
description: Use an existing CodeGraph index to locate code owners and call paths, then verify source; fall back to exact repository searches when no usable index exists.
---

# CodeGraph-First Code Search

## Repository Context

从目标 checkout 执行 `git rev-parse --show-toplevel` 确定 repo root。
所有仓库路径从该 root 解析，不从个人安装的 skill 目录解析。
先读 repo `AGENTS.md`；检查 root 是否有 `.codegraph/`。
没有索引目录时直接用 `rg` / `rg --files` 和精确源码，**不创建索引**：索引由用户决定。

## Indexed Repositories

先使用已有 CodeGraph 工具回答符号、调用链、所有权和影响范围问题。
MCP 工具可延迟发现；可用时优先 `codegraph_explore`、`codegraph_node`。
CLI 的等价入口为 `codegraph explore "<symbol or question>"` 和
`codegraph node <symbol-or-file>`；在目标 repo root 执行。
参数因安装版本而异，不确定时读本机 help，避免重复猜测命令。

精确限定当前 checkout 和真实源码路径，排除旧 worktree、临时比较副本及生成物。
索引过时、不可用或报错时说明限制，转为源码检索；不要在未经授权时以修复为由新建/重建索引。

## Evidence Rules

- CodeGraph 用来找入口、caller/callee 和影响范围；最终 bug 或行为结论必须由当前源码证实。
- `rg` 用于精确错误日志、字段/config键、脚本、文档和已定位源码片段，不需先为纯文本问题查询图。
- 不把二进制、构建输出、实验日志或秘密加入图；`.codegraph/` 保持本地，不进入提交。
- 图与文件不一致时以实际文件为准，记录索引限制。无需因此重新审查整库。
