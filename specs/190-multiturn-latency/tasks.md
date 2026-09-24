# Tasks: Multi-turn Token Generation Latency

**Status**: PARTIAL
**Input**: [spec.md](spec.md), [plan.md](plan.md), [design contract](contracts/design.md)

## Current Checkpoint

2026-09-23 21:09 -05:00：B190-28 将默认 protected grant factory 的测试 transport
从内存 map 返回改为 Provider-owned Face 上的 exact signed Data 往返。首次运行因
测试 Data 未签名在 `exact grant Data fetch failed` 停止；计数证明 publication、exact
Interest 和 Data 投递均已发生，修复后 focused C++ selector `14/14` 通过，独立重复
`r5/r6/r7` 全部通过，均观察到 `GRANT_VERIFIED → RUNNER_READY →
EXECUTION_COMPLETED → TERMINAL`。这只是 T006 的 Face-backed exact-fetch sub-gate，
仍不是真实 NFD/MiniNDN route qualification；online Controller、durable key recovery、
revoke/rebind、精确 missing-object negative 和完整 protected restart 未闭合。T006
继续 `PARTIAL`，T007 继续锁定。详见 [B190-28](evidence/b190-28.md)。

2026-09-23 20:56 -05:00：B190-27 完成默认 protected grant factory 的 C++ production-wiring
sub-gate。`Provider::serve` 不再被 test runtime factory 覆盖；默认 factory 通过
operator registry、exact grant-name fetch、`GRANT_VERIFIED`、assembly、`RUNNER_READY`、
`EXECUTION_COMPLETED` 和 `TERMINAL`。focused selector 13/13 连续 4 次通过，普通根
`spec185-provider-assembly` 21/21 cases、224/224 assertions 通过。首次有效运行前修正了
fixture 把 prepared model 名误写成 registry family 的问题；原始失败保留。一次完整
`spec185-prepared-request` 全量回归在 `PreparedRequestsSharePackageButAllocateIndependentIds`
处 `DI_NATIVE_PREPARATION_TIMEOUT` 后停止，未计 PASS。该 gate 仍使用生产批准的
transport dependency，不等同于真实 NFD route-backed grant transport；online Controller
confirmation、revoke/rebind、精确错误 fetch 和完整 T006 未闭合。详见
[B190-27](evidence/b190-27.md)。T007 继续锁定。

2026-09-23 20:40 -05:00：B190-26 完成 T006 默认 protected grant factory 的静态接线审查。
确认 `Provider::serve` 安装 production factory，`NativeProviderHandler` 在已校验
Selection projection 后调用并校验 binding；但现有 served-Provider C++ fixtures 都通过
`testProtectedRuntimeFactory` 覆盖默认 factory，独立 grant target 也绕过了 Provider/Selection
直接测试 credentials/transport/runtime。因此没有新增编译或运行结果，T006 仍 `PARTIAL`。
下一 Changed gate 固定为不注入 test factory 的 C++ production-wiring selector，必须通过
真实 route-backed exact grant fetch，覆盖 `Selection → GRANT_VERIFIED → assembly/terminal → cleanup`；
T007 继续锁定。详见 [B190-26](evidence/b190-26.md)。

2026-09-23 20:37 -05:00：B190-25 的 protected assembled 跨 Provider OS
`exec` gate 已通过。父进程完成一次 protected cold assembly 后释放 runtime；子进程以
不同 `providerBootId` 和新签发 grant 重建 `ProtectedRuntime`，命中同一稳定密文，走生产
`tryLoadNativeCanonicalOnnxRoleFromCache`/`openNativeAssembledEntryToFile` 解密并校验
assembled digest，plaintext lease 完成后目录被清理。修正嵌套 Boost selector 路径后，
新 selector 连续 3 次通过，普通根 `spec185-provider-assembly` 完整 21 cases 通过，
cache/exec fixture 无残留。该 gate 仍未证明 online Controller confirmation、真实网络
grant-Selection-placement rebinding、revoke、精确缺对象 fetch 或完整 T006；T007 继续锁定。
详见 [B190-25](evidence/b190-25.md)。

2026-09-23 20:22 -05:00：B190-24 的 Provider 原生 fetch/decrypt gate 已通过。
真实 User durable envelope 经生产 `SegmentFetcher`、`WireDecode`、当前 receive-key
lookup 和 `hybridAesGcmDecrypt` 后，plaintext 逐字节一致，Repo committed object
仍为 1；新 selector 连续 3 次通过，普通根 `spec189-encrypted-repo` 完整 12 cases
通过。key 是显式注入的当前 test key，因此不证明 OS Provider restart、NAC-ABE
inline unwrap 或旧 grant/KV 恢复。T006 仍 `PARTIAL`，T007 继续锁定。详见
[B190-24](evidence/b190-24.md)。

2026-09-23 20:14 -05:00：B190-23 的 C++ `fork`+`exec` Core durable serving gate
已通过。父进程完成冷发布并释放 Repo authoritative owner，子进程重新打开固定
Repo 根、重建 Face/KeyChain/ServiceUser，走 durable lookup 后通过生产 Interest
callback 读取完整 `REQUEST-LARGE` envelope；committed object 数量和 ciphertext
manifest digest 保持不变。新增 selector 连续 3 次通过，普通根
`spec189-encrypted-repo` 完整 11 cases 通过。T006 仍 `PARTIAL`：Provider
实际 decrypt/current grant-Selection-placement rebinding、revoke、精确缺对象
fetch 和真实 protected restart 未完成；T007 继续锁定。详见
[B190-23](evidence/b190-23.md)。

2026-09-23 20:09 -05:00：B190-22 已完成 B190-21 后的跨进程 protected serving
只读静态复审。确认 Repo durable metadata、Core non-secret reference 与进程内
`HybridMessageCrypto`/serving owner 的边界；现有 restart selector 仍是同一进程
close/reopen，不能替代真实 `exec` 后的 Core serving 或 Provider decrypt。下一 gate
固定为 C++ `fork`+`exec` OS-restart serving oracle；T006 仍 `PARTIAL`、T007 继续锁定。
详见 [B190-22](evidence/b190-22.md)。

2026-09-23 20:03 -05:00：B190-21 已完成 native protected assembled ciphertext reuse gate。
冷组装现在把密文持久化到由 `recipeDigest + roleAssemblySpecDigest + keyReferenceDigest`
确定的稳定目录；新授权 lookup 只返回密文 descriptor，Provider 在当前授权 runtime 下解密到
新的 request-scoped plaintext staging。C++ 回归覆盖稳定路径命中、无重复 source fetch、实际
解密、错误 key-reference/AAD 拒绝，以及独立 grant 不共享内存 runner 但复用同一合法密文；普通
根 `build/` 的 `spec185-provider-assembly` 为 110/110，assembly worker 为 6/6，相关 4 个
selector 和完整 21 cases 通过，新 selector 连续 3 次通过。既有独立-grant 回归的旧“必须二次
冷组装”断言已改为验证内存 runner 隔离与 durable ciphertext 复用。
T006 仍 `PARTIAL`：真实 OS restart/decrypt serving、在线 Controller confirmation、grant/
Selection/placement rebinding、revoke invalidation、精确 missing-object fetch、manifest
signature/tamper boundary 和真实 Qwen/MiniNDN 未观测；T007 继续锁定。详见
[B190-21](evidence/b190-21.md)。下一 gate 是 Core/Provider 跨进程 key-reference/serving
恢复与真实 placement fetch，再决定是否进入 T007。

2026-09-23 19:34 -05:00：B190-20 已完成 policy-transition protected-serving fence。新增
`ControllerVersion` service-scoped retirement、pending publication/final reference commit
fence、protected response read-side fence 和 durable V2 fail-closed/rebind；C++ oracle 已覆盖
旧 service serving retirement、其他 service 保持可用、Repo ciphertext 保留，以及 paused
publication 在版本推进后返回 `DURABLE_STALE_CONTROLLER_VERSION`。普通根 `build/` 的
`spec189-encrypted-repo` 为 120/120 compile-link，完整 10 cases 通过并连续 3 次重复通过。
T006 仍 `PARTIAL`：真实 OS restart/decrypt serving、在线 Controller confirmation、grant/
Selection/placement rebinding、Provider/assembled hit、精确 missing-object fetch 和真实
Qwen/MiniNDN 未观测；protected assembler hit 尚未具备完整跨进程资格。详见
[B190-20](evidence/b190-20.md)。下一 gate 是 B190-21 后的 Core durable
restart/authorization binding 与 Provider/assembled 跨进程审查。

2026-09-23 18:45 -05:00：B190-19 完成 T006 全局静态审查。独立复核发现不能直接把
“删除 `.dkr` reference”当作撤销修复：必须先区分首次 status restore 与真正版本推进，
为 durable publish/reference 加 service-scoped policy epoch fence，撤销当前
`m_largeDataFiles` serving owner，并为 reference I/O 建立 fail-closed typed result；否则
旧 ciphertext 仍可能继续 serving，或并发旧发布被标成新 epoch。审查覆盖
`source → prepare → Repo → ACK/Selection → placement → assembly/runner → terminal/cleanup`
链路，明确当前 `PreparedServiceRequest` 不携带 grant/Selection/placement。未改生产代码、
未启动新实验；T006 继续 `PARTIAL`，T007 继续锁定。下一 gate 固定为 B190-20
`policy-transition protected-serving fence`，详见 [B190-19](evidence/b190-19.md)。

2026-09-23 18:10 -05:00：B190-18 Core-owned durable reference recovery gate 已完成。`ServiceUser`
在 Repo durable lookup 命中前恢复并严格校验非秘密 reference；有效命中直接重注册已有
range source，不重新生成 key、加密或 `commitFile`；reference 缺失/损坏返回
`DURABLE_LOOKUP_METADATA_MISMATCH`，不把仍存在的 ciphertext 当作 protected hit。reference
仅在 Repo commit/read-back、local serving owner 和 expiry registration 全部成功后落盘。
普通根 `build/` 的 `spec189-encrypted-repo` 目标重编译链接通过，完整 8 cases 通过，
`DurablePublicationReusesAfterServiceUserRestart` 连续 3 次通过。独立只读复核确认本原子顺序，
但仍 BLOCK T006 完整闭合：真实 OS 进程重启/解密 serving、当前 grant/Selection/placement
绑定、revoke invalidation、Provider/assembled hit、缺对象精确 fetch 和 Qwen/MiniNDN 未验证。
详见 [B190-18](evidence/b190-18.md)。

2026-09-23 17:33 -05:00：B190-17 durable lookup-to-serving atomic gate 已完成。Core
在 Durable publish 前按稳定 `publicationIdentity` 查询 Repo，校验当前明文摘要/大小、当前
key epoch/reference、稳定 encrypted name、完整非秘密 metadata 和首段可读性；命中后直接注册 durable range source
并进入现有 segment serving，不生成新 key、不加密、不重复 `commitFile`。新增 Core C++ 回归
证明同一 durable identity 二次 publish 后 Repo object 与 Core publication 均保持 1 个；
owner close/reopen lookup/read 与该 serving 回归重复 3 次通过。普通根 `build/` 的
`spec190-protected-material-reuse` 7 cases、`spec189-encrypted-repo` 7 cases、
`spec188-model-preparation-publication` 5 cases 均通过。仍未证明跨进程 key/reference
recovery、新 grant/Selection rebinding、Provider/assembled hit、缺对象精确 fetch 或真实
Qwen restart；assembler protected miss 保持，T006 仍 `PARTIAL`，T007 继续锁定。详见
[B190-17](evidence/b190-17.md)。

2026-09-23 17:09 -05:00：B190-16 durable metadata atomic gate 已完成。`EncryptedLargeDataCommitOptions`
现在携带 Core-owned、非秘密的 publication identity、protection epoch、opaque key-reference
id/version、ciphertext manifest digest 和 serving locator；`RepoObjectManifest` 原样序列化并在
Repo owner close/reopen 后恢复。Durable 缺少任一字段在提交前以 `DURABLE_METADATA_INVALID`
拒绝，legacy transient overload 与 cleanup 语义保持不变。普通根 `build/` 的
`spec190-protected-material-reuse` 为 43/43 compile-link，7 个 C++ case 首轮通过并连续
3 次通过；公共头变更后的 `spec189-encrypted-repo` 为 120/120 compile-link、6 cases PASS。
API reference 增量刷新并通过 `git diff --check`。该 gate 只证明 Repo metadata persistence/read，
尚未证明 ServiceUser key-reference recovery、serving re-registration、新 grant/Selection
绑定、stable protected assembled hit 或真实 Qwen restart；protected miss 保持，T006 仍
`PARTIAL`，T007 继续锁定。详见 [B190-16](evidence/b190-16.md)。

2026-09-23 16:08 -05:00：用户明确将本 Spec190 执行中的 ASan/UBSan 构建门 deferred；已同步
`plan.md` 的 Dynamic Validation 和各任务 Exit，不再把未执行 sanitizer 结果伪称 PASS。按最新
durable evidence，T003 的 r34 真实三轮链路已补齐任务表状态为 DONE，T005 的普通根 `build/`
和 Repo lookup/repair C++ gate 已补齐状态为 DONE；下一项严格 active task 为 T006，不能跳到
T007 或真实 T011。sanitizer 若未来恢复，必须另开 scope/build/evidence gate。

2026-09-23 16:10 -05:00：T006 `ProtectedMaterialReuse` 进入 `IN_PROGRESS`。先完成
crypto-owner/key-reference/retention、encrypted Repo backing、assembler protected-miss
门和 Provider serving lease 的只读静态审查；在接口与安全失败流闭合前，不修改生产代码、不
解除 protected miss、不运行真实 Qwen。下一 gate 是 T006 五 lane static review 与 Changed gate。

2026-09-23 16:28 -05:00：T006 target interface freeze 已写入 CD-09，但仍是 target、不是
current implementation。冻结内容包括 `Transient/Durable` retention、opaque key-reference
和 ciphertext-only Repo backing、Core 重新 serving 与新 grant/Selection 绑定、显式 invalidation
及 fail-closed typed miss。下一 gate 是基于该契约的 C++ 五 lane static re-review；在 review
和 Changed gate 通过前不改 protected miss 门、不运行真实 Qwen，T007 继续锁定。

2026-09-23 16:22 -05:00：完成契约后的 scoped static re-review，确定第一个 owner-correct
atomic gate 是 `EncryptedLargeDataRangeStore` 的显式 `Transient/Durable` retention 与
`RepoEncryptedLargeDataStore::Source` 的显式 release：默认旧 API 保持 transient，durable
提交不能静默降级，Source 析构不删除 committed object，显式 release 才允许失效删除。
本 gate 不接入 protected 默认路径、不持久化 key、不解除 assembler miss；下一步只改该公共
边界、Repo adapter、C++ selector/Waf target，完成 compile-link 和 named regression 后再审查
Core key-reference owner。

2026-09-23 16:31 -05:00：B190-12 retention atomic gate 已通过：普通根 `build/` 重新配置，
新 `spec190-protected-material-reuse` 43/43 compile-link，5 个 C++ case 连续 3 次通过；
既有 `spec189-encrypted-repo` 在公共头变更后 120/120 重建，6 个回归通过。已修复一次
测试夹具 overload hiding 编译边界。T006 仍未完成：key-reference persistence/recovery、
新 grant 绑定、Provider serving/assembled protected hit 和真实重启均未验证；下一 gate 是
Core crypto-owner 的 scoped static re-review，不能跳到 T007。

2026-09-23：B190-13 完成 Core key-reference identity static re-review。首边界为 grant
`keyId` 在 `NativeGrantVerificationResult` 丢失、`ProtectedRuntime` 没有非秘密 opaque
reference、protected entry AAD/manifest 未绑定该 reference；因此继续保留 assembler protected
miss，不改稳定目录、不运行真实 Qwen。下一 gate 只实现 reference propagation 与 wrong-reference
fail-closed C++ selector，证据见 [B190-13](evidence/b190-13.md)；T006 仍 `PARTIAL`，T007
继续锁定。

2026-09-23 16:51 -05:00：B190-14 reference propagation atomic gate 已通过。普通根
`build/` 的 `spec181-protected-runtime-closure` 完成 compile/link，selector 运行 `32` 个 C++
case 无错误；已验证 grant `keyId` → runtime opaque reference → assembled AAD/manifest 的
正向链路，以及错误 reference 的 fail-closed 拒绝。ASan/UBSan 按用户决定 deferred，未与普通
`build/` 混用。稳定 protected cache 路径、ciphertext-only Repo 跨进程恢复、new-grant/Selection
重绑定和真实 Qwen 仍未验证；T006 继续 `PARTIAL`，下一 gate 仍是 durable protected lookup/
recovery，不能跳到 T007。详见 [B190-14](evidence/b190-14.md)。

2026-09-23：B190-15 完成下一边界的全局静态审查。确认 protected 目录仍使用随机 staging
目录、cache eviction 会删除其 owner、`tryLoad` 对 normal protected 仍强制 miss、Provider
cache key 仍绑定 grant 实例；同时 Core durable publication 仍使用 request/version 名，receipt
没有可恢复 key-reference/manifest，wrapped key 仍是进程态。不能直接改稳定路径或替换
`grantName|grantDigest`，否则会把无 owner recovery 的 ciphertext 当作 hit。下一 atomic gate
改为 Core/Repo durable identity metadata 与 restart serving recovery；证据见
[B190-15](evidence/b190-15.md)。T006 仍 `PARTIAL`，T007 继续锁定。

2026-09-23 16:02 -05:00：用户明确取消 ASan/UBSan 资格方向，fresh sanitizer `-j2` 从 `1/120`
推进到 `8/120` 后受控停止；可用内存约 5.4 GiB，最近 `vmstat` 没有持续 `si/so`，但没有产生
sanitizer 结果。根 `build/` 仅约 80 KiB 配置、没有可复用对象；后续普通 Waf 构建固定使用根
`build/`，不与独立 sanitizer 配置混用。T005 的现有 focused/transaction 证据保持有效，完整
sanitizer 门不再作为本轮工作目标，不因此把 T005 标记为完整验收。

2026-09-23 16:03 -05:00：普通根 `build/` 重新配置后以 `-j3` 完成 `120/120`，同一
`spec190-repo-lookup-reuse` 二进制连续 3 次通过 4/4 C++ 用例；SHA-256 为
`cc2f471eb3d8d13a99e8c7cfee56779e99bf956fe80986136da527f9e424b89f`。普通构建及 focused
runtime gate 已闭合；ASan/UBSan 按用户范围明确 deferred，不能据此把 T005 的原始 sanitizer
exit 词条计为 PASS。

2026-09-23 15:48 -05:00：T005 进入 `IN_PROGRESS`，本轮只处理尚未闭合的 ASan/UBSan
全闭包证据。已完成当前生产调用链、Repo publication lock、fixture/oracle、Waf target 与
安装边界的只读复审；上一轮 `-j2` 在约 15/120 因 12GB 主机资源边界停止，旧构建目录已不在
工作树，因此本轮登记新的 Changed gate：fresh `--with-tests --with-sanitizer=address,undefined`
配置，在同一受影响 `spec190-repo-lookup-reuse` target 上先以 `-j2` 观察真实峰值和 swap。
若 `-j2` 稳定完成，再单独评估 `-j3`；任一资源边界只记为未闭合，不改变生产结论。

2026-09-23：T005 已完成 native static/compile/runtime gate 的主要边界，但尚未闭合完整验收。
静态审查确认原路径在 `load` 和完整 source inspection 后才进入 `publish`，因此跨进程
Repo 命中仍会触发 publication 入口。已在 `RepositorySourceProvider` 增加可选
prepare-before-STORE lookup；`NativeRequestCatalog` 用完整 catalog digest 作为稳定
publication identity，`ModelPreparationCache` 命中后复用 `rollbackOwned=false` receipt，
reference-only material manifest 不提前读取全部 payload。`spec190-repo-lookup-reuse`
普通 build 成功，selector 首轮仅因 fixture root 权限未设为 owner-only 而失败，修复后通过
并重复 3 次；新增 `ConcurrentLookupAndPublishObserveOneCommitBoundary` 验证 lookup 与
publish 共用 Repo publication lock；受影响的 `spec189-prepared-request/Spec189RuntimeUsesProtectedEncryptedRepoPublication`
重新构建并通过。普通二进制 hash 将在本轮定向测试完成后更新。ASan/UBSan 全闭包以 `-j2`
编译至约 15/120 时因本机 12GB 内存持续 swap、可用内存约 243 MiB 受控停止，未计入 PASS；
随后在同一目录以 `-j1` 继续至约 19/120，无新资源边界或编译错误后因本轮工作中断而停止，
仍未计入 PASS。事务故障矩阵、真正 Runtime 第二次
prepare/新进程回归均已完成。新增的 C++ Runtime restart selector 首次在第二次
`User::prepare()` 暴露 lookup 漏恢复 material receipt 字段；重建后又暴露并修复了
`package_manifest_digest` 缺省值与 `NativeRequestCatalog` 不一致的问题。修复后该 selector
已通过，证明固定 Repo 在 Runtime close/reopen 后第二次 prepare 的 lookup=1、missIngests=0、
publicationCalls=0；这仍是同一测试进程内的 owner/runtime 重启边界，不宣称 OS 新进程资格。
随后增加了缺失 source 子对象的 root-last repair、已取消 publish 保留旧 receipt、以及 filesystem
fsync fault 下旧 receipt 保留的 C++ 反例，`spec190-repo-lookup-reuse` 4 cases 连续 3 次通过。
随后新增的 OS 进程 selector 直接 `fork()` 已确认在继承多线程 Runtime 状态后停在 futex，
父进程停在结果 pipe read；该次受控终止并登记为测试夹具边界，不计生产失败。改为
`fork()+exec()` 后，真实新进程 child 的第二次 prepare 记录 `lookups=1、missIngests=0、
publicationCalls=0`，父进程重新打开同一 root 并确认 committed source 保留；同一批次增加
recovered receipt 的 stale cleanup no-op 断言，`spec190-repo-lookup-reuse` 4 cases 连续
3 次通过。`spec189-prepared-request` 的 Runtime restart 和 protected Repo selectors 也已
重建通过；不同 initializer/layer identity 也已纳入同一 4-case selector 并连续 3 次通过。
T005 保持 `[ ]`/`PARTIAL`；sanitizer 仍未闭合，若恢复该资格测试应重新配置/构建，再按完整
验收决定是否闭合。实验启动保持 MiniNDN examples 风格的单一 `--direct-start`，
不再增加启动包装层。

2026-09-23：T004 原生固定 per-node Repo owner/restart selector 已闭合。现有
`FilesystemRepoStoreBackend` 的固定根、`BackendOwnershipLease`、原子 manifest 和
`recoverOrphans()` 经 CodeGraph/源码复审及 C++ 证明已满足 T004：3 个新 PID 复用同根，
committed graph/layer/later payload 可读且字节不变，半提交不可见，第二 owner 返回 BUSY，
损坏 sidecar 保守保留，symlink root 拒绝，逻辑配额在写入前拒绝。普通 selector 4/4、重复
3 次、既有 `spec189-repo-fd-owner` 3/3、独立 ASan selector 4/4 均通过；没有修改 Repo
生产实现，也没有增加 Python recovery/commit。T004 标记 `[x]`，现在严格解锁 T005；网络
Repo 服务和真实 Qwen 重启仍按任务交给 T011，不能提前计入本项。

2026-09-23：r34 使用修复后的 material-event oracle 和延后 CS purge 完成真实集成 PASS。
launcher 返回 `NDNSF_DI_QWEN_NATIVE_MININDN_PASS`，三轮均有 live first event/terminal/
checkpoint/success，最终 `NATIVE_CONVERSATION_TURNS_SUCCEEDED count=3`；token IDs 为
`[9,353]`、`[353,353]`、`[353,353]`，Provider 后续轮次有 KV restore，C++ oracle 为
`SPEC189_CPP_ORACLE_PASS`。`run-record.json` 为 `PASS`，最终 purge 记录 10 个 CS erase，
encrypted Repo 和进程均无残留。T003 的实现、compile-link、C++ parent/pipe 和真实两节点
三轮验收已闭合，现将 T003 标记 `[x]`；T004–T011 仍 `NOT_STARTED`，下一步严格进入 T004。

2026-09-23：r33 命令在 argparse 阶段因两个 binary hash option 拼写错误返回 `rc=2`，未
启动 MiniNDN、Provider 或 Repo，不计入 runtime-test。该命令边界已保留；下一次使用新
r34 run root 和已核对的修复 oracle hash。

2026-09-23：r32 在移除首轮 `RUNNER_READY` purge 后完成真实三轮 requester 链路：每轮 live
first event/terminal/checkpoint/success 均存在，三轮 output 均写出，两端后续轮次均有 KV
restore；第三轮不再出现 material unavailable。launcher 随后在旧 C++ Spec189 oracle 的
`MATERIAL_FETCH unknown-kind` 处退出。oracle 已扩展为兼容当前
`material-receipt/material-bundle` 和 digest-addressed verified payload，并通过 15-case
fixture；同一 r32 raw run 用修复后的 oracle 离线复核得到 `SPEC189_CPP_ORACLE_PASS`，
但 r32 尚未由 launcher 产生最终 purge/run-record。T003 仍 `PARTIAL`，下一步只做修复
oracle 后的新 run root 集成重跑。

2026-09-23：r31 已用正确 immutable source、单一 direct-start 和真实 C++ parent/pipe driver
进入 MiniNDN。启动、ACK、Selection、placement fetch、两端真实 ORT execution、第一/二轮
output/checkpoint 以及第二轮两端 `CONVERSATION_KV_RESTORED` 均已观察；第三轮在
Provider-1 首次边界 `DI_CANONICAL_material-bundle_UNAVAILABLE` 失败。原因是旧 launcher
在第一轮 `RUNNER_READY` 后就 purge LargeData CS，而 conversation 后续轮次仍依赖这些
material。已形成唯一 Changed gate：将 purge 推迟到最后一轮 Provider barrier 和 native
oracle 完成后；失败路径仍保留 run-scoped cleanup。T003 仍 `PARTIAL`，尚未重跑。

2026-09-23：r30 的真实 C++ parent/pipe live-turn 尝试在 MiniNDN 前停止，原因是命令误写
canonical source 路径（遗漏 HuggingFace `models--.../blobs/` 命名空间）。没有启动任何
Provider 或请求；`spec190-live-turns` 已完成 201/201 编译。该参数边界已保留，下一次使用
新的 run root 和正确 immutable source 路径，不覆盖 r30。T003 仍 `PARTIAL`。

2026-09-23：r29 用 `--max-new-tokens 2` 做了缓存/terminal focused probe，沿单一
direct-start 进入 MiniNDN、Provider execution 和 native cache lookup；Provider-0 首先记录
`CACHE_LOOKUP_MISS` 并进入 `COLD_ASSEMBLY_GATE`，为避免再次无意义地启动长冷组装而中断。
静态核对确认现有 assembled entry 是旧的 `float16/quantization=none` recipe，而当前候选是
`weight_only_int8`，并且 source/graph identity 不同；这是缓存严格拒绝旧模型的正确边界，不是
放宽 hash 校验的理由。r29 的精确 encrypted Repo/staging 已清理，保留 assembled cache 和 raw
evidence。详见 [B190-10](evidence/b190-10.md)。当前缓存命中、terminal/EOS、checkpoint/KV
reuse 和三轮仍未验证；T003 保持 `PARTIAL`，下一步做缓存键/当前 recipe 的只读对照，不立即重跑。

2026-09-23：r28 通过单一 direct-start 及完整 normal Repo 链路，两 Provider 分别越过旧的
512 event 边界（529/528 execution updates），且没有 `native epoch token event admission was rejected`。
但第一轮在 `requestTimeoutMs=900000` 到期后失败于 `NATIVE_STREAM_ORACLE_FAILED: terminal event timeout`；
没有 EOS、terminal、checkpoint、KV reuse 或第二/三轮。粗测第一轮约 529 updates/900 秒。
这证明 native stream budget gate 生效，但暴露出 C++/ORT token execution throughput 尚不足以完成
1025-token 预算；单纯延长 timeout 不计为修复。详见 [B190-09](evidence/b190-09.md)。
T003 仍 `PARTIAL`，下一步进行一次只读性能/超时静态审查。

2026-09-23：r27 的两次直接启动均在 MiniNDN 之前停止，已保留为
[B190-08](evidence/b190-08.md)。第一次是摘要参数缺少 `sha256:` 前缀；第二次在摘要、二进制和
build receipt 预检通过后，命中 `ENCRYPTED_REPOSITORY_PATH_MUST_BE_OUTSIDE_RUN_ROOT`。
这不是 C++ 或模型失败；下一次只把加密 Repo 放到固定 `/var/tmp` staging，证据仍放在新的
run root。r26 的 native `maxEvents` Changed gate 尚未获得新的真实运行结果，T003 继续 `PARTIAL`。

2026-09-23：r26 使用单一 `--direct-start` 进入真实 normal Repo 链路，完成 MiniNDN/NFD、
ACK、Selection、placement-bound fetch、两端 assembly/`RUNNER_READY` 和约 512 个真实
token epoch；compact evidence 日志保持约 7 MB/Provider，没有 r24 的日志膨胀。首个失败是
Provider-1 的 C++ `native epoch token event admission was rejected`，不是 launcher、资源
或 ORT。静态审查确认 `StreamRequestOptions::maxEvents` 默认 512，而请求
`maxNewTokens=1025`；token budget 没有同步 stream event budget，导致长流在默认事件上限
处失败。Requester 没有进入 terminal/EOS、checkpoint、KV reuse 或三轮验收。r26 原始证据
保留于 [b190-07](evidence/b190-07.md)。下一步只做一个原生选项投影 Changed gate：令
`maxEvents >= maxNewTokens + 1`，完成 C++ 回归、受影响安装闭合后再跑一次 normal Repo；
不增加 launcher 层。T003 仍 `PARTIAL`，T004–T011 保持 `NOT_STARTED`。

2026-09-23：fresh r18–r21 已验证 direct-start 的单一监督路径：LocalExperiment 外层 guard 未触发
资源边界，`resource-samples.jsonl` 共155条，最低 `availableBytes=3745480704`，峰值
`rootRssBytes=5133791232`、`ownedSwapBytes=166862848`，且没有残留 MiniNDN/Provider
进程；直接路径也没有生成内层 `workload/supervisor.json`。真实链路越过
`ACK_CLOSED`、`NATIVE_SELECTION_COMMITTED`，Provider-0 完成 `MODEL_MATERIALIZED`、
`WORKER_START`、`RUNNER_SPEC_READY` 并进入 ORT 执行，随后在 C++ ORT 的
`/model/Gather_5` 失败：`indices element out of data bounds, idx=1 must be within the
inclusive range [-1,0]`。Provider-1 仍在 `DEPENDENCY_FETCH`，没有 `RUNNER_READY`、terminal、
EOS 或 KV 三轮结果。r21 的临时 C++ I/O trace 确认错误发生在真实请求之前：
`RUNNER_CREATE_BEGIN` 后的 runner constructor warmup 给 Qwen causal graph 传了全零
`attention_mask`，并把动态初始 KV 维度填成 1，而非合法的单 token prefill（mask=1、
past=0）。已删除临时 trace，并将唯一 Changed gate 收敛为 C++ warmup 使用
`attention_mask=1`、`position_ids/cache_position=0` 和 epoch-0 的零长度 KV，正式请求
仍使用 authenticated lineage。受影响 native 目标重建/安装成功；
`Spec175NativeAssembly/DirectInt8CandidateAssemblyRunsOrtAndContinuation` 以当前 Qwen
INT8 source 完成 assembly、ORT warmup、prefill 和 continuation，`*** No errors detected`。
因此启动器/内存门不是首因，也不再增加 launcher 层；下一步仅用同一 immutable candidate
启动一次普通 Repo 两节点实验。T003 仍 `PARTIAL`，T004–T011 保持 `NOT_STARTED`。

本轮另将 `--direct-start` 从递归重新解析改为当前进程直接进入 MiniNDN 主路径；它只减少
一层 Python 调用，不改变授权、ACK、Selection、Repo、assembly、ORT 或 cleanup 契约。

随后用同一 immutable candidate 启动 normal Repo 时，r22 在 MiniNDN 业务启动前于
`Minindn.cleanUp()` 停止：root command-local PATH 漏掉 `/usr/local/bin`，导致找不到
`nfd-stop`。该失败没有启动 Controller/Authority/Provider/Requester，也没有形成模型或
协议证据；r22 原始目录保留。该项仅修正命令 PATH，下一次使用新的 r23 run root 和同一
candidate/Repo/configuration；T003 仍 `PARTIAL`，T004–T011 保持 `NOT_STARTED`。

r23 使用完整 PATH 进入真实 Repo/ACK/Selection，并完成 Provider-0 assembly worker 的
`RUNNER_SPEC_READY`；随后仍在 constructor warmup 的 `/model/Gather_5` 失败。静态对照
确认 direct selector 之前的显式 `inputShape.*=past:0` 掩盖了 normal manifest 缺少 shape
覆盖的问题。已将 constructor warmup 复用已有 epoch-0 动态 state shape 规则，并移除
direct selector 的显式 shape 覆盖；受影响 native build/install 与该 selector 再次通过。
下一步只用新的 run root 重跑一次同一 normal Repo；T003 仍 `PARTIAL`，T004–T011 保持
`NOT_STARTED`。

r24 使用完整 PATH 进入真实 Repo/ACK/Selection，并让两个 Provider 完成 assembly、
`RUNNER_READY` 和真实 C++ execution；但日志 observer 对每个 token 重复序列化完整
ONNX node-provider assignment JSON，两个 Provider 日志增长到约 `0.93 GB`，而根分区只剩
`2.6 GB`，为防止磁盘耗尽而停止。未观察到模型/协议错误；该轮没有 terminal/EOS/KV
验收，不能计为 PASS。已保留 raw run，下一步只做一个 compact evidence-log Changed gate：
保留内存 evidence 和有意义状态变化的完整 `OBSERVED`，将逐 epoch `UPDATE` 改为不含节点
映射的摘要，再重建 Provider、跑 C++ 定向回归和一次新的 normal Repo。详见
[b190-06](evidence/b190-06.md)。

r25 验证 compact evidence 已生效：两个 Provider 在真实执行中各约 `6.6 MB` 日志并产生
超过 200 个摘要 update；随后因 run-scoped encrypted Repo 约 `734 MB`、宿主可用空间低于
`4 GB` 而停止。检查确认 run 内 `canonical-source.onnx` 与 immutable source cache 是同一
只读 inode，并非额外物理模型副本。r24/r25 的 transient encrypted staging 已在保留日志、
配置和 raw evidence 后清理；本轮还发现 SIGTERM 未进入 launcher `finally`，已加入信号桥和
定向回归，下一步只需 fresh run 验证子进程与 staging 的失败清理。T003 仍 `PARTIAL`，
T004–T011 保持 `NOT_STARTED`。

2026-09-23：fresh r16 已越过 `ACK_CLOSED` 和 `NATIVE_SELECTION_COMMITTED`；Provider-0
完成 `MODEL_MATERIALIZED` 并启动 assembly worker，Provider-1 进入 `DEPENDENCY_FETCH`。
但 LocalExperiment 的外层 host guard 以 `RESOURCE_BOUNDARY:ownedSwap` 停止：root
resource sample 的峰值 RSS 为 `5071327232` bytes，最低 `MemAvailable=3289763840` bytes，
owned swap 峰值 `291000320` bytes，超过 `maxOwnedSwapBytes=268435456`；因此没有
`RUNNER_READY`、terminal、EOS 或三轮 KV 结果。外层 cleanup 为 `PASS`、无残留；内层双重
supervisor 被 SIGTERM 打断并留下 `KeyboardInterrupt/cleanup=UNOBSERVED`，确认启动器存在
重复监督和错误的 owned-swap 硬门。

已完成一个最小 Changed gate：LocalExperiment 作为唯一 host supervisor，向 Native
MiniNDN runner 传入 `--direct-start`，由 runner 直接启动 MiniNDN；C++ assembly worker
静态确认仍不创建 ORT session，真实 Provider 父进程在 worker 退出后创建唯一 authoritative
session。`ownedSwap` 继续记录为诊断样本，不再单独停止；`MemAvailable`、`SwapFree`、磁盘和
deadline 门保持不变。Python 定向回归 `61 passed`，`py_compile` 与 `git diff --check` 通过。
本 gate 尚未重跑真实实验；T003 仍 `PARTIAL`，T004–T011 保持 `NOT_STARTED`。下一步只用
同一 immutable candidate fresh run，确认是否越过该资源边界并进入 runner/terminal。

r17 在该 gate 后没有触发资源边界，也没有双 supervisor；它在 preparation 首先停止于
`SOURCE_IDENTITY_MISMATCH`。静态核对发现 root `/usr/local/lib/python3.8/dist-packages`
中的 ONNX adapter 仍是旧 `graph.py`，对 TensorProto scalar shape 生成了不同的 canonical
graph digest；这是安装闭包失配，不是模型或 C++ 协议失败。仅重装现有 ONNX Python adapter
（`--no-deps`）后，root 模块 hash 与仓库一致，canonical digest 恢复为
`sha256:fbcde7...`，同一 61 项 Python 回归再次通过。r17 原始目录保留；下一步仅做
fresh r18，验证已安装 adapter 与 direct-start/resource policy 一起越过 preparation。

2026-09-23：canonical mapping 修复后的 fresh r15 已越过 r14 的 `ACK_CLOSED` state-name
边界，并记录 `NATIVE_SELECTION_COMMITTED`；Controller、Authority、Provider-0/1 均 READY。
Selection 之后两个 Provider 都在解析 V3 generation contract 时停止于
`V3 Selection generation contract is incomplete`。r15 的 supervisor 在显式中断后
`cleanup=PASS`、`remainingProcesses=[]`，但 Provider 报错后外层仍因 `--run-for-ms` 保持
RUNNING，说明失败早停仍需修复。静态审查对比 source/header 与实际安装身份确认首因是
Provider 二进制构建早于当前未跟踪的 `NativeGenerationLimits.hpp` 1025 修复：Provider
仍可能按旧的 1024 上限解析合法 1025 contract；不是模型、Repo、内存或网络失败。
保留原始目录 `.codex-tmp/spec190-qwen-int8-direct-prepared/direct-int8-prep-r15/`。
下一步只重建并安装受影响 C++ production targets，运行对应 1025 selector，再 fresh
normal run；不增加 launcher 层 gate。T003 仍 `PARTIAL`，T004–T011 保持 `NOT_STARTED`。

随后完成受影响 target 的 build/install：`DI_NativeRequester`、`DI_NativeArtifactAuthority`、
`DI_NativeOnnxAssemblyWorker`、`di-native-provider` 均成功重建并安装；
`NativeGenerationLimits.hpp` 与 `/usr/local/include` hash 一致。既有
`NativeGenerationBudgetAccepts1025AndRetainsWireBounds` 通过 `1 test / 22 assertions`。
Waf 的无关 Python wrapper staged install 因缺少 `NDNSF_GLOBAL_NATIVE_DIGESTS` 被跳过，
不影响本 C++ gate。下一步是只启动一次 fresh normal r16，确认是否进入 Repo fetch/assembly；
T003 仍 `PARTIAL`，T004–T011 保持 `NOT_STARTED`。

此前 r14 已越过 r13 的 1025 token request-contract 边界，真实启动再次到达
Controller、Authority、Provider-0/1 READY，并进入 `Runtime.open→User.prepare→User.request`。
Requester 在 ACK 已关闭之后进入原生 planning，但以
`NATIVE_REQUEST_STAGE_FAILED boundary=ACK_CLOSED message=native state mapping differs from the
source boundary` 停止；这不是 ACK 传输超时，也不是内存失败。静态对照确认 Qwen INT8 source
实际 KV 名称为 `past_key_values.<layer>.key/value` 与 `present.<layer>.key/value`，而实验
生成的 catalog/options 仍使用 `past_key.<layer>` 与 `present_key.<layer>` 语义名作为 ONNX
边界名，导致 C++ `NativeCanonicalRolePreparer::bindStateContracts` 在 ACK 后拒绝 source
boundary。r14 原始目录保留于
`.codex-tmp/spec190-qwen-int8-direct-prepared/direct-int8-prep-r14/`；resource guard/cleanup
均 PASS、无残留；未运行 Selection、Repo fetch、assembly、terminal、EOS 或三轮验收。下一步
只修复 Qwen semantic→canonical state-name mapping，并补对应 Python/C++ 定向回归；静态复审、
受影响构建/安装后再 fresh normal run。T003 仍 `PARTIAL`，T004–T011 保持 `NOT_STARTED`。

本 Changed gate 的首次 Python 回归在 production/C++ 之前停止：新增 helper 漏了 `re` import，
且 idempotent canonical-name 判断只按前缀接受了不完整名称；35 个既有用例通过，新增 1 个
正例和 3 个反例未通过。该失败不改变 r14 的首个生产边界；修复后必须重新通过完整定向套件。

随后仅修正 import 与精确 regex：`py_compile PASS`，Qwen launcher 定向回归 `39 passed`；
已有 C++ `Spec182CanonicalPublisher/StateBindingConsumesActualCausalOnnxExport` 通过
`1 test / 29 assertions`；用真实 r14 stage manifest 生成的 metadata/options 已输出 canonical
Qwen source names。该 gate 的 static/compile-link/runtime-test 均通过，下一步是 fresh normal
Repo run；T003 仍 `PARTIAL`，T004–T011 保持 `NOT_STARTED`。

2026-09-23：r13 在安装 bounded-material 修复后首次越过 preparation，真实启动到达 Controller、
Authority、Provider-0/1 READY，并进入 `Runtime.open→User.prepare→User.request`；随后 request
阶段以 `generation options violate the execution contract` 停止。无 ACK、Selection、Repo
fetch、assembly、terminal 或 token 结果；resource guard/cleanup 均 PASS、无残留。静态定位发现
Python wrapper/native launcher 已接受 `1025`，但共享 C++ `NativeGenerationLimits.hpp` 仍为
`1024`，`NativeRequestEnvelope` 因此拒绝正式 1025 options。这是共享常量未同步的真实
Changed gate，不是模型或资源失败；已保留 r13 原始目录，T003 仍 `PARTIAL`。下一步只构建并
运行 `spec189-epoch-projection` 的 1025 边界 selector，再安装同一 DI library，之后 fresh
normal run；T004–T011 保持 `NOT_STARTED`。

2026-09-23：完成一次启动复杂度修整的最小 Changed gate。`LocalExperiment` 现在以 canonical
source SHA-256 作为 ONNX graph summary cache key；同一内容命中后不再重复解析 754MB 图，cache
只保存 graph/input/output 计数和状态，不保存模型字节，也不改变 source/material/binary digest。
candidate 的 `modelIdentity` 显式包含 `modelFormat`、`quantization` 和
`quantizationSubtype`，因此量化类型变化会产生新的 candidate identity；native runner 的启动前
receipt/binary digest 复核仍保留，删除的只是同一阶段内重复执行的两遍校验。静态复审确认
cache 不绕过模型身份、材料边界、grant、ACK、Selection、placement 或 cleanup。验证：Spec Kit
entrypoint sync `11/11 PASS`；两个 launcher `py_compile PASS`；`test_spec184_qwen06b_local_experiment.py`
`35 passed`；使用 r12 输入重新执行 outer `check`，`status=PASS`、source `8372` nodes，产生
新的 candidate digest `sha256:e61146...e0c364`。本单元没有启动 MiniNDN，也没有安装尚未安装的
fixed C++ library；`static=PASS`、`compile-link=NOT_APPLICABLE`、`runtime-test=PASS`、
`unobserved=fixed native install/Repo/ACK/Selection/placement/assembly/terminal/EOS/three-turn`。
T003 仍为 `PARTIAL`，T004–T011 保持 `NOT_STARTED`；下一步先安装 r12 的 C++ material 修复，
再按静态审查后的 fresh run 执行一次真实链路。

2026-09-23：r11 使用正确的 `--rounds 3 --max-new-tokens 1025 --require-multi-token` 和完整
PTY/PATH 启动，MiniNDN、Controller、Authority、两个 Provider 均 READY；requester 在
`PREPARATION_FAILED` 停止，错误为 `model descriptor contains unknown or lossy fields`，没有
ACK/Selection/Repo fetch/assembly/terminal。静态对照确认当前 `NativePlanning.cpp` 已支持
`quantization_subtype=weight_only_int8`，但安装的 `/usr/local/bin/DI_NativeRequester` 比源码旧；
这属于生产安装闭包失配，不是 Qwen/ONNX 运行结果。已用 system-first Waf 重建受影响目标
`119/119` 并安装，安装后二进制与 `build-spec189-oracle/examples/DI_NativeRequester`
hash 一致；`Spec182CanonicalPublisher/ModelIdentityBindsWeightOnlyQuantizationSubtype`
正确 selector 通过 `1/1`、`6/6 assertions`。r11 原始证据保留，下一步必须用新 run-id r12
重新 prepare/run；T003 继续 `PARTIAL`，T004–T011 保持 `NOT_STARTED`。

2026-09-23：r12 使用同一正确参数和完整 PTY/PATH 启动，所有 daemon 和两个 Provider READY，
但 requester 在 `PREPARATION_FAILED` 停止：`native canonical material payload is invalid`。
静态检查确认 Qwen INT8 有 56 个 initializer 的 `raw_data == 1 MiB`，但序列化后的
TensorProto 为 `1,048,617` 字节；生成器和验证器都只按 raw size，导致边界 initializer 未拆分
却被 payload 上限拒绝。已将两处统一改为同时检查 raw size 和 serialized TensorProto size，
并增加 C++ boundary regression。`Spec182CanonicalPublisher/InlineInitializer*` 通过 `2/2`、
`31/31 assertions`；修复库尚未安装，也未重跑 normal Repo。T003 继续 `PARTIAL`，
T004–T011 保持 `NOT_STARTED`。

2026-09-23：重新核对 normal Repo 启动参数时发现此前错误使用 `--max-new-tokens 2`；该值只适合
短 smoke，不能作为本 Spec 的验收。r9 的第一次 root launcher 还因外层 PATH 漏掉
`/usr/local/bin` 在 `nfd-stop` 前置清理处停止；补全 PATH 后，r10 到达 MiniNDN，但仍使用错误的
2-token 参数，最终 requester 失败，未形成可用的 Repo/terminal 证据。两轮均不计入验收。
源码门禁随后暴露 wrapper 原上限 `64`、native runner 原上限 `1024`，会拒绝要求的 `1025`；
已将两层上限统一为 `1025`，command regression、Python launcher syntax 共 33 项定向测试通过。
下一步生成新的 immutable r11（`--rounds 3 --max-new-tokens 1025 --require-multi-token`），
使用 PTY 与完整系统 PATH 重跑；T003 继续 `PARTIAL`，T004–T011 保持 `NOT_STARTED`。

2026-09-23：对 T003 native gate 做了补充静态边界审查。production
`NativeCanonicalRolePreparer` 会依据 semantic node mapping 生成每个 role 的 `nodeIndices`
和 layer range，assembler 也会校验 selected node cover；但当前真实 Qwen C++ selector 使用
完整 8372-node 的单 role projection，只证明了完整图的 assembler/ORT prefill/continuation，
没有证明 candidate-specific selected-layer 或两 Provider stage execution。该证据缺口已写入
`b190-03`，不得把它提前计为 T003 完成；r9 normal Repo 仍需在 root-owned context 中执行。
T003 继续 `PARTIAL`，T004–T011 保持 `NOT_STARTED`。

2026-09-23：T003 的 normal-run 编排预检发现 `LocalExperiment.py` 未把 native launcher 的
`--require-multi-token` 转发到实际命令；这会使三轮 run 可能只验证单 token，不能满足 EOS/预算
门。已做最小 wrapper 修复并通过 32 项 Python 回归、两个 launcher `py_compile`。重新生成的
`direct-int8-prep-r9` 已完成 candidate/profile/binary/model hash 固定，命令明确包含
`--rounds 3 --max-new-tokens 2 --require-multi-token`，candidate digest 仍为
`sha256:1974ce61eec1955b536062d325389cadc2bfc1009d5ddd040b0c494c40a8fb16`。prepare 不是
MiniNDN/Repo 验收；r9 仍 `NOT_EVALUATED`，T003 继续 `PARTIAL`，等待 root-owned fresh
normal Repo；T004–T011 保持 `NOT_STARTED`。

2026-09-23：修复 direct INT8 C++ selector 的测试夹具 lineage 缺口后，真实候选已通过
`NativeCanonicalOnnxAssembler`、OA02 worker、C++ ONNX Runtime CPU session/warmup、一次
prefill 和一次 continuation。受影响 `integration-tests` 重建 `128/128`，
`Spec175NativeAssembly/DirectInt8CandidateAssemblyRunsOrtAndContinuation` 在固定
`model_quantized.onnx` 上运行约 89.5 秒并通过 1/1；检查了 FP32 logits `[1,1,151936]`
以及 continuation 的 KV/present `[1,8,2,128]`。此前失败的
`stateful ONNX execution is missing authenticated generation lineage` 已证明是测试夹具未
提供 production 要求的 lineage，而不是放宽 production 校验。该项为 `ADVANCE` checkpoint，
T003 仍 `PARTIAL`：尚未完成 fresh normal Repo 的 prepare/publication/ACK/Selection/
placement fetch/terminal，且 root owner 仍是实验启动前置条件；T004–T011 保持
`NOT_STARTED`。

2026-09-23：修复现有 `RegisteredOneProviderAssemblyLoadsOrt` C++ fixture 的 V3 dataflow
绑定缺口（仅补齐 role/request/attempt/plan digest，不放宽 production validation）；受影响
`integration-tests` 完成 128/128，限定 selector 在显式
`NDNSF_SPEC182_BIN_DIR=build-spec189-oracle` 下通过 1/1。真实 assembler、worker 和 C++
ORT session/warmup 已在 tiny fixture 中执行；这不等于 direct INT8 candidate，也不覆盖
continuation、Repo、ACK/Selection 或两节点终态。该项为 `ADVANCE` checkpoint，T003 仍
`PARTIAL`，T004–T011 保持 `NOT_STARTED`。

2026-09-23：完成可复用 `ndnsf-failure-escalation` skill 的停滞门修订并同步本地
`AGENTS.md`：以 task start/last meaningful checkpoint 为计时基点，明确只有
`ADVANCE`/`ROOT_CAUSE`/`CHECKPOINT` 才算实质进展；约一小时内若仍只有重复分析、局部
helper/test 结果或同一首边界重试，必须停止生产改动/实验，完成全局静态审计、真实 Changed
gate 和 closure decision 后才能恢复。该项为流程/文档 checkpoint，不改变 T003 的产品验收
状态；skill validator 已在修订后通过，当前 T003 仍 `PARTIAL`，T004–T011 保持
`NOT_STARTED`。

2026-09-23：direct candidate fresh run `direct-int8-prep-r7` 在 launcher root preflight 停止：
`MININDN_REQUIRES_ROOT: run this script with sudo -E`。当前会话 UID 1000，`sudo -n -v`
不可用；仅创建了 run-scoped supervisor/resource-samples，未启动 MiniNDN、Provider、Repo、
requester 或 native execution。该边界是 owner 环境阻塞，不是模型/ORT/协议结果；T003 继续
`PARTIAL`，等待具备 root owner 的实验上下文后再执行一次 fresh run。

在 root owner 等待期间，两个不依赖 MiniNDN 的原生 gate 已完成受影响目标重建并通过：
`CrossProcessAdmissionTimesOutBeforeMaterialFetch` 40/40 assertions，
`MaterializedRoleReusesCertifiedGraph` 7/7 assertions。它们只证明 cold-admission lock、
materialized role graph 重用和 cleanup 边界，不能替代 direct candidate 的 ORT continuation 或
两节点验收。

2026-09-23：生产 ORT assembly selector 首次运行在 fixture worker 路径预检停止：
`DI_NativeOnnxAssemblyWorker` 实际位于 `build-spec189-oracle/`，而 selector 默认未查询该
受影响 build root；因此没有 assembler、ORT 或模型结果。已确认可用显式
`NDNSF_SPEC182_BIN_DIR=build-spec189-oracle` 修正运行环境，下一步只重跑相同 selector。
T003 仍为 `PARTIAL`，不把该路径错误计为模型失败。

随后用显式 worker 路径重跑，fixture 进入生产 projection 后在既有
`NativeExecutionPlanJson` dataflow 投影边界失败：`V3 Selection dataflow cannot be projected
for this Provider`（缺少匹配的 execution-role/request/attempt/plan digest）。该 fixture
未进入 assembler/ORT，也不是 direct INT8 candidate 结果；当时保留该首边界并转回
direct candidate launcher。随后确认该 selector 本身属于 B190-03A native gate，且生产校验
要求的四个绑定字段在 fixture 中确实缺失，因此只修复 fixture 数据流绑定；修复后的 selector
结果已在本 checkpoint 及 b190-03 evidence 记录，未改变 production validation。

2026-09-23：B190-03A 的 source identity/quantization subtype 子步骤已闭合：C++
`NativeModelDescriptor` 对 legacy `none` 保持 canonical JSON 兼容，仅将
`weight_only_int8` 纳入 model digest；catalog/role preparer 要求 recipe subtype 与模型身份
一致。受影响目标 121/121 链接成功，`ModelIdentityBindsWeightOnlyQuantizationSubtype`
通过 6/6 assertions；inline/external bounded-chunk selectors 分别通过 25/25、26/26，
launcher `py_compile` 通过。剩余 B190-03A 仅为真实 native assembler/ORT runner 的一次 token
与 continuation gate，以及随后 fresh normal Repo；T003 仍为 `PARTIAL`，T004–T011 保持
`NOT_STARTED`。详见
[`b190-03.md`](evidence/b190-03.md#b190-03a-identity-and-bounded-material-regression-20260923)。

2026-09-23：B190-03A identity gate 首次受影响构建在新增 C++ 负例测试编译时停止：
`di-native-canonical-publisher.t.cpp` 使用了该文件不存在的 `digest()` helper；生产修改过的
`NativePlanning.cpp`、`NativeRequestCatalog.cpp` 与 `NativeCanonicalRolePreparer.cpp` 均已完成
编译，尚未链接或运行。已定位为测试局部 helper 名称错误，下一步只修正为现有
`nativePlanningDigest()` 后重跑同一目标。详见
[`b190-03.md`](evidence/b190-03.md#b190-03a-identity-compile-boundary-20260923)。

2026-09-23：B190-03A 的 inline material bounded-chunk 修复已完成受影响 C++ 目标构建，
`spec189-canonical-publisher` 链接成功；新增 `InlineInitializerUsesBoundedChunksAndReassemblesAfterSelection`
通过 25/25 assertions，既有 `ExternalInitializerUsesBoundedChunksAndReassemblesAfterSelection`
通过 26/26 assertions。两项均为原生 C++ publisher/selection/reassembly 回归，不代表
direct INT8 已通过 Repo、ACK/Selection、Provider assembly 或 terminal。当前可继续执行
B190-03A 的 source identity/quantization descriptor 与 native candidate gate；T003 仍为
`PARTIAL`，T004–T011 保持 `NOT_STARTED`。完整命令和边界见
[`b190-03.md`](evidence/b190-03.md#b190-03a-inline-material-bounded-chunk-regression-20260923)。

2026-09-23：B190-03A inline material 修复的首次受影响目标构建在 compile 阶段停止：
`deriveNativeCanonicalMaterialManifest` 仍将解析出的 `ModelProto` 声明为 `const`，而新的
bounded inline chunk 路径需要将 `raw_data` move 到 shared backing；因此编译器报告对 const
对象调用 `mutable_graph()`。未进入链接、定向 C++ 测试或运行；该边界是实现编译错误，不是
模型/ORT/协议结果。下一步仅修正该声明后重新执行同一受影响构建。详见
[`b190-03.md`](evidence/b190-03.md#b190-03a-inline-material-compile-boundary-2026-09-23)。

2026-09-23：完成一次全链路静态兼容性审计。保留本地 Qwen INT8 转换尝试；转换在
`onnxruntime.quantization.quantize_dynamic()` 的 per-channel MatMul 权重量化阶段运行
29:09 后按策略中断（退出码 `130`），不形成可验收候选。随后固定缓存中的
`liodon-ai/Qwen3-0.6B-ONNX/model_quantized.onnx` 已完整下载并通过只读 ONNX/ORT CPU
one-token smoke：文件 `753999618` bytes、197 `MatMulInteger`、输入控制张量 `INT64`，
KV/present/logits `FLOAT`。它仍未通过当前 C++ publication/Repo/assembly 链路：新的
normal run r6 在 Requester preparation 首边界失败于
`DI_NATIVE_PUBLICATION_MATERIAL_PAYLOAD_TOO_LARGE`，因为 inline initializer 被当作单个
超过 1 MiB 的 payload。这个失败不是 IO/KV mismatch，也不是 T004；T003 继续 `PARTIAL`，
T004–T011 继续 `NOT_STARTED`。B190-03A 的完整修复顺序已写入 [plan](plan.md#t003-model-source-compatibility-recovery-gate)，
证据见 [`b190-03.md`](evidence/b190-03.md#direct-int8-candidate-downloaded-and-r6-preparation-boundary-2026-09-23)。

2026-09-22 23:34 -05:00：对“全链路改为 INT8”做了只读可行性检查，未保留未验证的
backend/profile 改动。ORT CPU 确实支持量化算子，但当前 Qwen ONNX 的控制输入为
`INT64`、KV/hidden/output 为 `FLOAT16`，而 NDNSF C++ `TensorElementType` 只有
`Float32/Float16/Int64/Bool/UInt8`，没有 signed `Int8`；ORT runner 的类型映射也没有
`INT8`。因此仅改 backend、`precision` 或 `quantization` 会制造不真实的 contract，不能
作为 T003 Changed gate。一次直接对 FP16 Qwen 图做 dynamic INT8 的试验还在
`DynamicQuantizeLinear` 的 FP16 activation 类型边界失败；随后候选生成在大图校验阶段被
人工中断，未产生可验收候选。已撤回本轮新增代码，T003 继续 `PARTIAL`，T004–T011
继续 `NOT_STARTED`。若继续“全 INT8”，前置任务必须是新的量化导出/校准模型与 signed
INT8 tensor bundle/runner contract，并先完成 C++ focused regression；不能继续修改 launcher
字符串或启动两节点实验。

2026-09-22 22:03 -05:00：继续 T003 的恢复诊断时发现新的主机级前置阻塞：`/swapfile-codex`
总量约 6 GiB，当前几乎全部使用，`SwapFree` 约 548 KiB；主要 swap 使用者是 VS Code
renderer/工具链和桌面进程，不是仍在运行的 MiniNDN/Provider。该状态会使新的真实实验在
代码行为尚未稳定前触发全局 `RESOURCE_BOUNDARY:SwapFree`，不能通过提高门限或重复 run
解决。已完成只读确认，未执行 `swapoff`、终止桌面进程或新的实验；T003 保持 `PARTIAL`，
T004–T011 继续 `NOT_STARTED`。下一步需要先恢复可控的主机 swap 状态，再按新的 Changed gate
冻结复审、重建和重跑 T003。

2026-09-23：补上 LocalExperiment 的持久缓存目录接线：`prepare` 现在接受
`--cache-dir`（兼容别名 `--artifact-cache-root`），默认使用系统稳定根，并将解析后的目录
写入 launch record、显式传给 native runner。目录若位于本次 run 的 workload/evidence 根内，或
已存在但不是目录，会在启动前拒绝；不会由 cleanup 删除。native 现有的 source Repo 内容寻址
identity 和 Provider recipe-addressed assembled cache 保持不变：只有完整 hash/identity 命中才复用，
当前授权、ACK、Selection、placement 和 protected Repo 边界不被绕过。32 项 Python 定向测试及
三个脚本 `py_compile` 通过；尚未做跨 run 的真实两节点 cache-hit 验收，因此不改变 T003 的
`PARTIAL`，T004 仍为 `NOT_STARTED`。证据见 [`b190-05.md`](evidence/b190-05.md)。

2026-09-23：完成 CPU-safe `SmolLM2-135M` all-FP32 candidate的最小原生兼容修复与一次
新的 normal Repo run。静态审查确认 candidate 的 stateful ONNX manifest 使用
`llama-causal-position-v1`，而 `CausalPositionInputContractV1` 原先只接受
`qwen-causal-position-v1`；现已在 `OnnxRuntimeModelRunner.cpp` 允许这两个已认证策略，仍
保留 attention/position 输入名和图签名校验，并在 `di-onnxruntime-gpu-evidence.t.cpp`
补充 llama materialization 回归。受影响生产目标 `ndnsf-distributed-inference`、
`di-native-provider`、`DI_NativeRequester` 和 `DI_NativeOnnxAssemblyWorker` 使用系统编译器
`-j4` 构建成功并安装到 `/usr/local`，构建/安装 hash 一致；独立 C++ focused binary 的
`CausalPositionInputsComeOnlyFromAuthenticatedLineage` 为 `1 test / 8 assertions PASS`。
全量 `unit-tests` 仍在既有并行脏改动的 `di-native-planning.t.cpp` 两处
`NativeArtifactBinding` aggregate assignment 编译错误处停止，不能计为全量回归通过。

新的 `f32-smollm135m-r2` 已完成 check/prepare 并进入真实
`prepare → Repo → ACK → Selection → placement-bound fetch → Provider assembly`；Provider-0
已到达 `MODEL_MATERIALIZED → WORKER_DONE → CACHE_FINALIZATION_DONE → RUNNER_READY`，说明
causal-policy 首边界已越过。随后 host guard 以 `RESOURCE_BOUNDARY:SwapFree` 停止，
`minSwapFree=536846336 < 536870912`；但 `minAvailable=4791259136`、
`maxOwnedSwap=0`、进程 RSS 峰值 `2794491904`、cleanup `PASS` 且无残留。该结果是全局
swap-free 门触发，不是 owned-swap/OOM 证据；Provider-1 当时仍在 dependency fetch，未形成
terminal，不能宣称 Spec189/190 或 Qwen3-0.6B 验收通过。完整边界、原始日志和资源样本见
[`b190-04.md`](evidence/b190-04.md)及
[`f32-smollm135m-r2`](../../.codex-tmp/spec190-smollm135m-20260923-runs/f32-smollm135m-r2/)。
T003 仍为 `PARTIAL`，T004 仍为 `NOT_STARTED`；下一步先处理/复核全局 swap-free 资源条件
和 Provider-1 dependency-fetch 首边界，不提高资源阈值、不绕过 protected cache、不启动 T004。

2026-09-23：针对 CPU EP 的 dtype 约束，选择较小的 `HuggingFaceTB/SmolLM2-135M`
作为 CPU-safe candidate，使用现有 `llama` profile，并显式生成 `float32` ONNX；这不是把
Qwen3-0.6B 的 FP16 initializer 原地转换后继续复用，而是新的 model/graph/initializer
identity。固定候选目录为
`.codex-tmp/spec190-smollm135m-20260923-retry2/`，policy 生成成功，manifest digest 为
`1b4b955d9dfaf28dd270eaa08baaa5d8e6afade2ddbdbb76f94a43dbca4eaecc`。候选为 2 stages、30
layers、EOS `[0]`；两个 stage 分别为 `326168031` 和 `326171569` bytes，initializer
dtype code 全部为 ONNX `FLOAT`，CPU EP session 均可加载，未出现
`InsertedPrecisionFreeCast`，输入/KV/output/logits 均为 `tensor(float)` 契约。该候选只
证明了较小 FP32 ONNX 能避开当前 Qwen 的 FP16→FP32 runtime cast；它还没有经过完整
`prepare → Repo → ACK → Selection → 两 Provider assembly/execute → terminal`，不改变
Qwen3-0.6B 主目标，T003 仍为 `PARTIAL`，T004 仍为 `NOT_STARTED`。

独立脚本维护（2026-09-22）：SIF 模板改为消费根 Waf 的 DESTDIR 安装输出，
第二轮移除 base 复制/强制 APP 导出，测试附件改由 Waf opt-in 安装；
离线脚本检查 61 passed / 1 skipped / 5 deselected，未运行 Waf、编译或 SIF 构建。
见 [SIF install review](../../Experiments/TigerCluster/docs/waf-install-static-review-20260922.md)。
不改变本 Spec 任何模型实验任务或资格状态；原生安装、ABI 与容器运行仍待验证。

2026-09-23：Changed gate 后的 normal Repo `run-67` 已完成 launcher、MiniNDN/NFD、ACK/
Selection、placement-bound fetch，以及两个 Provider 的 `WORKER_DONE →
CACHE_FINALIZATION_DONE → RUNNER_READY`；新 gate 下第一个 Provider 冷组装没有复现旧的
3 GiB 级 assembly-worker 峰值，峰值 RSS 从 run-58 的 `7395811328` 降至
`6884945920`。但两个 request-scoped runner/后续执行准备重叠时仍触发既有资源门：
`availableBytes=1327259648 < 1610612736`、`ownedSwapBytes=326225920 > 268435456`，
cleanup `PASS` 且无残留。没有 terminal、三轮 requester 结果、C++ oracle 或资格 PASS，
因此 T003 仍为 `PARTIAL`；该 gate 证明减少了 cold-assembly 工作集，但没有解决两个
request-scoped ORT runner 与第二阶段执行准备的总内存/换页压力。静态核查确认本路径的
runner 不是 `m_runners` idle cache，而是由 worker execution/publish 生命周期暂时保持的
两个 ORT session。run-67 原始证据和阶段边界见 [`b190-03.md`](evidence/b190-03.md) 本轮
小节；run-59–66 的 launcher 参数边界也已保留，未覆盖历史证据。下一步应先静态审查两
runner 同时驻留后的 ORT/initializer/KV 保留关系，再决定一个单变量 Changed gate，不提高
资源阈值、不绕过 protected cache，也不启动 T004。

2026-09-23：对 run-67 的 native ORT profile 做了只读细化。Provider-1 的资源边界 RSS 为
`3286151168` bytes，profile 在 warmup 和第一次真实 `model_run` 中各出现一次大型
`InsertedPrecisionFreeCast_onnx::MatMul_9461_kernel_time`，输入为
`float16[1024,151936]`，输出 `622329856` bytes、参数 `311164928` bytes；Provider-0
对应最大 cast 输出为 `12582912` bytes。该证据把下一步 Changed gate 收敛到 CPU EP/ONNX
dtype contract，而不是 `m_runners` cache 或删除 warmup；allocator ownership 仍未观测，
因此不计为精确 RSS 根因闭合，T003 仍为 `PARTIAL`，T004 仍为 `NOT_STARTED`。
当前 recipe 还强制绑定原始 `graphDigest`/`canonicalInitializerDigest`；任何把 Qwen
initializer 转为 CPU-specific `float32` 的方案都必须先定义 signed derived-artifact identity
和缓存键，不能作为未声明的本地修补。

2026-09-23：完成 CPU FP16 kernel 可用性诊断。宿主 AMD Ryzen 7 3700X 仅暴露 `F16C`
转换指令，没有 `AVX512_FP16` 算术指令；当前 `/opt/onnxruntime` 为 `1.26.0`。独立
FP16 `MatMul` 最小模型在 CPU EP 上可执行，但 ORT 优化图明确生成
`InsertedPrecisionFreeCast_W/X/Y`；与 run-67 profile 的 `lm_head` cast 相互印证。
因此当前环境没有可直接切换的 FP16 `MatMul` kernel，不能用 session 选项强行删除
`float16 → float32`；现有 C++ focused selector 仍为 `1/1`、`1/1`、`15/15`，没有
生产代码改动。T003 仍为 `PARTIAL`，T004 仍为 `NOT_STARTED`；后续若要消除此转换，
只能引入支持该算子的 CPU/硬件后端，或定义 signed derived CPU artifact，不得修改原始
initializer 后继续复用当前 digest。

2026-09-23：prepacking Changed gate 的首次 normal Repo 启动记录为 `run-68`，在
MiniNDN/NFD、Repo、ACK 或 Provider 启动前被 launcher preflight 拒绝：所复用的 immutable
stage manifest 未带显式 `modelFamily`，当前脚本要求通过 `--model-family qwen` 补齐。
该边界不是模型、ORT 或资源结果，原始 run-68 目录保留；下一次使用新 `run-69` 重试。
T003 仍为 `PARTIAL`，T004 仍为 `NOT_STARTED`。

补充：run-69 使用 `--model-family qwen` 仍在同一行 `load_stage_manifest()` 边界失败，
证明该参数只影响后续 planner，不能修复 manifest schema。已找到带显式 `modelFamily=qwen`
且与原 stage/canonical/node-mapping digest 一致的 immutable manifest；下一次使用新
`run-70`，不覆盖 run-68/run-69。

2026-09-23：prepacking 诊断实验已完成 `226/226` compile-link、三个 C++ selector
（`1/1`、`1/1`、`15/15`）并安装 native targets。run-70 使用正确 manifest 进入真实链路，
两个 Provider 均到达 `WORKER_DONE → CACHE_FINALIZATION_DONE → RUNNER_READY`；随后 host
guard 以 `RESOURCE_BOUNDARY:SwapFree` 停止，cleanup `PASS`、无残留。聚合证据为
`maxRss=6806839296`、`minAvailable=2661167104`、`maxOwnedSwap=75616256`、
`minSwapFree=533250048 < 536870912`。更重要的是，打开 prepacking 只保留/复用 ORT 的
FP32 预打包表示，并没有消除 `float16 → float32` 转换，不符合本 Spec 的约束；因此该
Changed gate 已拒绝并回滚到 `session.disable_prepacking=1`，不能据此宣称 RSS 改善或验收
通过。T003 仍为 `PARTIAL`，T004 仍为 `NOT_STARTED`。

补充静态审查：CPU Qwen 路径的 constructor warmup 不会把 KV/state 留在
`deviceStateBySession`；该 map 只在 `statefulIo && selectedProvider == cuda` 时写入，CPU
路径的 warmup 输出在构造函数中立即销毁。剩余常驻内存主要属于 `Ort::Session`、CPU arena、
prepacked/converted weights 与 runner 生命周期，不能通过调用 `releaseSessionState` 释放。
因此不新增 warmup 清理伪修复；prepacking 也不再作为候选 gate。若要真正消除 FP32
转换，必须更换具有实际 FP16 `MatMul` 的后端/硬件或另行定义 signed derived artifact。

2026-09-23：完成 T003 内一个新的 cold-assembly Changed gate：
`materializeNativeCanonicalModel()` 返回后立即释放 `materialPayloads` 和
`materialManifest`，再进行 role identity、staging 与 worker preparation；这消除了
materialized role 在模型字节、选中 layer payload/index 与后续 assembly 状态之间的无必要重叠。
该修复未改变 ONNX、initializer、KV 或 output contract，也未改变 resident runner 的生命周期。
使用系统编译器重新构建 `226/226`；`spec190-materialized-role`、
`spec190-cold-assembly-gate` 和 `spec190-readiness-progress` 分别通过 `1/1`、`1/1`、
`15/15`，materialized-role selector 连续三次通过。build/install 产物 hash 与安装路径一致，
worker `ldd` 无缺失依赖；editable `py_repoclient` 子步骤仍因缺少
`NDNSF_GLOBAL_NATIVE_DIGESTS` 被安装门跳过。尚未运行新的 normal Repo，因此没有宣称 RSS
或 T003 验收闭合；当前仍为 `PARTIAL`。持久证据见
[`b190-03.md`](evidence/b190-03.md) 的本轮 Changed gate 小节。

2026-09-22：完成 DI 十页生命周期讲稿文档单元（`NO_DESIGN_CHANGE`）。
`docs/NDNSFDI/slides/main.pdf` 解释 prepare/Repo publication、ACK 后 placement、
Selection 后获取/组装、prefill/decode 与不同缓存；明确 r260 仅为三轮短输出的
cache-compatible PASS，跨 request resident session 仍属 T007，未重跑模型实验。
构建/逐页渲染审查及源码/原始 oracle 依据见
[`review-20260922.md`](../../docs/NDNSFDI/slides/review-20260922.md)。
本项不改变 T003 `PARTIAL` 或 T004–T011 的验收状态。
文档 checkpoint commit 被现有全索引 development-assistant 引用 hook 阻止；未绕过，
未 push。slides 产物保留，tasks.md 的其他并行修改未整体暂存。

2026-09-22：修复 Spec190 的 protected reuse 顺序矛盾。原 `contracts/design.md`、
`contracts/material-reuse.md` 和 `traceability.md` 把 T006 的安全设计门或当前任务映射误指向
T009/旧任务编号，导致严格串行下 T006 无法闭合、T007 无法合法开始。现已统一为
`T006 ProtectedMaterialReuse → T007 ResidentSession`：durable key-reference、serving recovery、
new-grant binding 和 retention policy 由 T006 先冻结；T006 未完成前继续保留 normal protected
Repo 的 cache miss 门；T009 仅负责 FINALIZE/drain。该轮为文档静态修复，无源码/配置/实验变化，
T003 保持 `PARTIAL`，T004–T011 保持 `NOT_STARTED`。审查记录见
[`audit.md`](audit.md) 的 Protected Reuse Ordering Repair 小节。

2026-09-22：完成 T003 cache/lease/resident-runner 静态复审，未新增 Changed gate。复审确认
`ProviderArtifactKey` 不包含 request/attempt 等瞬态身份，但包含 source/root/graph/initializer、role、
`offerDigest`、recipe/backend/device/precision/layout、protection epoch 和 grant/provider identity；
因此不能把不同 Selection 的 `CACHE_LOOKUP_MISS` 归因于 request-id 污染。更关键的是
`tryLoadNativeCanonicalOnnxRoleFromCache()` 在 normal protected Repo 且未设置
`cacheCompatibilitySourceDir` 时按安全契约直接返回 miss；这属于当前 protected material reuse
边界，不能在 T003 里移除或提前改成跨 grant plaintext 命中。

同一复审确认 cache entry 发布前会清空 runner template 的 `path` 和 `lifetime`，只保留 metadata/
content-addressed ciphertext descriptor；`ProviderArtifactLease` 通过 move-only RAII release pin，
Provider 仅把 lease 绑定到实际 runner 生命周期。assembly worker 返回后，父进程 scrub canonical
source/material，protected plaintext 使用有界 guard 和 staging lease；没有发现 idle cache 保留第二份
模型字节或失败路径遗漏 cleanup 的静态证据。随后按 raw phase 重新对齐 run-58：Provider-1
只到 `WORKER_DONE → CACHE_FINALIZATION_BEGIN`，未到 `RUNNER_CREATE_BEGIN/RUNNER_READY`；因此
“两个 resident ORT session/runner”不是已证实结论。当前最小可证边界是 Provider-0 的 resident
runner 与 Provider-1 protected cache finalization 的模型级临时缓冲可能重叠；需先静态检查
seal/open/write 的缓冲所有权，再选择一个最小 Changed gate。
证据见 [b190-03](evidence/b190-03.md) 的 cache/lease review 小节及源码
[`Provider.cpp`](../../NDNSF-DistributedInference/cpp/ndnsf-di/Provider.cpp)、
[`ProviderArtifactCache.cpp`](../../NDNSF-DistributedInference/cpp/ndnsf-di/ProviderArtifactCache.cpp)、
[`NativeCanonicalOnnxAssembler.cpp`](../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.cpp)。
T003 保持 `PARTIAL`，T004–T011 继续 `NOT_STARTED`；下一步必须先明确 T006/T007 的受控生命周期/
protected material reuse 设计，再决定是否编码，禁止重复 normal Repo 盲试。

2026-09-22：对 run-58 的 437 个资源样本做了内存结构分解。失败前未知进程组
`2900289` 从约 1.57 GiB 突增到约 2.97 GiB；Provider-0 已确认进程组 `2900263` 约
1.84 GiB。sampler 没有 native counters，不能把 RSS 拆成 ORT/heap/mmap；结合源码中
`sealNativeAssembledEntry()` 的 `modelBytes → cipher → wire` 和
`openNativeAssembledEntry()` 的 `wire → cipher → plaintext` 全量 vector 链，当前最小可证
假设是 protected finalization 的模型级临时缓冲重叠，而不是已证实的两个 resident ORT
session。该诊断未改源码、未重跑；`UNOBSERVED` 边界与原始数据见 [b190-03](evidence/b190-03.md)
的 run-58 memory decomposition，T003 仍 `PARTIAL`，T004–T011 仍 `NOT_STARTED`。

2026-09-22 18:15 -0500：完成 T003 内唯一 ORT arena Changed gate 的 compile-link、focused C++
回归和一次 normal Repo。静态审查确认两个 Provider 的 resident runner/session 在同一次多
token request 中必须保持存活，不能以 move 或提前释放破坏 KV/runner 语义；本轮仅试验
`DisableCpuMemArena()`。run-55 通过 launcher、MiniNDN/NFD、ACK/Selection、placement-bound
fetch，两个 Provider 均到达 `RUNNER_READY`，随后以 `RESOURCE_BOUNDARY:MemAvailable` 停止，
峰值 RSS `8372178944`、owned swap `422146048`，较 run-54 更差；cleanup `PASS`、无残留。
该变量已撤回，T003 仍为 `PARTIAL`，T004–T011 继续 `NOT_STARTED`。native install 的
shared-library 安装和 hash/ldd 校验完成，但 editable `py_repoclient` 子步骤因缺少
`NDNSF_GLOBAL_NATIVE_DIGESTS` 未完成；不计为完整 install PASS。持久证据见
[b190-03](evidence/b190-03.md) 的 run-55 小节、[`run-55`](../../.codex-tmp/spec190-t003-file-backed-20260922/run-55/)
和 `docs/failure-log.md`。下一步仍只能在 T003 内基于新的静态证据选择一个最小变量，不能提高资源阈值或启动 T004。

run-57 使用正确的 Qwen v2 stage manifest 和 `DisableMemPattern()` 完成一次 normal Repo；两个
Provider 均到达 `RUNNER_READY`，但首个 requester terminal 前仍以
`RESOURCE_BOUNDARY:MemAvailable` 停止（`availableBytes=1429606400`、`rssBytes=8234680320`、
`ownedSwapBytes=183209984`，cleanup `PASS`、无残留）。该 gate 减少了部分 cold assembly
压力但没有闭合 resident runner 工作集边界，T003 继续 `PARTIAL`，T004–T011 继续
`NOT_STARTED`；run-57 raw evidence 已写入 [b190-03](evidence/b190-03.md)。若继续，只能先
静态复审下一个单变量，不能提高资源门或启动 T004。

run-58 的最后一个单变量将 resident runner 的 graph optimization 设为 `ORT_DISABLE_ALL`，
focused C++ 回归通过；normal Repo 中 Provider0 到 `RUNNER_READY`、Provider1 到
`WORKER_DONE`，之后仍因 `RESOURCE_BOUNDARY:MemAvailable` 停止（`availableBytes=1547898880`、
`rssBytes=7395811328`、`ownedSwapBytes=14311424`，cleanup `PASS`、无残留）。该配置比
run-57 进一步降低峰值但没有形成 terminal，因此 T003 仍 `PARTIAL`，T004–T011 仍
`NOT_STARTED`。当前证据表明剩余边界是两个 Provider session 的驻留工作集，需要下一次
明确的生命周期/缓存架构决策后再动代码；不再用重复 normal Repo 盲试。

补充：run-56 在 normal Repo 前因误用 service manifest 作为 `--stage-manifest`，于
`load_stage_manifest()` 以 `stage manifest requires an explicit modelFamily` 停止；raw run
已保留，未进入 MiniNDN/Repo/Provider。该失败边界不改变当前 C++ gate，下一次仅改用已核对
的 Qwen v2 stage manifest，T003 仍为 `PARTIAL`，T004–T011 仍为 `NOT_STARTED`。

2026-09-22 15:08 -0500：按生产调用链修正未启动任务的严格顺序；保留 T001–T003 的历史
ID和当前 T003 `PARTIAL` 状态，只将后续职责重排为 `T004 RepoRestart → T005
RepoLookupReuse → T006 ProtectedMaterialReuse → T007 ResidentSession → T008
StageTransferBudget → T009 TerminalDrain → T010 Convergence → T011 MatchedExperiment`。
这次是 Spec/plan/tasks 的文档变更，不把文档审查、focused C++ PASS 或 run-38 cleanup PASS
升级为产品完成。当前仍只有 T003 可继续，T004–T011 均保持 `NOT_STARTED`。

run-35/36/37 均在 MiniNDN 前置阶段 fail-closed，分别为
`MODEL_STAGE_MANIFEST_DIGEST_MISMATCH`、`REQUESTER_BINARY_DIGEST_MISMATCH` 和
`ENCRYPTED_REPOSITORY_PATH_MUST_BE_OUTSIDE_RUN_ROOT`；run-38 进入真实 normal Repo 链路后，
Provider-0 完成 worker/cache finalization/`RUNNER_READY`，Provider-1 在
`MODEL_MATERIALIZED`/`WORKER_START` 后由 `RESOURCE_BOUNDARY:MemAvailable` 停止。
这些边界已写入 [b190-03](evidence/b190-03.md) 和 `docs/failure-log.md`，不改变任务完成状态。

run-50 在 MiniNDN 启动前因 stage manifest 摘要遗漏 `sha256:` 前缀命中
`MODEL_STAGE_MANIFEST_DIGEST_MISMATCH`；run-51 在同一阶段因手工拓扑摘要错误命中
`TOPOLOGY_DIGEST_MISMATCH`；run-52 因 root-owned canonical evidence 对普通 shell 不可读而
命中 `MODEL_CANONICAL_DIGEST_MISMATCH`；run-53 进入 MiniNDN 初始化后因 multiline sudo
shell 的 `SUDO_COMMAND` 环境值触发 NFD 启动解析异常。四个 raw run 均已保留，不计入产品
运行结果。run-54 已通过启动并进入真实 two-provider 链路，但在两个 runner 均 ready 后以
`RESOURCE_BOUNDARY:MemAvailable` 停止，仍未形成 terminal/三轮终态。T003 仍为 `PARTIAL`，
T004–T011 继续保持 `NOT_STARTED`。

本轮文档静态复审：`audit_speckit_structure.py --strict` 为 `PASS`（11 tasks、17 FR、9 SC、
5 user stories、2 tasks complete），`verify-spec-kit-sync.py --require-entrypoints` 为
`PASS: 11/11`，`git diff --check` 通过。审查覆盖 production call-chain mapping、task
dependency/order、C++ selector/evidence owner、build/source closure 和 migration/failure
boundary；`static=PASS`，`compile-link=UNCHANGED`，`runtime-test=UNCHANGED`，
`unobserved=run-38 normal Repo qualification and protected durable-cache hit`。Closure decision
为 `OPEN_FOR_NEXT_BATCH`：只有当前 T003 完整退出后才进入 T004。

2026-09-22 run-43：完成本轮唯一 `worker-file-backed-response` Changed gate 的 CodeGraph/只读
静态复审、受影响目标 `133/133` compile-link、安装和定向 C++ 回归。新增的 worker status-3
digest-only response 让父进程在真实 Provider 路径中避免通过 stdout 返回 752 MB assembled
model；run-43 第一轮完成两 Provider `RUNNER_READY → EXECUTION_COMPLETED`、Provider-1
`TERMINAL`，Requester 报告 `NATIVE_REQUEST_SUCCEEDED`。第二轮仍重新 material fetch/assembly，
未命中跨 request persistent assembled runner；Provider-1 在 `MODEL_MATERIALIZED → WORKER_START`
期间触发 `RESOURCE_BOUNDARY:ownedSwap`（峰值 `379510784 > 268435456`），supervisor
`cleanup=PASS`、无残留进程。raw evidence 已保留在 `run-43`；三轮 terminal、C++ oracle、
negative-parent 和完整 qualification 仍 `UNOBSERVED`。这是 T003 内 gate 的 focused/static
`PASS`、normal Repo `UNQUALIFIED`，不解锁 T004。

2026-09-22 15:46 -0500：完成 run-43 第二轮 cache/runner 静态诊断。确认
`ProviderArtifactCache` 的同 key 进程内命中实现正常；两轮 `selectionDigest` 不同，且 normal
protected Repo 在未设置 `cacheCompatibilitySourceDir` 时按安全策略显式跳过 assembled-cache，
当前日志未输出 canonical cache key，不能把两轮宣称为同一 key；因此本次
`CACHE_LOOKUP_MISS` 不能诊断为 cache 实现退化，也不能证明已有 runner 可跨 selection 复用。
`spec185-provider-assembly` 重新编译
`110/110`；补 worker bin-dir 后 `ProviderArtifactCachePinsEvictsAndSeparatesIdentity`、
`ProtectedArtifactCacheColdHitBindsGrantAndRetainsCiphertext`、
`ProductionAssemblerCacheColdHitUsesExactArtifact` 均 `1/1 PASS`。一次未设置 bin-dir 的整套
selector 失败于测试环境找不到 worker binary，未计功能失败；已保留该边界并修正后重跑。
当前仍是第二轮 Provider-1 冷物化/结构组装与 resident runner 重叠触发
`ownedSwap=379510784`，没有新的 T003 代码改变；T003 `PARTIAL`，T004–T011 保持
`NOT_STARTED`。持久证据见 [b190-03](evidence/b190-03.md) 和
[`docs/failure-log.md`](../../docs/failure-log.md)。

2026-09-22：在 `Experimental` 分支完成一次新的 T003-only static Changed gate。静态审查
确认 file-backed materialized role 的 staged `model.onnx` 已由父进程以确定性 wire 写入并带有
已校验 digest；assembly worker 的 S1–S6 结构/边界/recipe 校验不需要再分配第二份完整序列化
buffer。因此仅在 `modelFile != nullptr` 的 worker 路径复用该文件 digest，并继续由 parent 在
child reap 后有界读取；inline/parity 路径仍执行原有 `deterministicWire`。这不改变 ORT
authoritative session、Repo、ACK/Selection、输入/KV/输出或 cleanup owner。
该 gate 通过 CodeGraph/只读静态审查、DI/worker compile-link、integration target `128/128`
和定向 C++ 回归：file-backed/materialized/cold/readiness 分别 `1/1`、`1/1`、`1/1`、`15/15`，
带正确 worker 环境的 Spec185 cache selector `11/11`。完整 `unit-tests` 仍在既有
`di-native-planning.t.cpp` 的 `NativeArtifactBinding` aggregate-assignment 编译边界停止，
不计为本 gate 失败。Waf 普通 install 触及 `/usr/local` 权限边界后，已用匹配 build 输出完成
显式安装并核对 shared library/worker SHA256 与 `ldd` closure。normal Repo 尚未在该 gate 后
重跑；因此 T003 继续 `PARTIAL`，T004–T011 继续 `NOT_STARTED`。Spec190 的调用链顺序无需
重排，下一步仍是 T003 gate 后的一次 normal Repo；只有 T003 完整闭合后才能进入 T004。

随后按该 gate 运行 normal Repo。run-44、run-45 的两次启动均在模型复制/Mininet 前因手工
initializer 摘要参数错误而 `MODEL_CANONICAL_INITIALIZER_DIGEST_MISMATCH` fail-closed，
原始 supervisor 记录 `cleanup=PASS`、无残留；不计产品运行结果。修正为从源文件即时计算摘要
后，run-46 越过 launcher preflight、ACK/Selection、两 Provider material fetch，Provider-0
到达 `RUNNER_READY`，Provider-1 进入 `ASSEMBLY_HEARTBEAT progress=0.550`。随后 host guard
在 Provider-1 cold assembly 与 Provider-0 resident runner 重叠时首触
`RESOURCE_BOUNDARY:ownedSwap`：首个超限样本 `ownedSwapBytes=341704704`、
`rssBytes=7010164736`、`availableBytes=1658068992`，其中 assembly worker RSS 为
`3225214976`、Provider-0 RSS 为 `1604648960`；全轮最大 RSS 为 `7010164736`，最大
`ownedSwapBytes=341704704`，cleanup=`PASS`，无残留。run-46 没有 terminal、三轮终态、完整
C++ oracle、negative-parent 或 qualification PASS；raw evidence 已保留。该结果说明本 gate
没有引入协议/清理错误，但尚未关闭 resident-runner 与第二 cold assembly 的工作集重叠。
T003 仍 `PARTIAL`，T004–T011 继续 `NOT_STARTED`；下一步只允许在 T003 内做新的静态审查和
最小 Changed gate，不得放宽资源门或提前进入 RepoRestart。

2026-09-22：严格串行完成 T001、T002；T003 实现与静态 gate 已完成，NAC-ABE fixture 首边界已修复并通过两项 C++ focused selector。run-04 在约 12.43 分钟后由资源守卫以 `RESOURCE_BOUNDARY:ownedSwap` 停止；run-05 已让两个 Provider 到达 `RUNNER_READY` 且未触发资源门，但 Provider-1 在下一次 exact dependency fetch 前因 stale `NDNSF_DATA_V1` group no-progress 状态失败；run-07 又在 Provider-1 并发 cold assembly 时触发 `RESOURCE_BOUNDARY:MemAvailable`。新增的跨 Provider cold-assembly `flock` gate 已完成静态复核、production worker rebuild、fork-based C++ selector 1 次加 3 次重复通过，以及 Spec182 activation 9/9；run-11 已跨过资源门并让 Provider-0 到达 `RUNNER_READY`，但 Provider-1 的首个 producer-readiness wait 在返回 manifest 后仍被错误计入 no-progress，约 365 秒后失败。新增 readiness baseline gate 已通过 CodeGraph review、受影响 DI library compile-link、独立 C++ target 和 readiness/idle-gap/in-operation selectors，readiness selector 再重复 3 次通过；广泛 `unit-tests` 仍停在既有 Spec182 fixture 编译边界。run-08～run-10、run-12、run-13、run-17 均为已保留的 launcher/preflight 边界；run-12 为手工 requester digest 不匹配，run-13 为错误 tokenizer 路径，run-17 为 tokenizer directory 摘要，均未进入 MiniNDN/Repo/Provider。run-14 已通过 preflight、ACK/Selection 并跨过 readiness 边界，但由 host guard 以 `RESOURCE_BOUNDARY:ownedSwap` 停止，峰值 `303861760 > 268435456`，cleanup=PASS，仍未得到 terminal/三轮闭环。materialized-role 复用分支的 compile-link、focused selector 一次加三次重复和 CodeGraph 复核均已通过；run-15 的 manifest 解析边界已登记。run-16、run-18 和 run-19 均通过正确 manifest、ACK/Selection，Provider-0 到达 `RUNNER_READY`，Provider-1 完成 materialization 并进入 assembly worker，随后由 host guard 以 `RESOURCE_BOUNDARY:MemAvailable` 停止，cleanup=PASS，仍未得到 terminal/三轮闭环。run-18 后新增的 canonical initializer pre-worker release Changed gate 已完成 compile-link、focused selector 一次加三次重复和静态复核；run-19 证明该释放仍不足以覆盖 Provider-0 resident runner 与 Provider-1 ORT worker validation 的重叠峰值（最小 `availableBytes=1374429184`，最大 `ownedSwapBytes=174768128`）。随后 worker-session-options Changed gate 已完成 `di-native-assembly-worker`/DI library compile-link、materialized selector 一次加三次重复、CodeGraph 复核及 Spec182 worker protocol 30/30；run-20 已让两个 Provider 均到达 `RUNNER_READY`，但 host guard 仍以 `RESOURCE_BOUNDARY:MemAvailable` 停止（最小 `availableBytes=1362620416`，最大 owned swap 为 `94552064`），cleanup=PASS，仍未得到 terminal/三轮闭环；同时暴露 launcher 每 0.2 秒整读多 MB Provider 日志的诊断开销。下一步只能在 T003 内限制该诊断读取并降低真实 worker 峰值后重新执行 immutable run；当前未启动 T004 或任何后续任务。
旧→新：T001–T005不变；旧T008→T006、旧T009→T007、旧T010→T008、旧T011→T009、旧T006→T010、旧T007→T011。
Batch ID/evidence路径保持原身份，历史提交/审计不改写；下表与正文使用新任务ID。
T001、T002 的实现、C++ focused regression、compile-link 和只读静态复核已分别闭合；证据见
[b190-01](evidence/b190-01.md) 和 [b190-02](evidence/b190-02.md)。T002 的 Python 边界测试
为 8/8，C++ `Spec190AckWindow` 为 3/3；没有运行 MiniNDN/Qwen 两节点实验或性能验收。
T003 的 requester/launcher 实现、full target compile-link 与只读静态复核通过；NAC-ABE
fixture 首边界已通过“prepare 前 binding + C++ pump”修复，`PreparedRequestCompletesThroughServedProvider`
与 `PreparedConversationCommitsTwoNativeTurns` 均退出 0。真实 Qwen run-04 已进入两 Provider
生产路径，Provider-0 到达 `RUNNER_READY`，Provider-1 仍在 material bundle assembly；资源守卫随后
观察到 `ownedSwapBytes=348352512 > 268435456` 并停止运行，未观察到三轮 terminal、第二轮失败/取消
负例或真实 cleanup 后的完整验收。详见 [b190-03](evidence/b190-03.md) 和
`docs/failure-log.md`。下一步只能在 T003 内用真实 Changed gate 覆盖该资源边界、静态复审后重跑；
不能将 T003 计为 DONE，也不能跳过或并行推进后项。

补充：bounded Provider-log tail 与 post-`RUNNER_READY` NFD large-data purge gate 的最终
Python 定向回归为 88/88；其中已修复本轮 offset 后冲突 request identity 未立即 fail-closed
以及两个测试夹具断言问题。该 gate 只证明 launcher 控制逻辑，不能替代真实两节点终态；T003
仍为 PARTIAL。run-21 在 installed requester CLI/ABI contract 边界停止，已登记 raw evidence；
已只安装匹配 build artifact 的 requester 与 DI shared library，并完成 hash/help/symbol/ldd
预检；下一步使用新的 immutable run 验证真实链路。
run-22 已观察到三轮 requester 与 Provider execution/terminal，但 launcher barrier 因旧轮次
误判冲突且 READY tail scan 漏掉 purge 时机而失败；T003 仍为 PARTIAL，下一步只修复这两个
当前 gate 边界，禁止进入 T004。修复后的 barrier/current-identity filter 与 incremental READY
scan 已通过 88/88 Python gate；run-23 已完成真实三轮、purge、terminal 和 negative-parent，
但 C++ oracle 因 round-zero 日志丢失 cache-mode preamble 而失败；下一步只修复该 evidence
边界并运行新的 immutable run，仍禁止进入 T004。
run-24 已重复完成三轮、purge、terminal 和 negative-parent，但多轮 C++ oracle 要求每个
`requester-N.log` 都包含 run-scoped preamble；T003 仍 PARTIAL，下一步只修复 per-turn evidence
视图并运行 run-25。
round-zero preamble 保留修复及 split regression 已通过，相关 Python gate 为 89/89；immutable run-25 已完成三轮 requester、Provider execution/terminal、negative-parent 和 post-`RUNNER_READY` purge；C++ oracle 为 `CACHE_DIAGNOSTIC_PASS`，supervisor cleanup 为 PASS。该轮显式记录 `fullPathQualification=NOT_RUN`，且尚未执行 C++ 父进程/pipe 实时读取门；T003 仍为 PARTIAL，不解锁 T004。新增 run-26 在 MiniNDN 启动前发现固定 model-source cache payload 的 `0600` owner write bit，`MODEL_CANONICAL_SOURCE_NOT_IMMUTABLE` fail-closed；无协议结果，已保留 raw evidence。run-27 修复权限后真实 requester 三轮和 live markers 均出现，但未筛选的 Boost.Test driver 还执行 15 个无关 Spec185 测试并整体失败。run-28 已用限定 `Spec190LiveTurns/ParentPipeReadsLiveEventsBeforeTerminal` 返回 0，且 cache-compatible 三轮、Provider terminal、purge、negative-parent 和 C++ `CACHE_DIAGNOSTIC_PASS` 均通过；仍因 `fullPathQualification=NOT_RUN` 保持 PARTIAL。run-29 真实 normal Repo 传输到 Provider-0 `RUNNER_READY` 和 Provider-1 assembly，随后因 `ownedSwap=306475008` 越过资源门而停止；run-30 两 Provider 都到达 `RUNNER_READY` 后仍以 `ownedSwap=285323264` 停止，cleanup PASS、未形成 requester 终态；下一步只在 T003 内降低原生 cold-session 峰值并重验，不放宽资源门。

run-31 使用新的 ORT bounded-session candidate 和 normal encrypted Repo 重跑；模型源 cache 命中、ACK/Selection 及 Provider-0 `RUNNER_READY` 均已观察，但 Provider-1 尚未完成 runner 时 host guard 再次以 `RESOURCE_BOUNDARY:ownedSwap` 停止，最大 `ownedSwapBytes=326877184`，`minAvailableBytes=1654267904`，`maxTotalRssBytes=7692361728`。supervisor cleanup=PASS、无残留进程，未生成 requester terminal/run-record；因此 T003 仍 PARTIAL，下一步只能在 T003 内基于该资源证据做静态复审和最小修复，T004–T011 继续保持 NOT_STARTED。
run-32 在 worker 采用 `ORT_DISABLE_ALL` 后以同一 normal encrypted Repo candidate 重跑；模型源 cache、ACK/Selection、Provider-0 `RUNNER_READY` 以及 Provider-1 `DEPENDENCY_FETCH=complete`/`ASSEMBLY_STARTED` 均已观察。Provider-1 assembly worker 峰值触发 host guard 的首个边界 `RESOURCE_BOUNDARY:MemAvailable`：`minAvailableBytes=1605091328 < 1610612736`，`maxOwnedSwapBytes=92225536`，`maxTotalRssBytes=7507595264`；supervisor `cleanup=PASS`、`returncode=-2`、无残留进程，未生成 requester terminal、run-record 或完整 C++ oracle。T003 仍为 PARTIAL；下一步仍只在 T003 内基于该边界做静态复审和最小修复，T004–T011 继续保持 NOT_STARTED。

本轮完成唯一的 `worker-child-skip-ort-session` Changed gate。C++ 静态审查确认 assembly
worker 的结构/digest/IO/wire 校验不依赖完整 ORT session；生产 Provider 在 worker 返回后
负责唯一 authoritative ORT session 创建和 warmup，且在 `RUNNER_READY` 前完成。公共
in-process parity 路径仍保留 ORT load。受影响 DI/worker/provider/test targets 已重新
compile-link 并安装，`spec190-materialized-role` 1/1、cold assembly selector 1/1、
`spec190-readiness-progress` 15/15 均通过。该 gate 只降低重复 ORT session 的冷启动峰值，
尚未证明 normal Repo 完整链路；下一步仅运行一次新的 normal Repo，T003 仍为 `PARTIAL`，
T004–T011 继续保持 `NOT_STARTED`。

run-33 在 normal Repo 启动前因冻结参数错误命中 `PROVIDER_BINARY_DIGEST_MISMATCH`：误将
shared library 摘要用于 provider executable 字段。该边界已保留 raw evidence 并登记到
`docs/failure-log.md`；没有 MiniNDN、Repo 或 Provider 结果，不计入 Changed gate。校正
executable 摘要后只使用新的 run root 重试一次 normal Repo，T003 仍为 `PARTIAL`，T004–T011
继续保持 `NOT_STARTED`。

run-34 在校正预检参数后完成 normal Repo 的 cache、ACK/Selection、material fetch，并让
Provider-0 到达 `RUNNER_READY`；Provider-1 在 `MODEL_MATERIALIZED`/`WORKER_START` 后因
`RESOURCE_BOUNDARY:MemAvailable` 停止，`minAvailableBytes=1336852480`、
`maxOwnedSwapBytes=5410816`、`maxTotalRssBytes=7660179456`，assembly worker 峰值
`2896437248` bytes。cleanup PASS、无残留，但无 requester terminal/run-record/oracle 完整
结果。该结果确认 worker ORT session 去重不足以关闭冷启动物化/结构组装峰值；T003 保持
`PARTIAL`，不追加第二个 Changed gate，T004–T011 继续保持 `NOT_STARTED`。

## Execution Progress

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [T001 Phase timing](#t001-phase-timing) | DONE | — | [b190-01](evidence/b190-01.md)；216/216 compile-link，`Spec190Timing` C++ regression 3/3，静态复核 PASS；MiniNDN/Qwen/performance unobserved | 2026-09-22 04:33 -05:00 |
| [T002 ACK window](#t002-ack-window) | DONE | T001 | [b190-02](evidence/b190-02.md)；Qwen 1000ms profile、非法值校验、C++ compile-link、Python 8/8、`Spec190AckWindow` 3/3、静态复核 PASS；真实两节点/Trust Schema签名验收未运行 | 2026-09-22 05:10 -05:00 |
| [T003 Live turns](#t003-live-turns) | DONE | T002 | [B190-03](evidence/b190-03.md) r34：真实三轮 requester/两 Provider ORT、C++ parent/pipe live events、terminal/checkpoint/KV restore、C++ oracle、最终 purge/cleanup 全链路 PASS；full-token latency qualification 仍归 T011 | 2026-09-23 16:08 -05:00 |
| [T004 Persistent Repo owner](#t004-persistent-repo-owner) | DONE | T003 | [B190-09](evidence/b190-09.md)；4/4、重复3次、文件故障3/3、ASan 4/4；网络/真实模型重启 deferred | 2026-09-23 |
| [T005 Query and reuse](#t005-query-and-reuse) | DONE | T004 | [B190-10](evidence/b190-10.md)：完整 identity lookup、Runtime/OS restart、竞争/取消/fsync、缺依赖修复、stale cleanup、initializer/layer identity 及普通根 `build/` C++ 4-case ×3 PASS；ASan/UBSan deferred by user scope | 2026-09-23 16:08 -05:00 |
| [T006 Protected material reuse](#t006-protected-material-reuse) | IN_PROGRESS | T005 | [B190-12](evidence/b190-12.md)、[B190-14](evidence/b190-14.md)、[B190-16](evidence/b190-16.md)、[B190-17](evidence/b190-17.md)、[B190-18](evidence/b190-18.md)、[B190-19](evidence/b190-19.md)、[B190-20](evidence/b190-20.md)、[B190-21](evidence/b190-21.md)、[B190-22](evidence/b190-22.md)、[B190-23](evidence/b190-23.md)、[B190-24](evidence/b190-24.md)、[B190-25](evidence/b190-25.md)、[B190-26](evidence/b190-26.md)、[B190-27](evidence/b190-27.md)、[B190-28](evidence/b190-28.md)：默认 factory 已通过 Provider/Selection/credential/Face-backed exact signed Data fetch/assembly/terminal C++ sub-gate，focused 14/14 + r5/r6/r7 全部通过；完整 prepared-request 全量回归此前在独立 preparation timeout 后停止，仍不计 PASS。真实 NFD/MiniNDN route、online Controller confirmation、durable key recovery、NAC-ABE inline unwrap、current grant-Selection-placement rebinding、revoke invalidation、精确 missing-object fetch 和完整 protected restart 未完成，protected miss 门不解除 | 2026-09-23 21:09 -05:00 |
| [T007 Resident session](#t007-resident-session) | NOT_STARTED | T006 | 从Spec189 R261承接，真实ORT与owner验证待做 | 2026-09-22 15:08 -05:00 |
| [T008 Stage transfer](#t008-stage-transfer) | NOT_STARTED | T007 | actual bundle/wire字节与多发修复待做 | 2026-09-22 15:08 -05:00 |
| [T009 Finalize and drain](#t009-finalize-and-drain) | NOT_STARTED | T008 | 先定位控制闭环首边界，再限定修复 | 2026-09-22 15:08 -05:00 |
| [T010 Convergence and candidate](#t010-convergence-and-candidate) | NOT_STARTED | T009 | 新增真实Repo/重启/字节预算oracle与preflight；门未执行 | 2026-09-22 03:00 -05:00 |
| [T011 Matched experiment](#t011-matched-experiment) | NOT_STARTED | T010 | 三组warm配对、cold/三次restart/缺层及SC-001–009待验收 | 2026-09-22 03:00 -05:00 |

## Shared Execution Contract

每项包含反例/实现/只读静态审查/定向验证/证据，不机械拆成行政子任务。
批次、五lane、dynamic profile与收敛门引用[plan.md](plan.md#logical-batch-quality-plan)。
以下测试target/selector及新test文件均为planned，注册/链接是对应任务的一部分，不能宣称已有命令通过。
Native assertion/fixture/oracle均C++；Python仅启动和配置；共享业务生产路径，不写fake ACK或fake runner当模型证明。
每批记录一个 `evidence/b190-0N.md`，包含Review trace、Coverage matrix、Closure decision、
四类miss、build计时和实际结果；没有实测不勾选。失败保留raw并更新docs/failure-log.md。

## Per-Change Static Review Gate

每次修改生产C++、测试/fixture、launcher、profile/config、候选/证据元数据或本Spec文档后，
都必须先完成受影响范围的只读静态复审，才能构建、安装、定向测试、预检或重跑实验。复审
至少核对固定baseline与完整diff、生产入口/callers、owner及失败/清理顺序、测试/oracle、
Waf/安装/符号闭包和五lane coverage，并登记`static`、`compile-link`、`runtime-test`、
`unobserved`四类miss及首个失败边界。任何后续修改都会使受影响范围的旧`STATIC_PASS`失效。
纯文档修改不触发产品构建或模型实验，但必须完成Spec结构/交叉一致性检查并更新本checkpoint
或当前evidence。`STATIC_PASS`不是行为、性能或资格PASS；当前T003的资源边界仍须在本项内
形成真实Changed gate后才能重新测试，T004–T011保持`NOT_STARTED`。

## Strict Serial Dispatch Gate

`ACTIVE_TASK_ID` 只能是当前进度表中第一个不是 `DONE` 的任务；本 checkpoint 的
`ACTIVE_TASK_ID=T003`。后续任务即使设计上已知存在风险或未来可能 `BLOCKED`，在其紧邻
前项 `DONE` 前都必须保持 `NOT_STARTED`，不得提前派发、预实现、预验收或把结果回填到前项。

每个任务只有在以下条件全部满足后才可将状态改为 `DONE` 并解锁下一行：本任务的实现、
接口/设计核对、只读静态复审、受影响构建、具名 C++ 回归/动态验证、负例与生命周期清理、
五 lane evidence、四类 miss retrospective 和 closure decision 均有持久证据；硬验收不能写
`UNOBSERVED`、`NOT_RUN` 或 `PARTIAL`。`STATIC_PASS`、单项 focused PASS、构建成功或
cleanup PASS 都不能解锁下一任务。

当前任务失败时只在当前任务内登记稳定的 recovery/Changed gate，保留原始 run 和首边界，
先静态复审再重建/重跑；失败、`PARTIAL` 或 `BLOCKED` 均停止调度。只有当前任务重新满足
完整 exit 才能继续下一个任务；若后项发现前项缺陷，重开最早受影响任务并暂停后项，不能
通过来回跳转或把欠账转交 T010/T011。严格串行模式不使用 `[P]` 或并行执行示例。


## Phase 1: T001

### T001 Phase Timing

- [x] T001 Deliver correlated phase timing in `NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.cpp`, `ndn-service-framework/ServiceUser.cpp`, and `tests/unit-tests/spec190-timing.t.cpp`.

**Outcome / owner**：Core/DI/CLI各自产生阶段事件，可以区分网络、认证、规划、模型计算、交付及终态等待。
**Read**：research.md、CD-01、现有RuntimeTiming、NativeInferenceClient::beginCoreRequest/commitConversationTurn、r260 raw。
**Write scope**：上述文件及相应.hpp（仅必要声明）、NativeInferenceClient.cpp、ORT adapter真实run计时、
examples/DI_NativeRequester.cpp、tests/wscript；不重做通用logging框架。
**Design binding**：CD-01 + TurnTiming；现有业务签名不改，字段owner按data-model；Design status: proposed。
**Steps**：核对日志身份→C++ fixture定义事件顺序/遗漏/重复反例→接入真实发布/验证/run/交付点→
独立C++解析校验本地持续时间→冻结静态复审→增量构建/回归→保存baseline字段缺口，不重跑昂贵模型复现已知60秒。
**Acceptance**：`spec190-latency-tests / PhaseTiming` 拒绝跨request/attempt拼接、缺阶段不能报0、steady计时不受wallclock回拨影响；
真实run计数不含warmup，首token和checkpoint不能混淆。五lane与新target定义/链接闭包均登记。
**Exit**：指标和C++回归可用；性能改善NOT_CLAIMED。Batch B190-01，风险低，dynamic none。

## Phase 2: T002

### T002 ACK Window

- [x] T002 [US1] Enforce the one-second Qwen ACK profile with verified closure regressions in `Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`, `examples/DI_NativeRequester.cpp`, and `tests/unit-tests/spec190-ack-window.t.cpp`.

**Outcome / owner**：DI profile实际采用1000ms，Core维持原认证、截止与候选快照契约。
**Read**：CD-01；ServiceUser::handleAckCollectionTimeout、BeginCollaborationWithProviders、NativeOfferAdmission；T001事件。
**Write scope**：launcher的预算/arg配置、C++ requester配置读取和生效值输出、原生test/Waf；
只有定向反例证明原生超时路径缺陷时才改NativeInferenceClient/ServiceUser，并明确Changed gate。
**Design binding**：CD-01；不改变通用5000ms默认、不新增提前关闭算法、不在ACK前加载模型。
**Steps**：消除模型大小绑定ACK窗口→覆盖显式值传递/非法值→C++可控时钟触发发布和冻结→
检查首次/续轮认证路径→静态复审后限定构建/测试。
**Acceptance**：`AckWindow`：1000ms生效；0/负/大于总deadline拒绝；999/1000/1001ms、late/duplicate/invalid/negative ACK、
认证未完、cancel/deadline同时触发，冻结一次且不可变；缺角色无Selection；原grant/offer校验无绕过。
**Exit**：定向C++通过，真实1秒成功率交T011；超时诊断先保留首边界，不自动重试延长。Batch B190-02。

## Phase 3: T003

### T003 Live Turns

- [x] T003 [US2] Stream events during execution and reuse one native Conversation in `examples/DI_NativeRequester.cpp`, `NDNSF-DistributedInference/cpp/ndnsf-di/Conversation.cpp`, `examples/Spec189MaterialFetchOracle.hpp`, and `tests/unit-tests/spec190-live-turns.t.cpp`.

**Outcome / owner**：原生CLI真实边生成边消费，在同一Runtime/PreparedModel/Conversation依次完成三轮。
**Read**：CD-02；PreparedModel.hpp里的RequestHandle/EventReader、Conversation::State、launcher逐轮start流程。
**Write scope**：CLI、Conversation.cpp及必要PreparedModel.cpp生命周期修复、launcher turns配置与启动接线、test/Waf；
不新增Python对话实现、不新增服务模式/UI。
**Design binding**：CD-02；单轮兼容、turns数组契约；每轮当前options/generationId/输入与前轮checkpoint绑定。
**Steps**：先完成 B190-03A 的 source identity、inline material chunking、native runner contract
和 C++ candidate gate；再用 C++可阻塞fixture验证事件先于terminal→接入实时读取/flush→数组driver循环复用对象→
验证立即下一轮与真正并发busy边界→launcher一次启动driver→静态复审/回归。
**Acceptance**：`LiveTurns`：首token在terminal之前；无丢失/重复/乱序；EOS/EOT/预算均停止；三轮不同requestId但同对象；
第二轮失败后无第三轮；取消仍释放；result后立即request不偶发busy；真正同时request拒绝。
必须有C++父进程/pipe调用真实CLI，在后续生成被fixture暂停时能读到首事件；测试暂时空队列不当EOF、
失败/取消没有正常terminal也有界退出，不能只用EventReader内存fixture证明flush。
**Exit evidence**：r34 integrated PASS；三轮真实 requester/Provider logs、`run-record.json`、
`ndn-cache-purge.json` 和 `SPEC189_CPP_ORACLE_PASS` 保存在 [B190-03](evidence/b190-03.md)。
**Exit**：B190-03A 已明确 PASS 且 C++driver与fixture闭合，同模型同handle证明交T011；不靠checkpoint文件重开新Conversation通过。Batch B190-03；sanitizer deferred by user scope。

## Phase 4: T004

### T004 Persistent Repo Owner

- [x] T004 [US5] Reopen a fixed per-node Repo safely in `NDNSF-DistributedRepo/src/backends/FilesystemRepoStoreBackend.cpp`, `NDNSF-DistributedRepo/src/RepoNode.cpp`, and `tests/unit-tests/spec190-repo-restart.t.cpp`.

**Outcome / owner**：固定node根跨run/进程重启，单writer恢复可读committed数据；run cleanup不删除共享payload。
**Read**：CD-07、BackendOwnershipLease、recoverOrphans、RepoNode::registerServices、launcher prepare_fixed_workspace/cleanup_encrypted_repository。
**Write scope**：上述后端/Node仅补已发现缺口；C++ requester/Provider注入单owner与固定配置；launcher只配置根/启动，禁止在Python补恢复/commit逻辑；native test及tests/wscript，现有安装target `ndnsf-distributed-repo`。
**Design binding**：CD-07；复用现有constructor/范围接口，node/deployment/root/owner稳定，boot/catalog状态新建。
**Steps**：先真实后端跨owner/进程close-reopen C++fixture→验证已有恢复/锁能力→接per-node固定根并排除reset/cleanup→测试crash/半提交/双writer/损坏/满额和目录逃逸→复审后只编受影响Repo/调用者。
**Acceptance**：`RepoRestart`至少3次新PID复用同根，payload hash/count/bytes不变；所有committed对象可查读，半提交不成为hit，第二writer BUSY，不夺锁；有效read期间不删对象；普通失败清理只清owned staging；启动不将整权重库加载入内存，测试小ONNX/文件与子进程均RAII收尾。
**Exit**：原生存储/owner证明闭合，网络Repo服务及真实模型重启交T011。B190-09，asan+文件故障注入。

## Phase 5: T005

### T005 Query and Reuse

**Prepare-level requirement**：production `user.prepare(model)`在拆层/导出/打包前查Repo；完整命中直接复用已校验prepared receipt。C++ fixture记录split/export/package/STORE计数：第二次调用及新进程重启后均为0，返回材料/IO契约与冷准备等价；变化内容/配置必须miss，部分缺失仅重建缺失依赖闭包。hash/必要inspection与授权成本单列，不能先完整准备再只去重STORE。

- [x] T005 [US5] Reuse verified committed publications before STORE in `NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoSourceProvider.hpp`, `NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.cpp`, and `tests/unit-tests/spec190-repo-lookup-reuse.t.cpp`.

**Outcome / owner**：User prepare查询完整publication identity，命中直接引用，避免重复ingest/分层/存储及大型vector物化。
**Read**：CD-08；RepoSourceProvider::load/publish和已有Spec189RepoPublication回归；Runtime.hpp两个Repository port、RepoClient::requestManifest。
**Write scope**：既有adapter及Runtime.hpp/.cpp新增lookupPrepared可选port、NativeCanonicalArtifactPublisher必要接线、ModelPreparationCache.cpp/.hpp前置lookup与恢复、PreparedModelPackage.hpp版本化元数据接线、NativeRequestCatalog factory引用恢复、examples/DI_NativeRequester.cpp、C++fixture/Waf；不复制另一个Repo publisher。
**Design binding**：CD-08；稳定完整identity替代仅sourceDigest根；旧port默认nullopt兼容，不改原请求授权/Selection。
**Steps**：已有首写/复用回归扩到真正close-reopen→完整identity/key冲突反例→小manifest先查的native快路径→miss才bounded publish/root-last，命中保留rollbackOwned=false→复审/构建/定向测试。
**Acceptance**：`RepoLookupReuse`第二run STORE/新payload/materialization计数0；请求元数据/本地hash读另计；同source不同initializer/profile/layer不可错误命中；缺子对象只补缺失；unauthorized/conflict/unavailable不当miss盲写；lookup/publish竞争、取消不删他人已提交对象，误清理旧run材料反例必须失败。
**Exit**：native canonical复用通过，不据此声明protected密文/网络serving复用已实现。B190-10 事务故障矩阵通过；sanitizer deferred by user scope。

## Phase 6: T006

### T006 Protected Material Reuse

- [ ] T006 [US5] Preserve current authorization during durable material reuse in `NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoEncryptedLargeDataStore.hpp`, `ndn-service-framework/ServiceUser.cpp`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.cpp`, and `tests/unit-tests/spec190-protected-material-reuse.t.cpp`.

**Outcome / owner**：真实Repo路径新run新grant合法复用既有材料/组装产物；不靠临时目录保留或compatibility bypass骗过验收。
**Read**：CD-09；Core publishEncryptedLargeData/EncryptedLargeDataRangeStore，Source析构清理、当前grant/host plaintext lease、assembler protected-miss门。
**Write scope**：上述Core/Repo/DI owner及必要hpp、NativeProtectedProvider/runner factory接线、test/Waf；不重做加密算法。
**Design binding**：CD-09，当前BLOCK生产编码；先在契约冻结durable key-reference/serving恢复、新grant绑定及retention公共签名、owner/错误/取消/失效流程，标明与原request-scoped API兼容；独立只读审查后才解除该gate。
**Steps**：复用现有crypto-owner而非在Repo藏key→完整加密identity/receipt与恢复事务→显式durable与transient清理分离→当前Selection后校验材料/assembled命中→缺层走原保护fetch→旧grant/key/boot等C++反例→冻结组合审查/回归。
**Acceptance**：`ProtectedMaterialReuse`旧/错grant、失效key、错AAD/ciphertext、非法保留policy仍拒绝；合法restart后真实Repo lookup/read/serving可用，相同protected identity大payload不重复STORE；材料/assembled热命中零material网络payload，缺一对象只取该role必要范围；保留原副本安全与迟到清理边界。Provider新boot必须新session，旧KV receipt不能命中；不能把T007明文diagnostic通过计为本任务通过。
**Exit**：retention/source-lifetime、authenticated key-reference 和 Repo durable identity metadata/restart-read atomic gates 已完成；新安全契约的 Core key-reference/serving recovery、
Provider serving/assembled hit 和生产回归仍未完成，真实Qwen证明交T011；接口未闭合保持
`PARTIAL`，不删除原安全门。B190-12。

## Phase 7: T007

### T007 Resident Session

- [ ] T007 [US3] Reuse bounded CPU loaded sessions with isolated request evidence in `NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeSessionCache.hpp`, `OnnxRuntimeModelRunner.cpp`, and `tests/unit-tests/spec190-resident-session.t.cpp`.

**Outcome / owner**：Provider-owned CPU session复用，旧ctx/grant/KV/profile不会成为新请求状态；退出实际释放。
**Read**：CD-04/data-model；NativeRunnerPreparation、NativeCanonicalOnnxAssembler已产生的digest；ProviderArtifactCache的metadata-only契约。
**Write scope**：新cache.hpp/.cpp、同目录OnnxRuntimeModelRunner.hpp/.cpp、DI/ExecutionEvidence.hpp/.cpp、DI/Provider.cpp、examples/DI_NativeProviderExecutable.cpp、必要factory接线、C++fixture与Waf；不迁移通用artifact cache。
**Design binding**：CD-04；fresh wrapper + shared load owner；原单参数入口保持；独立request与load证据。
**Steps**：C++自生成小ONNX/外部权重fixture→完整key与lease状态→single-flight/idle清理/close→fresh evidence与真实ORT调用→Provider host/库默认接线→显式disabled控制→逐项静态复审及组合测试。
**Acceptance**：`ResidentSession`实际ORT两次输出正确、同key只加载一次；key每个关键字段变更miss；并发/TTL/换模型/evict/close/loading-failure/cancel均不泄漏或UAF；失效授权即使已有session也拒绝；旧profile不能改request/attempt，重复EndProfiling反例；所有临时ONNX由RAII清理。首次loader请求取消但另有合法waiter、全部waiter取消、close后load迟到完成均有C++反例，不得借发起请求ctx延长生命或重新插入已关闭cache。并发evict/acquire、在用项retire拒绝新租用、close后禁止cold fallback、drain超时/最终成功均验证；1slot默认只作用本profile的显式resident配置，原无cache入口和其他profile行为不变。
**Exit**：native fixture通过、owner计数闭合；Qwen驻留命中/资源趋势交T011。Batch B190-05，sanitizer deferred by user scope；另跑有界TSan `ResidentSessionConcurrency` 覆盖acquire/release/evict/close/single-flight竞态（plan定义范围/预算）；加密临时backing和CUDA bypass记录，不声称其驻留已支持。

## Phase 8: T008

### T008 Stage Transfer

- [ ] T008 [US4] Bound stage data transfer in `NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.cpp`, `NdnsfCollaborationDependencyIo.cpp`, and `tests/unit-tests/spec190-stage-transfer.t.cpp`.

**Outcome / owner**：按真实tensor/encoded/wire预算发送当前stage所需数据，不多发KV/权重/完整logits；材料流量观察点可被独立校验。
**Read**：CD-06、outputForEdge/withoutProviderLocalState、lastLogits/makeTokenFeedback、dependency publish/prefetch、T001事件。
**Write scope**：上述DI路径、NativeEpochCoordinator.cpp、RuntimeTiming、必要Core分段/wire观察点、test/Waf；额外mask/position删除必须同时改sealed边契约和接收重建，不能仅改发送端。
**Design binding**：CD-06；保留业务签名，统一identity计数，counter累计与delta明确；Design status: proposed。
**Steps**：扩展既有Provider-local KV C++fixture捕获实际bundle→构造prompt/delta/decode/finalize动态输入→独立解码/计算shape字节→接入生产wire/retry/本地copy分层指标→仅修有反例证明的多发/复制→冻结复审/回归。
**Acceptance**：`StageTransferBudget`注入额外KV、weights、full logits、重复bundle、缺失position/lineage和乱序必须检出；实际tensor集合及数值与sealedcontract一致，允许合法metadata/重传但单独计；cumulative snapshot不能重复累计。layer/assembled/resident三个状态分列，C++fixture注入已知非零材料传输验证计数，不以尚未实现的热缓存作本项前置。
**Material acceptance owner**：热材料零payload与缺对象精确补取完整归T006原生生产回归及T011真实系统验收，SC-007/009保持不变；T008只完成stage数据契约与流量计量，不宣称保护材料缓存已有命中能力。
**Exit**：C++预算/正确性反例闭合，未采集字段标unknown；不靠降低logging或断言network=0常量通过。B190-08；sanitizer deferred by user scope。

## Phase 9: T009

### T009 Finalize and Drain

- [ ] T009 [US2] Close the authenticated FINALIZE lifecycle in `NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.cpp`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp`, and `tests/unit-tests/spec190-terminal-drain.t.cpp`.

**Outcome / owner**：健康轮不等满30秒补偿窗；持久commit、rollback补偿、失联恢复及最终drain正确。
**Read**：CD-03；publishConversationControls、finalizeProviderState、waitConversationPromotion、Core协作发布服务owner；r260 checkpoint→terminal 29.9秒证据。此项不是“将30000常量改小”。
**Write scope**：上述DI文件、必要Conversation coordinator/Runtime owner、已确认有缺陷的Core通用owner（若触及需补Core回归），test/Waf。
**Design binding**：CD-03；诊断子步骤可执行，生产patch须先按实际首边界细化FN/owner；当前不能称生产修复已ready。
**Steps**：T001关联COMMIT发布/验证/ack、durable journal commit、FINALIZE发布/服务存活/接收/接受→找首次缺失或拒绝原因→补最小C++重现→修发布生命周期/身份/处理或唤醒，而非删barrier→复审/回归。若同handle已完全解决，只保留必要诊断与回归，不强造Core重构。不得直接增加新握手协议掩盖现有控制消息未送达。
**Acceptance**：`TerminalDrain`：正常FINALIZE提前结束等待；丢失FINALIZE已commit的KV保留到原expiry；未commit超时rollback；重复/乱序/错身份不双提交；journal失败补偿；关闭中迟到回调安全；drain返回结果真实。必须覆盖两角色仅一侧COMMIT成功/另一侧commit ACK丢失：不成功checkpoint、不进入下一轮、已提交侧补偿；与持久commit之后丢FINALIZE保留KV的情况严格区分。fixture显式拥有Face/io/scheduler直到worker join，重复20次；原生受控时钟验证健康FINALIZE不依赖补偿超时推进，真实owner回调完成/退出与异常保留期限均在本项测试通过。
**System acceptance owner**：MiniNDN健康checkpoint→Provider terminal≤2秒由T011完整负责，SC-003/005门不变；达不到须重开本项修复并重验受影响候选，不能以压缩超时通过，也不能称系统指标已由本项fixture证明。
**Exit**：有首边界证据和生产回归，保持持久提交含义；真实时延交T011。Batch B190-04；sanitizer deferred by user scope。

## Phase 10: T010

### T010 Convergence and Candidate

- [ ] T010 Deliver a closed candidate and native latency oracle in `examples/Spec189TwoProviderOracle.cpp`, `examples/wscript`, `Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`, and `specs/190-multiturn-latency/evidence/b190-06.md`.

**Outcome / owner**：生产入口与设计一致，oracle辨别真流式/真session复用/正确KV；错误候选不得启动。
**Read**：所有CD/FR/SC及T001–T009已完成证据；当前安装工具和launcher preflight；原Spec189协议oracle。
**Write scope**：复用oracle规则的Spec190薄入口（新文件时登记实际路径）、Waf、C++oracle反例fixture、
必要preflight mutation tests、active文档/API对应变化；不新建泛化平台或容器pipeline。
**Design binding**：CD-05–09；新target有definition/link/install映射；签名/API/双PDF仅同步本Spec已实现变化。
**Steps**：先写假profile/假cachehit/假token/错attempt/缺KV/缺终态反例→接入共享C++oracle→
冻结源/运行/输入/配置身份→错hash/错config/缺依赖零启动检查→只读CodeGraph+源审查完整五lane→修正→重审。
**Acceptance**：原生focused selectors与适用spec189回归、动态owner门、安装身份通过；语义/安全/owner/证据缺口为零；
记录PASS只指design-code convergence与preflight，不是性能PASS。有未解决控制性缺口则BLOCK T011。
还要验证Repo固定根不在任何cleanup目标内、单writer配置、现存数据不能当错误候选的可删除staging；
新增C++oracle必须检查真实Repo提交/查询/读取、restart、payload delta及实际wire，拒绝compatibility假通过。
**Exit**：一个不可变candidate READY_FOR_EXPERIMENT；T001–T009全部DONE，无partial依赖；本项oracle/preflight与收敛检查通过。Batch B190-06。

## Phase 11: T011

### T011 Matched Experiment

- [ ] T011 Validate matched three-turn performance using `Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`, the installed `spec190-multiturn-oracle`, and `specs/190-multiturn-latency/evidence/b190-07.md`.

**Outcome / owner**：以真实两节点三轮输出/KV/时间/资源共同证明改善，不只报测试数。
**Read**：T010已冻结candidate、quickstart、CD-05、r260输入/采样和raw；不重新生成模型资产。
**Write scope**：新run roots与本批evidence/tasks；不得运行中修源码、换安装库或改候选。
**Steps**：验证系统安装闭包→先一组匹配smoke，失败停止并保留首边界→通过后至少3组配对，
控制组/处理组交替→用C++oracle逐轮检查token/事件/KV/load/FINALIZE→统计全部时延和资源→清理本次临时进程。
**Acceptance**：SC-001–009、T009≤2秒正常finalize门；失败不丢样本；若累积真实请求观察不足60秒，
追加相同样本，不延长单轮sleep；记录每轮首token、decode、checkpoint、terminal/退出及总时长，解释13秒残差归属。
**Exit**：仅满足全部目标才标Spec190性能PASS；否则PARTIAL，定位并回到所属任务；不因此关闭Spec189 full Repo资格。Batch B190-07。

新增Repo-enabled验收按CD-06–09：同固定根首次cold提交、至少三次Repo/Provider进程重启、合法warm查询命中、
一次选中材料缺失的真实fetch；control/treatment均相同warm状态。磁盘目录/manifest/digest、STORE计数、
material payload/wire与session load独立核对；不能仅靠旧cache-compatible oracle通过。

## Dependencies and Strategy

**Execution mode**: STRICT_SERIAL。
T001 → T002 → T003 → T004 → T005 → T006 → T007 → T008 → T009 → T010 → T011。
其中 T004–T009 按生产调用链固定为：固定 Repo owner/restart → prepare lookup/reuse →
protected material reuse → Provider resident session → stage transfer → FINALIZE/drain。
每个Depends都是完成依赖，不是仅实现/静态通过依赖；不允许[P]、独立任务先跑或跨任务暂缓测试。
每项内部按设计核对/必要诊断→实现及测试→只读静态审查→修复复审→受影响构建/原生回归/适用动态验证→证据与DONE闭合。
一个顶层任务对应一个可验收批次，内部步骤不是可跳转的顶层任务；共用构建树，不为串行而全树重编。
未完成、PARTIAL或BLOCKED时停在当前项修复；T006安全契约与T009诊断分别在所属任务闭合，不能跳去后项。
最终T011只拥有真实两节点系统/性能验收；T001–T009自己的原生回归、负例和生命周期门不得留给T010/T011补做。
局部DONE不等于SC或整Spec PASS；原有真实实验要求全部保留。若末项发现前项缺陷，重开最早受影响项，
暂停后项并修复/重验依赖闭包；这是受控失败恢复，不是正常执行顺序的来回跳转。
