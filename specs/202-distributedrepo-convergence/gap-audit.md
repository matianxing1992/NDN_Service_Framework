# Gap Audit: Current DistributedRepo vs Target

**Date**: 2026-10-08 12:24 -05:00
**Mode**: STATIC_ONLY
**Product readiness**: BLOCK
**Baseline**: HEAD `0957f675b230e89278f2fab96e30023353e5958e` +93 tracked modifications/26 untracked entries（含目录）；不是 clean checkout。静态审计不启动测试，不以旧 checkbox 推断当前完整通过。

## Findings

| ID | Severity / class | Current evidence | Ideal / correction | Owner task |
| --- | --- | --- | --- | --- |
| G001 | HIGH / PRODUCT_DEFECT | `ArtifactRepositoryApi.cpp:3257–3262,3628–3644` 先 rename 替换 destination，随后 fsync/deadline 可抛错；renamed 后不恢复旧文件 | 保留旧 namespace 至最终同步与 deadline 接受；失败恢复；fatal rollback 单列且不重试；不移除现有 post-sync deadline | T002 |
| G002 | HIGH / PRODUCT_DEFECT | `RepoNode.cpp:802–809` source-failure 分支将所有 pending 目标改成 NOT_ATTEMPTED；B已STORE/timeout/count1可被当作SKIPPED | count0才未尝试；count>0 UNKNOWN/unknown-outcome，保留 attempt/迟到提交不确定性 | T003 |
| G003 | MEDIUM / PRODUCT_DEFECT | `LegacyRepoImporter.cpp:1553–1559` no-replace rename后parent fsync可失败；`1820–1827,1837–1853` 已存在且验证通过的目标直接返回，没有重试该屏障 | 暴露完整目标与 durable acknowledgment 分开；两条幂等恢复分支重建parent durability，保留源/完整目标，不臆造全局rollback | T004 |
| G004 | HIGH / SOURCE_CLOSURE_GAP | 当前Repo/DI/native binding/harness大量未提交；branch36 commits不能复现实际116-source/173-input候选 | 按owner复核overlay，明确commit/不可变bundle与可重建依赖，不混入keys/models/results/generatedbuild | T001,T009 |
| G005 | HIGH / QUALIFICATION_GAP | Spec193各批次不同candidate/profile局部PASS；T007未完成 | 最终一致source family+声明profile组合全部受影响门和完整矩阵两轮；旧PASS保留历史 | T008–T010 |
| G006 | MEDIUM / DOCUMENT_CONTRADICTION | A19派生tasks/coverage/traceability要求artifact availability分类传统segmented parent；`RepoNode.cpp:232–238`要求artifact-v2-bytes | A19验证artifact缺失/截短，O07验证真实seg/1缺失，A20验证offline，三个平面分别映射，不增加跨plane API | T005 |
| G007 | MEDIUM / ORACLE_GAP | `_signed_packets`仅解析outer Data，遗漏Sync Interest参数中的签名Data和IP重组；A21离线attempt因此PARTIAL | 解析签名MappingData、caller/controller一致序列、去重publication；Provider顺序明确是ordinal推断，不伪称body有targetProvider | T006 |
| G008 | HIGH / QUALIFICATION_GAP | O17多个子场景跨候选诊断，尚无统一当前完整出口 | 正确区分metadata grant和原始Data producer encryption，在统一候选完成route/failover/location corruption/drop/import/cache/server-metadata-auth | T005,T010 |
| G009 | MEDIUM / AUDIT_COVERAGE_GAP | native审查覆盖主要生命周期/worker/读取/迁移路径；没有声称逐行覆盖全部parser、公开local/object入口、Core内部与所有参数 | 逐入口双向清单、strict malformed/boundary审查与具名selector；未知项明确NOT_EVALUATED，不能写“未发现缺陷=全覆盖” | T001,T007,T009 |
| G010 | MEDIUM / WORKFLOW_GAP | Context Mode最初缺当前project ContentDB；canonical helper已重建index，active guard通过，但project guard仍报NO_REAL_SESSION_EVENTS，MCP retrieval为空。CodeGraph已up-to-date；fullsource/实验权威仍是仓库 | 指针202/managedplan/index同步；本轮采用仓库fallback，不把index修复或ctx_stats冒充host capture/retrieval通过 | T001 |

G001/G002 是当前可由源码演示的错误；G003 是 durable recovery 缺口。G004/G005/G008/G009 不能被描述成已观察到的 runtime 产品故障。没有额外缺陷证据时不为理想状态添加新协议。

## Architecture Comparison

| Dimension | Existing production shape | Target and remaining distance |
| --- | --- | --- |
| ownership | native API、RepoNode、filesystem stores和Core/ServiceUser；Python facade保留native owner | 保持现有owner，不重写Core或新增Python catalog；需修复事务尾部/target ledger，并完成逐入口审计 |
| lifecycle | In-App local durable root、显式archive、Persistent seed barrier/worker/receipts | 不是从零实现；当前源故障后attempt事实错误需修复，restart/latecommit全集需重新资格 |
| reads | LOCATION/full reference/digest、Provider visited/cooldown、stage/rename/deadline | G001破坏失败原输出不变；rollback与恢复诊断缺设计/回归 |
| availability | artifact receipt fence与presence/integrity；传统对象manifest/segments | G006属于派生文档跨plane要求错误，不证明缺少新的产品availability API |
| original Data | DATA_RECEIPT/DATA_LOCATION +原名hint/wire路径 | 必须保留producer-encryption边界；多诊断不是完整当前O17资格 |
| import | native schema-gated离线snapshot/WAL/SHM/CAS、staging/no-replace/idempotency | G003 parent-fsync恢复屏障缺失；不声称observer抛错保证覆盖所有rename后错误 |
| language/DI | native pybind、strong owner/GIL释放、generic identity适配和实际DI复用 | P批次已有局部证据；全候选parity、cache授权和部署source closure仍需正式资格 |
| reproducibility | hash-bound preflight、拓扑与抓包、run manifests | HEAD/overlay未收口；全矩阵/全部受影响tests/动态参数门未闭合 |

## Source Identity

- `ArtifactRepositoryApi.cpp`: `531ad4306b51519c3ae821e81a1850250440623e8c97c3502a19817c318d01dd`。
- `RepoNode.cpp`: `5b75717f2d3a2b7f2eedf3acdbb71bc3aaf255bfa73e7313a5d2be0fb31997d0`。
- `LegacyRepoImporter.cpp`: `028a19173b48410064038d22dfb519f822215afff64145c69804aab7c37de0a5`。
- 完整repair baseline：`../193-distributedrepo-multinode-api-campaign/evidence/t007-native-repair-review-20261007/baseline-complete.zip`，SHA`b1719d83c3ed842911d17a0c7ba1d1da57bd6c86b192c53faf54c08f48f62672`；原partial baseline不可充当完整快照。

## Static Coverage Matrix

| Lane | Actual files/symbols and query | Result / gap |
| --- | --- | --- |
| entry/callers | `codegraph node ArtifactRepositoryApi.cpp --offset 3210/3580`、Python `artifact_repository_bindings.cpp --offset 23/399/458`、NativeServiceUser owner/stop | 真实API/borrowed owner/GIL路径核对；不声称全部调用方已逐行审完 |
| implementation/wire | `RepoNode.cpp --offset 802`、`LegacyRepoImporter.cpp --offset 1548/1815`；既有 fullref/receipt/LOCATION/worker审查 | G001–003源码已复核；parser/更多对象入口归T001/T007/T009 |
| tests/oracle | 既有inapp/persistent native fixture、真实缺段/availability日志、signedSync解析设计；`tests/wscript:341/371` | 缺post-rename故障和source-after-timeout回归；离线oracle未接入当前harness |
| build/source | Repo `wscript`静态库及targets、116-source/173-input历史lock、当前git状态 | SOURCE_CLOSURE_GAP；本轮NOT_RUN，不伪称构建/链接通过 |
| migration/evidence | Spec192/193 spec/plan/tasks/coverage、failure-log、last A06 manifest和新supersession | 历史证据保留；停止旧execution；formalqualification BLOCK |

## Evidence Limits and Last Run

`a06-large-diag-001` manifest PASS、minindn_cleanup PASS，锁`278aca45017462ca66c9e9da32dc95d80f6b4d2ea8cf5f47d03dbdbf80fb6269`；仅一轮131123B功能诊断，不是完整双轮或T007资格。本轮只读取终态manifest，没有重跑/改原证据。旧各批次的有限children、storage/packet oracles应按原run单独引用。

静态推断不能证明断电耐久性、任意I/O错误恢复、负载性能或Tiger表现。当前产品readiness BLOCK；新Spec文档可审阅并按依赖实施，不能把文档结构PASS作为产品完成。
