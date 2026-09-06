# Development Scope and Experiment Transfer

**Status**: current (scope decision); development IN_PROGRESS
**Evidence layer**: documented (owner-authorized scope and responsibility change)

## Owner Decision

2026-09-06 所有者明确确认“现在落实：本机开发与本地验证，SIF/Tiger
移交实验机器”，并补充 Git 合并等当前开发完成后再讨论。
长期分工按开发/实验，Qwen 与 YOLO 只是当前模型案例。

## Scope Mapping

| Before | Revision 7 | Remaining acceptance |
|---|---|---|
| Goal / US4 到 SIF 和一次 Tiger 请求 | 本地开发验证与可复现版本交付 | T001--T009、T012 的全部适用本地验收 |
| FR-008 候选/SIF | 同源开发交付封存 | T009 源/配置/依赖/工件/证据摘要、复现和完整性校验 |
| FR-009 Tiger 执行 | 实验交接与版本反馈契约 | T009/T012 材料完整；不要求远端签收或实验 PASS |
| SC-005 exact-SIF/Tiger | 可复现本地交付与移交材料 | 单一 commit 身份、完整引用/摘要、owner/反馈字段 |
| SC-006 单一功能裁决 | LOCAL_DEVELOPMENT_PASS | 本地验收齐全；不得声明 SIF/Tiger/GPU 资格 |
| T010/T011 | TRANSFERRED，原 ID/外部验收保留 | 实验机器另行构建、replay、执行并反馈 |

规范交接见 [handoff contract](../handoff-contract.md)。本机仍要求
真实授权链、共享 runtime、unit/integration 与本地 MiniNDN 同源
Y-A/Y-B/Y-N 全矩阵，不能用这次调整关闭 A05 的本地配置/构建身份缺口。

## Progress Boundary

活动任务从 12 个变为 10 个；已完成仍为 T001/T002/T003/T004/T006
五个，另两项只记 TRANSFERRED。T005/T007/T008/T009/T012 仍未完成。
转交没有完成任何新增实现或测试，也没有发出本地关闭裁决。
冻结 Spec180 文档与证据不改，旧部署条款的责任去向在本记录明示。

Git 合并未分析、未执行。实验机器没有收到工具发出的消息/文件，
本机没有构建 SIF、运行 Tiger 或修改其部署脚本；交接契约规定的是
后续交付与反馈责任，不冒充已发生的跨机器动作。

## Document Validation

`audit_speckit_structure.py --strict` PASS：15 FR、6 SC、4 stories、
10 active tasks、5 complete、15 FR traced。保留 T010/T011 原 ID
导致一个非连续编号 warning；这是显式移交的结果，非丢失任务。
活动集合精确为 T001--T009/T012，移交集合为 T010/T011；独立集合/
依赖检查 PASS，修改文档的 Markdown 链接目标均存在，`git diff
--check` PASS。以上只验证文档与责任映射，不是本地资格结果。
