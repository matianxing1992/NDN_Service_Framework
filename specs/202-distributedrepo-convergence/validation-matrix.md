# Validation Matrix: DistributedRepo Convergence

<!-- SPEC202 QUALIFICATION STATUS START -->
**Status**: NOT_RUN for current Spec202 qualification.
<!-- SPEC202 QUALIFICATION STATUS END -->
**Source definitions**: [Spec193 stable cases](../193-distributedrepo-multinode-api-campaign/coverage-matrix.md#stable-cases)作为保留的historical input；原59 case描述/oracle完整继承。历史candidate PASS不填入当前round。parent owner见traceability，最终T010统一资格。

## Inherited Stable Cases

| ID | Scenario / public entry | Independent oracle | Closing owner | Round 1 | Round 2 |
| --- | --- | --- | --- | --- | --- |
| A01 | In-App 小文件 publish/fetch、local commit | app 根文件和 SQLite、源/目的 SHA-256、远端零 STORE | T001/T009/T010 | NOT_RUN | NOT_RUN |
| A02 | In-App 大文件跨 chunk、应用进程重启后 reopen | 新进程 PID、chunk 清单、完整 SHA-256 | T001/T009/T010 | NOT_RUN | NOT_RUN |
| A03 | In-App 已提交 payload 删除/损坏后 fetch | 历史 local receipt 不变、目的文件不变、无成功返回 | T001/T009/T010 | NOT_RUN | NOT_RUN |
| A04 | In-App 非法 reference/路径及 Persistent 非法配置 | 零新本地/远端状态、零注册 handler | T001/T009/T010 | NOT_RUN | NOT_RUN |
| A05 | 真实 Provider capability discovery，不选择不写入 | 三个认证签名 identity、ACK/抓包、零新 receipt | T001/T009/T010 | NOT_RUN | NOT_RUN |
| A06 | 单目标 foreground publish 和 fetch | seed SQLite/文件/receipt、独立消费者 digest | T001/T009/T010 | NOT_RUN | NOT_RUN |
| A07 | 三目标 `ALL` 发布；仅选定集合写入 | 三个不同 principal 的 receipt/文件，未选节点零写 | T001/T009/T010 | NOT_RUN | NOT_RUN |
| A08 | `ONE`/`QUORUM` 部分可用与失败目标 | required/achieved、每 Repo 状态及不可变收据匹配 | T001/T003/T009/T010 | NOT_RUN | NOT_RUN |
| A09 | selected set 非法 seed、重复 principal、错误前缀 | 失败前零 STORE/receipt，值类型验证错误 | T001/T009/T010 | NOT_RUN | NOT_RUN |
| A10 | beginPublishFile seed barrier、异步进度/终态 | seed 持久 receipt 先于 handle，随后 owner journal/replica rows | T001/T009/T010 | NOT_RUN | NOT_RUN |
| A11 | 相同幂等键同内容重放；不同内容冲突 | 同一 operation ID、唯一 receipt、冲突零新状态 | T001/T009/T010 | NOT_RUN | NOT_RUN |
| A12 | owner/Repo 重启恢复 `IN_FLIGHT` | 新进程 epoch、同操作 ID 重放、终态唯一 receipt | T001/T009/T010 | NOT_RUN | NOT_RUN |
| A13 | 显式 beginArchive/archive 后 In-App 与 Persistent handle 分离 | archive 前远端零写、两类 receipt 不合并、同 reference | T001/T009/T010 | NOT_RUN | NOT_RUN |
| A14 | source/seed/target 失败或超时 | 失败无虚假 COMMITTED；暂存清理与后续重试可用 | T001/T003/T009/T010 | NOT_RUN | NOT_RUN |
| A15 | 新消费者跨进程 fetch，原应用文件已移除 | 网络 FETCH、字节 digest、原路径不存在 | T002/T010 | NOT_RUN | NOT_RUN |
| A16 | getReceipt 对应/不对应 handle；旧 receipt 不变 | 查询目标身份、SQLite 历史行、错误 ID 拒绝 | T005/T010 | NOT_RUN | NOT_RUN |
| A17 | metadata availability：可达且完整 | 当前 LOCATION/AVAILABILITY、文件元数据 | T005/T010 | NOT_RUN | NOT_RUN |
| A18 | verify_content availability：完整/损坏 | 全量生产字节 digest、历史 receipt 不变 | T005/T010 | NOT_RUN | NOT_RUN |
| A19 | 可达但artifact-v2对象缺失或payload截断；传统segment缺失另见O07 | 存储缺口与 ABSENT/PARTIAL，非 heartbeat 猜测；两种object plane分别验收 | T005/T010 | NOT_RUN | NOT_RUN |
| A20 | 单 Repo 离线与全离线 | process/NFD epoch、UNKNOWN/失败类别、不写目的文件 | T005/T010 | NOT_RUN | NOT_RUN |
| A21 | 三身份 failover、别名去重和 cooldown | NDN Interest/attempt 序列、每 principal 最多一次、内容 digest | T006/T010 | NOT_RUN | NOT_RUN |
| A22 | 旧 handle 遇到新位置版本/过期 hint | 旧回执逐字不变，当前 LOCATION version 增长 | T005/T010 | NOT_RUN | NOT_RUN |
| O01 | 原子 put/get、manifest/list、重复写同名 | 原始 bytes、RepoCore catalog/文件、对象摘要 | T007/T010 | NOT_RUN | NOT_RUN |
| O02 | 非连续 range 写、commit 与 getRange | 已写区间及空洞、完整内容、manifest 长度 | T007/T010 | NOT_RUN | NOT_RUN |
| O03 | abortRange、错误 owner、缺段/越界 commit | 暂存容量释放、无可见半提交、稳定错误 | T007/T010 | NOT_RUN | NOT_RUN |
| O04 | remove 存在/不存在对象及之后的 list/get | catalog epoch/文件删除、读取失败、幂等返回 | T007/T010 | NOT_RUN | NOT_RUN |
| O05 | 读取缺失/损坏对象，保留已有目的内容 | 独立文件损坏、API 失败、无假清单 | T007/T010 | NOT_RUN | NOT_RUN |
| O06 | 大于单 packet 的 segmented put/getObject | 分段个数/字节、APP 侧完整 digest | T007/T010 | NOT_RUN | NOT_RUN |
| O07 | segmented 缺段、重复段或错误 digest | 段库存与失败类别，禁止返回部分 bytes | T007/T010 | NOT_RUN | NOT_RUN |
| O08 | 原始 signed Data packet put/get | 逐字相同 wire、Data name 和 KeyLocator | T007/T010 | NOT_RUN | NOT_RUN |
| O09 | Data packet set 顺序与缺失索引 | 按 manifest 顺序逐字校验；缺失时整体失败 | T007/T010 | NOT_RUN | NOT_RUN |
| O10 | insert 已发布 signed Data 与 status | Repo 操作行、原始 wire、跨节点读取 | T007/T010 | NOT_RUN | NOT_RUN |
| O11 | insertPayload 自动分段与 APP 校验 | 每段 Data name/wire、重组 digest、签名 identity | T007/T010 | NOT_RUN | NOT_RUN |
| O12 | insert 同名冲突或签名/名称不匹配 | 零新 committed 对象与稳定拒绝 | T007/T010 | NOT_RUN | NOT_RUN |
| O13 | cache status 与对象插入/读取变化 | backend cache 实际项与报告匹配 | T007/T010 | NOT_RUN | NOT_RUN |
| O14 | catalog status/snapshot/delta/lookup 创建后 | 独立 catalog 行/epoch、snapshot/delta 内容 | T007/T010 | NOT_RUN | NOT_RUN |
| O15 | catalog delta 删除后及 stale epoch | tombstone/epoch、lookup 不返回已删对象 | T007/T010 | NOT_RUN | NOT_RUN |
| O16 | inventory/manifest/status 与真实存储对照 | 存储 DB、payload、wire name，未提交对象不可见 | T007/T010 | NOT_RUN | NOT_RUN |
| O17 | 分布式 receipt/当前位置选择后的原名 Data Interest | 各 Repo 已提交 receipt、当前认证位置、每条 Interest 原名及非空副本 hint、原始 wire/签名、失败切换 | T005/T010 | NOT_RUN | NOT_RUN |
| L01 | local registry put/get/list/remove 与远端隔离 | 本地 handler 调用、远端抓包零该对象请求 | T007/T010 | NOT_RUN | NOT_RUN |
| L02 | local range commit/abort、错误输入 | 本地暂存/对象行与错误后无半提交 | T007/T010 | NOT_RUN | NOT_RUN |
| L03 | local insert/status、catalog/cache | 本地 Data wire 与行状态、网络零请求 | T007/T010 | NOT_RUN | NOT_RUN |
| L04 | local segmented/object 读取和缺段拒绝 | 本地分段文件、重组 digest、零远端读取 | T007/T010 | NOT_RUN | NOT_RUN |
| N01 | requestCapability/cacheStatus 与 selected service | ACK signer 和响应 wire、未选 Provider 无最终 Selection/写入；广播 ACK 可出现且须验签 | T007/T010 | NOT_RUN | NOT_RUN |
| N02 | requestStore/StoreRange/CommitRanges/AbortRanges | 授权网络 trace、Repo 存储和无半提交 | T007/T010 | NOT_RUN | NOT_RUN |
| N03 | artifact range/store/commit wrapper | artifact receipt transaction、原始 payload digest | T007/T010 | NOT_RUN | NOT_RUN |
| N04 | requestFetch/FetchRange/Manifest/Inventory | 响应 wire、字节窗口和 catalog 实况 | T007/T010 | NOT_RUN | NOT_RUN |
| N05 | requestDelete/Status | 删除前后 DB/文件、operation row | T007/T010 | NOT_RUN | NOT_RUN |
| N06 | requestInsert | 原始 signed Data wire 和状态行 | T007/T010 | NOT_RUN | NOT_RUN |
| N07 | requestLocation/Receipt/Availability | 直连预期身份、签名、历史与当前状态区分 | T007/T010 | NOT_RUN | NOT_RUN |
| N08 | 错误 signer、无权限、超时、malformed reply | 零成功回调/零提交，网络 trace 与错误码 | T007/T010 | NOT_RUN | NOT_RUN |
| N09 | generic `requestSelectedService` 调用 CATALOG_STATUS/SNAPSHOT/DELTA/LOOKUP | catalog epoch/行、签名响应 wire、未选 Provider 无最终 Selection/写入；广播 ACK 可出现且须验签 | T007/T010 | NOT_RUN | NOT_RUN |
| N10 | Persistent 服务拒绝传统对象 STORE/range/INSERT/STORE_MANIFEST/DELETE | 有效签名请求收到稳定拒绝、artifact 与对象存储零新状态 | T007/T010 | NOT_RUN | NOT_RUN |
| P01 | C++ 发布→Python 查询回执/取回 | 同 receipt/reference、Python 进程 digest | T008/T010 | NOT_RUN | NOT_RUN |
| P02 | Python 发布→C++ 跨节点取回 | C++ receipt/reference、Repo 行、bytes digest | T008/T010 | NOT_RUN | NOT_RUN |
| P03 | DI model/input/output 授权新 Provider 使用 | 三类 reference、Repo FETCH、Provider 输出 | T008/T010 | NOT_RUN | NOT_RUN |
| P04 | DI 未授权与 warm-cache 拒绝 | 无内容落地、无 handler 完成、无临时 Repo 写入 | T008/T010 | NOT_RUN | NOT_RUN |
| P05 | 显式 legacy import 后独立消费者读取 | 源 DB/sidecar/payload hash 不变、目的 inventory/wire | T004/T010 | NOT_RUN | NOT_RUN |
| P06 | 退休 Python API/legacy object facade 显式拒绝 | 版本化错误、零网络请求和零存储变化 | T008/T010 | NOT_RUN | NOT_RUN |

## New Closing Native Regressions

| ID | Production entry / parameter classes | Required oracle | Task | Status |
| --- | --- | --- | --- | --- |
| R01 | Persistent fetch postrename EIO,existing/absent | actualhook、oldinode/bytes/absence、zeroverified、normalresidue0、healthyfollowup | T002 | PASS native/dynamic/sealed d75cf28e |
| R02 | Persistent fetch directorysync crossesdeadline,existing/absent | actualpostrenamehook/原有post-sync检查、return-time恢复和attemptfailure | T002 | PASS native/dynamic/sealed d75cf28e |
| R03 | In-App fetch postrename EIO,existing/absent | publicnativeAPI异常、oldinode/bytes/absence、normalresidue0 | T002 | PASS native/dynamic/sealed d75cf28e |
| R04 | rollback namespace/sync failure | explicitfatalrecovery、定位材料、禁止下一Provider覆盖；不谎称已恢复 | T002 | PASS native/dynamic/sealed d75cf28e |
| R05 | acceptedcommit backupcleanup failure | 成功仍成功、完整verifiedbytes、residue可诊断/维护 | T002 | PASS native/dynamic/sealed d75cf28e |
| R06 | 3targetALL：Btimeout→Csourcefailure；reopen后seed missing/corrupt | late：BUNKNOWN/count1/nonSKIPPED、CFAILED/noSTORE、late-responseimmutable；early：BUNKNOWN/count1、CNOT_ATTEMPTED/count0/zeroSTORE；seedreceipt1/terminalaggregate不变 | T003 | PASS native/dynamic/sealed d75cf28e |
| R07 | import parentfsync failure afterpublish/replay | actualbarrierhit、tworeturnbranch/successreplay/repeatedfault、source+inventory/no-replace | T004 | PASS native/dynamic/sealed d75cf28e |

R01–07是独立故障等价类，不把每种参数拆成顶层任务。native unit/integration先关闭bugplane；formalnetworkmatrix在T009convergence后执行。

## Mandatory Subscenarios and Profiles

- A06：2048B、4401B ranges、131123B larger foreground+independentconsumer；lastoldlarge只有single diagnostic，不声明current二轮。
- A07：7hosts，3selected+1online unselected sentinel；A08各ONE/QUORUM/reject，3Provider使用6hosts。
- A14：source/target/seedfailure；A19artifactmissing/truncated vsO07realsegmentloss vsA20offline分别记录。
- A21：sameAPI连续fetch、aliases、cooldown；signedSyncpublication与nativeattempt序列、ordinal推断分类。
- N08：denied、timeout、malformed、wrong-signer各closingnegative，不合并成一条“负例通过”。
- O17：normalroute、failover、corrupt-location、drop-original-data、legacyimport、cachehit、servermetadataauth；runnerflags互斥，逐subscenario做两round。
- P01/02双向crosslanguage；P03/04实际authorizedDI三材料/coldwarmdenial；P05nativeCLIimport/源不变；P06全部retired接口exactrejection/zeroeffects。
- Cross-run：传统object和artifact分别freshMiniNDNseed/read，Repo根保留/newepoch/noSTORE/nooriginalsource。

## Evidence Index Rules

每行关闭前填freshrun路径、common-source hash、profilelock、effectiveflags、signed/storage/consumeroracle、finitechild与expectedserviceexit、每hostpcap/loadedlibs/cleanup。round1/round2必须同source+同declaredprofile，不继承旧PASS字符串。单case可在隔离拓扑内连续但状态可复位；无法复位fresh重建。任何缺oracle/异常exit/cleanup或unknown保持OPEN/NOT_RUN。

动态与native回归的selector、source/toolchainhash、输出、预算和未抑制分类保存在既有evidence日志/JSON中；当前执行进度由tasks.md唯一维护，本表仅记录完整资格的逐项证据。
