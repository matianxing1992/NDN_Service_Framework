# Implementation Plan: Native DI Closure

**Branch**: Experimental | **Date**: 2026-09-11 | **Status**: PLANNED
**Baseline**: `94c1e644`；产品审计源码 `72b9e388cc3920b0bdcd4c36d302d63c71e7f15a`。
**Authority**: [spec](spec.md)、[tasks](tasks.md)、[transfer](contracts/transfer-matrix.md)。

## Summary

本 Spec 承接182未完成工作，不复制其庞大时间线。唯一 next dispatch 为 B1/T001。
具体缺陷的源码位置、触发条件与反例沿用冻结的
[request-chain audit](../182-native-di-python-bindings/evidence/request-chain-static-audit-20260911.md)。
旧 R12 批次映射为183 B1–B5；未完成的组件验收同样进入 B5，并非只搬四个 finding。

## Technical Context

使用现有 C++ DI 库、Core Face IO、独立 artifact authority、Provider、ORT/tokenizer、journal。
生产入口和 oracle 见 spec Acceptance Evidence Contract。没有新增依赖、wire 或目标 API；
不把 model/tokenizer/KV 状态移进通用 Core。开启每批前核对 build tree/cache/link identity，
复用已验证受影响 target，默认 -j4、观察 vmstat，不启动竞争构建。

## Constitution Check

中文叙述、英文标题/ID/状态；独立 authority、不信任 caller plan、C++ owner 和证据分级继承。
已完成实现不重复编写，未完成资格不因拆分关闭；当前/目标 Design 独立维护。

## Execution Order

| Batch | Tasks | Depends | Stable exit |
| --- | --- | --- | --- |
| B1 Thread ownership | T001/T002 | 冻结审计/迁移对账 | IO dispatch 与 turn 发布的并发反例通过，无 pending ticket 泄漏 |
| B2 Durable outcome | T003 | B1 | publish 前后取消、deadline、close 与持久记录/handle 一致 |
| B3 Secure export | T004 | B2；技术上独立，默认顺序执行 | 0600 原子导出、失败保留、loader round-trip |
| B4 Caller convergence | T005 | B1–B3 | 当前 caller/mode 清单闭合，支持模式默认 native |
| B5 Qualification and handoff | T006/T007/T008 | B4；矩阵盘点可提前只读进行 | 继承全部验收义务逐项有结果；本地资格通过，外部边界明确 |

## Code Design and Review

B1：authority transport 仅在 worker 等待，将 Core 提交封送到 postToIo；Operation 字段明确
线程 owner/mutex，锁内快照、锁外 bind、锁内检查 Pending/attempt 后发布，失败清理未发布 ticket。
B2：确定 durable publish 与成功结果的同一线性化决定；FINALIZE best-effort 不撤回 committed parent。
B3：受限临时文件、检查写入/关闭、既有持久性要求与原子替换；明确 symlink 策略。
B4：先生成真实 caller/mode 清单再按共享后端分组，不能机械按历史16调用点逐点构建。
B5：先盘点原 FR/CD/INV/PO/I、组件与 harness 未关闭项，再补缺失实现/fixture，静态合成门后才正式验收。

逐任务明确加载 `/home/tianxing/.codex/skills/review-agent/SKILL.md` 进行只读审查，记录路径/hash、
基线、完整 diff、调用方/测试、finding 和复审。批末按
`skills/speckit-code-design/references/batch-quality-gates.md` 组合审查再共享编译测试。
不得在 T001 后因一个小修改立即重跑全套；也不得把 T007 当成所有前置定向 C++ 验证的唯一时点。
原生断言、fixture/driver、oracle 为 C++；Face/IO/scheduler/callback owner 活到任务 drain/join。

## Coverage and Evidence Contract

每批一个简短 `evidence/bN-result.md`，状态唯一在 tasks.md。五 lane 必须填实际符号/命令或 gap：
生产入口/调用方；实现/wire；测试/harness/oracle；build/source closure；迁移/证据。
每条 scope 见 spec 的 entry 和 transfer matrix；开始实施时补具体 diff、target 和 selector，不能预填 PASS。
新链接边界需符号定义 TU/target 与 nm/readelf 对照。记录 review trace、Batch growth decision、
Closure decision 和 static/compile-link/runtime-test/unobserved miss；初始均未执行。
审查技巧沿用182审计后 R12 的线程读写表、线性化点、失败清理、wire 权威溯源和 production fixture 检查。

## Document Size Control

tasks.md 只保留当前 registry、当前 checkpoint 与任务，不追加逐命令时间线。
证据按批次单文件维护；原始日志放 .codex-tmp，不入 Git。新增问题先归现有批次，范围变更须说明
为何不能归入；已完成历史留182或批次证据，不重复抄进 spec/plan/tasks。
