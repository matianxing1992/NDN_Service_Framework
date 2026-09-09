# R10-B42 Artifact Identity Skill Feedback

日期：2026-09-09
状态：`DONE` for the shared Spec Kit workflow boundary; product tasks remain unchanged.

R10-B41 的 r15 重试暴露了一个可复用的流程缺口：target 名称和 source closure 已记录，
但实际链接输出路径与 runner manifest 的 artifact digest 没有被强制绑定。共享
`skills/speckit-code-design/references/batch-quality-gates.md` 现要求 build lane 同时记录
真实 target、source list/link closure、实际 output path、source identity 和可复算 digest；
引用 executable/shared library 的 runner 或 qualification manifest 必须从该实际输出重生成
并核对 digest。`skills/speckit-code-design/SKILL.md`、`skills/README.md` 和个人安装副本
已同步，versioned/personal reference SHA-256 一致。

静态检查：`git diff --check`、关键规则 `rg`、入口/README/reference 对照和
`sha256sum skills/... /home/tianxing/.codex/skills/...` 均通过；无 product build。本批不改变
T004–T017 或 T016 的状态，也不把技能同步当作产品验收。

`CLOSED_FOR_VALIDATION` for the workflow rule; future batches must show the actual artifact
path/digest in their five-lane build/source closure and retain the first stale-artifact boundary.
