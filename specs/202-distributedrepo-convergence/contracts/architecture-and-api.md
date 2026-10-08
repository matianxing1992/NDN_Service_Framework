# Architecture and API Contract

**Design status**: DB001 static baseline已关闭；DB002–004 READY_FOR_IMPLEMENTATION，用户已于2026-10-08授权在Repo实施全部任务。产品convergence仍BLOCK，重建/完整证据由T009/T010关闭。

## Owners and Public Call Paths

| Owner | Responsibility / call path | Lifetime / prohibition |
| --- | --- | --- |
| `ArtifactRepositoryApi` | application publish/begin/archive/status/receipt/availability/fetch；In-App访问本地store，Persistent通过借用ServiceUser和RepoClient | native Impl owns本地lease/目录；Persistent借用owner必须存活；不持有第二份remote durable catalog |
| `RepoClient` | 公开local primitives及request* codecs/ServiceUser bridge | local调用限同进程；network只走真实注册服务，不用Python模拟响应作网络资格 |
| `RepoNode` | registered handlers、selected seed durable barrier、operation worker及receipt/current-location | worker join/drain、Face/scheduler lifetime由既有owner管理；不新增Core授权实现 |
| filesystem native stores | payload/manifest/range staging、SQLite操作/target/receipt/location事务、lease与schema | bytes完成且durable receipt匹配后可见；应用root与serverroot隔离 |
| `ServiceUser/ServiceProvider/Core` | grant、request admission、NAC/签名、ACK/Selection/Response及认证Provider来源 | Repo消费已有身份/权限事实；不强制isAuthorized、不修改SVS timing掩盖失败 |
| pybind/facade +DI resolver | typed projection、强持有native user、GIL释放、generic identity适配、授权物化 | 没有Python replication/catalog状态机；缓存不是Repo replica；无原始source path传给远端DI |
| `LegacyRepoImporter` | 显式offline snapshot/import/verify/no-replace/replay | 不默认打开旧live DB；旧placement字段只能做provenance |

现有 public signatures 保持：`fetchFile(const ArtifactReference&, const std::string&) const`、`fetchFile(const ArtifactHandle&, const std::string&, int) const`、`RepoNode::runPublishOperationPass(...)`（private worker实现）、`importLegacyRepo(config, observer)`。实现阶段先核对当前header精确签名；本Spec无新增 public RPC、public wire字段或新的Repo类型。

## DB001 - Baseline and Scope Convergence

**Files**: `gap-audit.md`、`supersession.md`、`validation-matrix.md`、现有公开headers/pybind/facade、`tests/wscript`和Repo `wscript`。

读取dirty overlay作为实际源码；复核源/consumer/definition TU/target/output，逐入口映射原59 case。严格parser、旧manifest/catalog permissive解析属于需要负例检查的范围，不在未证明违约时重写codec。任何新语义发现先修契约，再执行受影响任务。保留原Spec产品目标与全部未完成资格，不移除失败或未覆盖入口。公开artifact控制mode默认仍为Collaboration，调用者显式选择才用Targeted；任何lower-level Targeted默认差异必须由C++明确隔离或修复，并以默认/显式两种调用的实际path验证。保留ArtifactReference与ArtifactPublishResult.reference兼容，破坏性变更须versioned migration，不靠Python隐藏差异。

## DB002 - Destination Commit Transaction

**Path preconditions**: 调用者在本次fetch期间独占destination namespace；不同API/process同时修改同一路径不受支持。保留leaf symlink本身，不follow它；目录和其他special leaf在覆盖前拒绝。parent leaf symlink受既有O_NOFOLLOW目录屏障限制；不新增跨process锁或悄悄扩展路径支持。

**Files / symbols**: `NDNSF-DistributedRepo/src/ArtifactRepositoryApi.cpp` 的两个`fetchFile`及其anonymous-namespace destination-install helper（planned）；既有`fsyncDirectory`/`RepoRequestTimeoutError`；`tests/unit-tests/distributed-repo-inapp-artifact-api.t.cpp`、`tests/integration-tests/spec192-persistent-artifact-api.t.cpp`、`tests/wscript`。public signature与wire不变。

**Flow**:

1. 在原有staging文件完成长度/摘要验证、file fsync/close后，保存旧destination的same-parent namespace；优先POSIX hardlink到唯一backup，不存在目的路径则记录absence。不能覆盖碰撞backup或跟随非预期目录。
2. 需要的backup目录屏障、deadline检查在修改destination前完成；link不支持/permission错误在覆盖前失败。
3. rename staging→destination、directory fsync，Persistent按既有契约在最终sync后检查全局deadline。全部满足才accepted commit。
4. accepted前任一普通错误：旧文件rename恢复或新destination unlink恢复absence；清理正常staging/backup，恢复目录同步，返回真实原错误。恢复工作可能超过原deadline，不能伪称POSIX syscall可中断。
5. rollback本身失败：保留可定位backup/恢复材料和explicit recovery诊断，作为fatal destination error停止Provider自动重试；不能再把本地无法恢复当作坏副本、重复覆盖恢复材料。
6. accepted后的backup cleanup是best-effort维护；不能把durable已接受的成功改成failed fetch。诊断不得泄漏key；cleanup residue须可定位、可重试，不伪称零残留。

**Ownership**: 局部helper只拥有本次staging/backup路径与状态；现有RAII FD/Api Impl lifetime不改变。异常按catch cleanup顺序释放；原始error与rollback-error分别可追溯。不引入后台GC、全局事务框架或public exception ABI，内部fatal分类必须在retry loop显式处理。

**Native oracle**: actual API，existing/absent × EIO/late-sync（Persistent）、existing/absent × EIO（In-App）；old inode/bytes/absence、verifiedBytes0、无正常residue；healthy follow-up通过。test-only `--wrap=fsync`实际命中post-rename directory，按parent dev/ino和destination inode隔离，跳过backup preparation与rollback sync；如果有两healthy副本，限制eligible第二LOCATION或明确终态fault，否则one-shot故障可能合法failover成功。另覆盖rollback failure恢复材料/stop-retry与accepted cleanup failure不反转成功。

不承诺断电恢复；native controlled fault证明return-time语义。需要并发同destination或symlink支持变更时，先明确owner/precondition，不暗加锁或改变公开路径行为。

## DB003 - Truthful Target Ledger on Source Failure

**Files / symbols**: `NDNSF-DistributedRepo/src/RepoNode.cpp::runPublishOperationPass` 的ownerSourceFailed分支、existing finalizer/aggregate mapping；现有生产integration worker fixture。不修改schema或wire。

pending且attemptCount0 → `NOT_ATTEMPTED`；pending且attemptCount>0 → `UNKNOWN`并保留`repo-target-outcome-unknown`、attemptCount、逻辑Repo/Provider binding，不伪造receipt。当前发现seed源错误的C保持`FAILED/repo-seed-source-invalid`。existing aggregate编码UNKNOWN为failed/unknown，不是SKIPPED；seed receipt与terminal immutable transaction不变。

closing regression使用真实三Provider/三个filesystem stores和恢复worker：B STORE请求明确观测，在B handler移除seed committed payload并bounded hold response；B超时后C materialization发现源失败。RAII release/drain B handler后再析构外部Face/scheduler；C零STORE，seed receipt1/B-C0，terminal重读及迟到响应不改变aggregate。不能用直接编辑SQLite模拟worker已经走过此路径。

## DB004 - Legacy Publication Durability Recovery

**Files / symbols**: `LegacyRepoImporter.cpp::publishNoReplace`与两条verified existing-destination return分支；native legacy integration/unit与CLI target；source/manifest schema保持。

新导入在no-replace rename+parent sync后才能durable acknowledgment。rename已成功而sync失败的目标可能完整可见，返回明确publish-durability failure，保留目标和source；不删除它制造“从未暴露”。重放验证source/config/idempotency/inventory/destination后，在返回成功前重新执行destination-parent fsync；含锁前与锁后分支均覆盖。屏障再次失败则继续失败；source不变、no-replace和manifest/provenance原语义保留。

observer在发布前抛错时的unexposed INCOMPLETE guarantee不扩大到所有发布后错误。使用真实生产importer的post-publish one-shot directory-sync fault、重复fault及success replay，断言callback/屏障命中、source DB/WAL/SHM/CAS摘要与目标inventory保持，fresh reopen正常；不把这些测试说成power-loss证明。

## DB005 - Metadata, Availability and Original Data

artifact完整八字段和LOCATION期望receipt/version必须严格echo。artifact`checkAvailability`处理artifact-v2-bytes；传统segmented缺段用传统manifest/FETCH与独立存储，offline另用reachability，保持history/current分离。

Original Data：native durable receipt至少绑定Data name/wire digest/Repo Node/Provider/commit ID；DATA_LOCATION当前认证应答绑定同name/digest/Provider及可路由非空hint，只有PRESENT才能发原名Interest。hint去重按认证Provider。返回完整原始wire不重签名/改名；metadata授权不扩展成原名Interest grant。producer encryption保护敏感原始内容；cache命中不证明指定Repo在这次真的回包。fixture与oracle分别声明该边界。

O17 default route、failover、corrupt-location、drop-original-data、import-legacy、cache-hit、server-metadata-auth在一个source family下逐项验收；现有runner互斥flag不得同次组合来伪造全覆盖。不为测试专用server断言不存在的生产部署行为。

## DB006 - Signed Offline Attempt Oracle

**Owner**: `tests/minindn/spec193_packet_oracle.py`及runner A21结果注册；不新增Repo RPC/observer。

用IP reassembly读取完整Interest5→ApplicationParameters36→embedded signedData6；验证ParametersSha256Digest、frozen root self/caller certificate及RSA KeyLocator/signature。MappingData205包含producer Name7、MappingEntry206/SeqNoEntry210（bootstrap212、seq214）和application Name7。以producer/bootstrap/seq去重，冲突mapping拒绝；caller Arizona和controller Memphis的mapping bytes/顺序一致。

all-offline phase的3 distinct STATUS REQUEST publications与native3 distinct C/B/A timeout attempts按序对照，A cooldown保留；Sync retransmission不增加logicalattempt。Provider归属是源码blocking顺序+nativeledger的推断，不能称payload直接编码targetProvider。覆盖首轮A失败→B成功、aliases拒绝、source digest、旧destination/receipt不变。旧抓包离线补充结果只补旧候选局部证据，新harness仍需当前资格。

## DB007 - Public Surface, Binding and DI

继承[旧API inventory](../../193-distributedrepo-multinode-api-campaign/api-inventory.md)，重新核对当前headers/pybind/registry并按validation-matrix登记；existing signature不因整理文档改变。local LIST与network INVENTORY/MANIFEST不同；Persistent artifact profile legacy write rejection与object-profile正向请求不同。

Python保留native user strong owner，销毁native API后再释放owner，阻塞调用释放GIL，错误result是独立值；capsule不是额外owner。DI resolver显式校验generic digest/size与handle，冷暖授权先于材料落地，Repo只保存显式opaque snapshot。不重建Python catalog、不计cache为replica。旧接口的版本化compat rejection和zero side effects继续验收。
