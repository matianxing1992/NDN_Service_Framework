# Spec189 Traceability

本表使用活动任务 ID；旧 T002/T004→T003、T008/T010→T009 是合并，不是 PASS。
五 lane 与动态 profile 见 [batch-execution.md](batch-execution.md)。

**Acceptance target**: `MiniNDN + Qwen/Qwen3-0.6B + 2 execution Provider nodes
(`ucla`, `arizona`) + NDNSF-DistributedInference native C++ inference`。支撑角色
不计入两个 execution nodes；只有完整 prepare/Repo/ACK/Selection/fetch/assembly/
handoff/terminal/drain 链路才可满足最终 SC。

| Requirements | Active tasks | Batch | Production proof / evidence |
| --- | --- | --- | --- |
| FR-001 | T001,T003 | B189-0,B189-1 | pinned snapshot/config/tokenizer/digests；b189-prepare.md |
| FR-002, FR-003, FR-004, FR-005 | T003 | B189-1 | native prepare 原子材料、Repo commit/可达性/owner/hot reuse；system-wide source-cache hash/size reuse；b189-prepare.md / b189-r155-source-cache-memory-boundary-20260920.md |
| FR-006 | T003,T009 | B189-1,B189-5 | production PreparedModel reference-only；同 handle 两请求零发布在最终真实运行收口；b189-prepare.md / b189-convergence.md |
| FR-007, FR-008, FR-009 | T005 | B189-2 | 真实 ACK 后规划、signed Selection、生产 no-fetch negatives；b189-placement.md |
| FR-010..FR-011 | T006 | B189-3 | Repo selected/shared materials→有界 native assembly；source protobuf release、authenticated role-boundary rebuild、worker runner boundary、r161 lineage diagnosis 与 r162 controlled stop；b189-execution.md / b189-stage-materialization-unit-20260921.md / b189-r156-worker-release-lineage-boundary-20260920.md / b189-r161-lineage-validation-diagnostic-20260920.md / b189-r162-cache-compatibility-stop-20260920.md |
| FR-012..FR-013 | T007,T009 | B189-3,B189-5 | NDN handoff、独立 C++ 输出 reference；lineage core/edge validation now installed but runtime handoff/output remains open；b189-execution.md / b189-r161-lineage-validation-diagnostic-20260920.md / b189-convergence.md |
| FR-014..FR-015 | T003,T006,T007,T009 | B189-1,B189-3,B189-5 | package/publisher eviction、request drain 与 bounded idle cache 分账；b189-prepare.md / b189-resource.md / b189-execution.md |
| FR-016, FR-017, FR-018 | T003,T006,T009 | B189-1,B189-3,B189-5 | host guard 前置；native counters 随 owner 交付；T009 完整采样/峰值；对应批次 evidence |
| FR-019 | T009 | B189-5 | 两次完整成功、各自同 handle 复用；b189-convergence.md |
| FR-020..FR-021 | T001,T009 | B189-0,B189-5 | candidate/run 身份分离，stale candidate preflight rejection；b189-convergence.md |
| FR-031 | T006,T009 | B189-3,B189-5 | 显式、默认关闭的 cache-compatibility diagnostic；requester metadata-only receipt、Selection 后 plain source cache identity/hash/size 校验与 Repo fetch skip；static evidence / r164 admission disk-boundary evidence，materialization runtime 待有足够磁盘后重试，不计正式资格 |
| FR-022 | T003,T005,T006,T007,T009 | B189-1..B189-5 | C++ native assertions / Python orchestration；对应唯一批次记录 |
| FR-023..FR-024 | T009 | B189-5 | raw logs/four miss classes/no SIF/Tiger；b189-convergence.md |
| FR-025 | T001,T009 | B189-0,B189-5 | 已验全局依赖闭包复用；b189-build-20260918.md / b189-convergence.md |
| FR-026 | T001,T003,T005,T006,T007,T009 | B189-0..B189-5 | 候选局部契约不进入全局 API；plan audit correction 与最终 candidate evidence |
| FR-027 | T003,T006,T009 | B189-1c,B189-3,B189-5 | preparation category owners/peak、materialization and post-drain counters；b189-prepare.md / b189-resource.md |
| FR-028 | T003 | B189-1c | committed+staged+range reservation quota、replacement rollback and mixed-write C++ selector；b189-prepare.md |
| FR-029 | T009 | B189-5 | generation-guarded handle install/close interleaving；b189-convergence.md |
| FR-030 | T006,T007,T009 | B189-3,B189-5 | committed Selection 的 per-provider/role progress allowlist、monotonic freshness 与真实 stream liveness；b189-r4-progress-heartbeat.md / b189-native-r70-progressed-stream-boundary-20260919.md |

## Success criteria

SC-001 → T003,T009；SC-002 → T005,T009；SC-003 → T006,T007,T009；
SC-004 → T006,T007,T009；SC-005 → T003,T006,T009；SC-006 → T009；SC-007 → T006,T007,T009。
所有旧 negative/recovery obligation 保留在归属任务，不另建字段级任务。
已有 selector PASS 只关闭其实际覆盖范围，不能代替最终 SC。
