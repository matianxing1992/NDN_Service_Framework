# T009 Local Delivery Tool Development

**Date**: 2026-09-06 | **Status**: focused implementation validation in progress.
**Layer**: tool unit/integration + counterfactual; real delivery NOT_SEALED.
**Subject**: isolated `d4a5e39c` plus candidate helper/test adoption.

## Contract And Scope

实现 [development delivery tool](../contracts/development-delivery-tool.md)。
原十平面 CandidateRecord API 为兼容保留，新的本地 seal/verify 不调用
它、不要求 SIF；使用相同 canonical JSON/hash 基元以及现有 source/
inventory/config/input/supervisor owner。本节没有真实 T008 资格结果，
不能生成正式开发交付或将 T009 勾选。

## Focused R1

隔离源码上 `python3 -m pytest -q tests/python/test_spec180_candidate.py`
**40 passed，17.08 s，exit 0**。包括六项原 API 回归与本地 schema、
真实临时 Git/生产 supervisor 受控命令、源码/输入/配置漂移、跨版本、
条目缺失、非零退出/信号/超时、清理/脱敏、日志与 oracle、错误审计、
外层摘要重算及独占输出行为。受控命令仅证明封印工具关系，不证明
真实 MiniNDN。原始结果：ignored workspace temporary directory 下
`spec181-delivery-tool-20260906-r1/tests.log`。

## Counterfactual R1

在隔离副本删除 `_delivery_context` 中 exitCode 类型/零值检查，保留
其余可运行生产代码，以同一具名 `exit-ENTRY_NOT_PASS` 回归执行。
结果 **1 failed，exit 1**，失败为期望拒绝却 `DID NOT RAISE`；并非
编译/环境失败。该测试能检出“状态字符串 PASS 掩盖实际非零退出”的
错误实现。变异源码和结果保留于
`spec181-delivery-tool-counterfactual-20260906-r1/mutant.py`、`tests.log`。
临时变异不纳入交付，下一轮恢复正确分支后验证最终字节。

## Output Publication Review

自审补充原子独占发布：同目录临时文件完整写入/flush/fsync 后通过
硬链接创建最终路径，finally 清理临时文件。保留已有输出不覆盖规则；
增加 fsync 失败时无部分记录/无残留的检查，以及真实 CLI 子进程 verify。
这些是待验证修正，不能沿用 R1 PASS 作为其验证结果。
