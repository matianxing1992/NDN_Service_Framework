# Spec189 Traceability

本表使用活动任务 ID；旧 T002/T004→T003、T010→T009 是合并，不是 PASS。
五 lane 与动态 profile 见 [batch-execution.md](batch-execution.md)。

| Requirements | Active tasks | Batch | Production proof / evidence |
| --- | --- | --- | --- |
| FR-001 | T001,T003 | B189-0,B189-1 | pinned snapshot/config/tokenizer/digests；b189-prepare.md |
| FR-002..FR-005 | T003 | B189-1 | native prepare 原子材料、Repo commit/可达性/owner/hot reuse；b189-prepare.md |
| FR-006 | T003 | B189-1 | production PreparedModel reference-only、同 handle 两请求零发布；b189-prepare.md（旧组件证据保留 b189-placement.md） |
| FR-007..FR-009 | T005 | B189-2 | 真实 ACK 后规划、signed Selection、生产 no-fetch negatives；b189-placement.md |
| FR-010..FR-011 | T006 | B189-3 | Repo selected/shared materials→有界 native assembly；b189-execution.md |
| FR-012..FR-013 | T007,T009 | B189-3,B189-5 | NDN handoff、独立 C++ 输出 reference；b189-execution.md / b189-convergence.md |
| FR-014..FR-015 | T008,T006,T007,T009 | B189-4,B189-3,B189-5 | fixture owner/drain、真实 runner/lease/materialization baseline；b189-resource.md / b189-execution.md |
| FR-016..FR-018 | T008,T009 | B189-4,B189-5 | 前置资源 guard、1-second sample、受控停止及真实峰值；b189-resource.md / b189-convergence.md |
| FR-019 | T009 | B189-5 | 两次完整成功、各自同 handle 复用；b189-convergence.md |
| FR-020..FR-021 | T001,T009 | B189-0,B189-5 | candidate/run 身份分离，stale candidate preflight rejection；b189-convergence.md |
| FR-022 | T003,T005,T006,T007,T008,T009 | B189-1..B189-5 | C++ native assertions / Python orchestration；对应唯一批次记录 |
| FR-023..FR-024 | T009 | B189-5 | raw logs/four miss classes/no SIF/Tiger；b189-convergence.md |
| FR-025 | T001,T009 | B189-0,B189-5 | 已验全局依赖闭包复用；b189-build-20260918.md / b189-convergence.md |

## Success criteria

SC-001 → T003,T009；SC-002 → T005,T009；SC-003 → T006,T007,T009；
SC-004 → T008,T006,T009；SC-005 → T008,T009；SC-006 → T009。
所有旧 negative/recovery obligation 保留在归属任务，不另建字段级任务。
已有 selector PASS 只关闭其实际覆盖范围，不能代替最终 SC。
