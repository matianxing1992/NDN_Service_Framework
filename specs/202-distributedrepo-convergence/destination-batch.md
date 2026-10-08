# B2 Destination Transaction Gate

**Members**: T002；single master owns API.cpp、test-onlysyscallfixture/main、inapp/persistentnative fixtures、Waf注册。T003–04不吸入本批。
**Batch growth decision**: 已有独立destination出口；冻结fixture和当前未修复API，先readonly red-gate review，再focused red，再localizedrepair/完整delta review/closingtests。没有修改生产Core/wire/timeout。
**Baseline**: T001150-inputsnapshot，Repo685e8efb+preservedoverlay。Red固定snapshot `evidence/t002-red-review-20261008/review-snapshot.zip` SHA`c2255521e54dd417946b2ebef01e050d47fb87066adbd3143bb12c05fd1241ae`。

## Dynamic Gate Card

**Risk/profile**: native namespace、FD和异常收尾；asan-ubsan，独立build输出，不抑制外部依赖ABI/leak报告。无新增线程owner，不要求新tsan gate。
**Native selectors**: `spec192-repo-inapp-api/DistributedRepoInAppArtifactRepositoryApi`；`spec192-repo-persistent-api/PersistentApiSelectsProvidersAndCountsOnlyDurableReceipts`（实际完整suite路径以list_content核对）。生产Repo archive linkwrap由testmain限定；普通目标不加wrap flag。
**Frozen parameter matrix**:

| Class | Destination / injected boundary | Native oracle |
| --- | --- | --- |
| nominal | 正常fetch，完整verifiedfixture | correct bytes/size、healthyfollowup、no residue |
| R01 | Persistent existing/absent × postrename directoryfsync EIO | actualfault hit、zeroverified、oldinode/bytes或absence、无正常residue |
| R02 | Persistent existing/absent × postrename sync1200ms，overall1000ms | post-sync deadline仍检查、结构化失败、return-time恢复 |
| R03 | In-App existing/absent × postrename EIO | publicnativeexception、oldinode/bytes或absence、healthyfollowup |
| R04 | olddestination × rollbackrename或rollbacksync EIO | fatalrecovery、可定位旧内容、Persistent一attempt停止retry |
| R05 | olddestination × acceptedbackup unlink EIO | 已接受成功不反转，正确newbytes，oldbackup保留 |

**Budget**: focused native inapp≤120s；persistent fixture≤600s（existing完整真实runtimefixture）；closing各2次用于确定性/寿命，动态各1次，首失败保存后先改gate再重跑。不是MiniNDN两轮fault。
**Evidence**: `evidence/t002-red-review-20261008`保存redsource/build/run；closing及dynamic独立目录/实际output/SHA/candidate，不混旧binary结果。
**Status**: STATIC_RED_RELEASE_PASS，readonlyreview五lane允许一次focusedbuild/red；closing、dynamic NOT_RUN。产品BLOCK。复审release与每个首次failure/Changedgate在此追加，不把help/linksmoke作为behaviorPASS。

## Five-Lane Review Scope

Entry/caller为两个publicfetch与既有真实nativecallbacks；implementation为parent fsync/install/rollback、retryfatal classification；test/oracle为dev/ino+leafstate过滤、真实postrename命中、preparedbackup跳过、完整reference-derived callbacks、FacePump寿命；build为staticRepo消费者/Wafwrapflags和实际输出；migration为原leafsymlink/目录拒绝、namespace独占和既有storedreceipt保持。readonlyprofile/SHA沿review.md。

## Red and Closing Review Checkpoint

Red build PASS，9m50.195s（包含受影响Core/Repo重编；不作提效比较）。In-App selector exit201，12项预期失败，包括absent未恢复、old inode/bytes被替换、recovery/cleanup无证据。Persistent red已命中postrename不恢复、fatal后继续第二副本及虚假成功，终态另记录；其oldexecutable/sharedlibrary hash在built-red-artifacts.json，fixedsource ZIP不受后续source修复影响。

Closing初审P2：pre-install backup unlink failure被忽略。**Changed gate**：保留originalfailure并显式报告destination/backup/cleanup error，internalfatal停止retry；增加PreparationCleanup两hookhits真实回归。额外leafsymlink/no-follow和retainedmaterialpath断言覆盖声明前置。v2snapshot SHA`db5f9eaa3c540f28a955c6f8d40cd2a102baf6ac1d3eb06602008ab5f5637cf2` readonly scoped recheck PASS，允许closing build/tests。没有扩展publicABI/wire或deadline。

Persistent red终态exit201，19预期失败，512.039827s；不是timeout124。首次closingbuild exit1：newhelper在定义前引用internal RepoRequestTimeoutError。**Changed gate**：project-symbol scope/order明确timeoutclass429→recoveryclass435→helper442→classifier675→callers3345/3712；移动同一internal定义并删除旧定义，不改语义。v3 SHA`8d7258e25e01a64054630107891c4817af23aecf702a7c595ae32cbab5e6ec52` readonly delta recheck PASS，重试log独立build-closing-v3.log。保留初始compiler错误，不用仅重跑关闭漏检。

## 2026-10-08 14:15 Closing / Dynamic Checkpoint

Normal v3 build PASS（1m5.315s）；完整In-App closing-1/2各exit0，完整Persistent closing-1 exit0，closing-2运行中。各freshlog在closing evidence目录；不是新的MiniNDN资格证据。T002仍PARTIAL直到完整第二轮和动态门闭合。

隔离ASAN/UBSAN configure首次被全局SDK stale receipt拒绝：安装SVS摘要5d024f1e…，receipt期待90d1dacc…；本任务runtime pinned source/build overlay仍d87ae04f…。既有installer的exact兼容probe确认五依赖均可重用，随后deps-only/no-system-packages canonical installer跳过全部依赖，仅刷新收据；没有更改SVS source或library。相同configure重试PASS，单独build-spec202-asan树，-j4构建中；原build/和Waf lock保持独立。

动态编译配置：WAFLOCK=.lock-waf_spec202_asan_build，CXXFLAGS=-O1 -g0，--with-sanitizer=address,undefined，--with-tests --with-examples，显式pinned ndn-svs source/build；cache确认Core/Repo消费者asan/ubsan+framepointer以及pinnedSVS include/libpath/rpath。外部已安装依赖未instrument，不抑制其报告；运行前记录实际binary/source/dependency身份，不用configure PASS声称动态PASS。SDK初始probe曾误用NAC minimum0.1.0（实际pkg0.1），保留日志并按installer exact0.1重试PASS。

## Repeated Gate Failure / Changed Gate

Persistent closing-2 exit201：quorumreceipt achieved1/required2与预期成功不符，随后fallbackfetch抛错时joinable lateLocationThread析构触发SIGABRT。正常API可返回合法PARTIAL，不把上述不足当quorum成功。原因仍待独立复现；并发ASAN build时memory8.5/9.9GiB、swap7.9/8GiB是观察到的负载，不是已证明根因。

Changed gate：在late thread声明后复用已有ScopedThreadJoin，使任何异常先join后销毁thread/environment；增加两个receiptJSON诊断，核对actualterminal/targetfailure而非仅计数。definition-map发现并删除草稿重复helper，实际仅一处定义。只读live delta against49e05dba五lane/lifecycle PASS。最初lifecycle build在release前误启动，主动SIGINT(exit68)，保留build-lifecycle.log；复审后build-lifecycle-reviewed.log exit0。Persistent-independent-1现在单独运行，无并发build。

ASAN-j4因memory/swap压力主动SIGINT exit68，保留build-asan.log；未视作代码或sanitizer PASS。恢复使用-j1并避免同时CPU-intensive integration。最终R01–R07仍须在完成native修复后的相同instrumented source family验证，未关闭动态门。

Independent closing-1/2完整Persistent selector均exit0（lifecycle-reviewed binary，无并发build），两个quorumreceipt均COMMITTED/achieved2/required2；后续destination/late-response断言均通过。此前closing-2 exit201仍保留，观察到负载相关性不证明其具体根因。normal-closing-artifacts.json保存B3/B4 relink前当前In-App/Persistent/Core/Repo/SVS摘要；此为boundednormal证据，不是最终family。T002 normal closing门已闭合，仍PARTIAL待最终instrumented ASAN/UBSAN gate。

**Serial dynamic profile update**: 先前O1/g0+j4 configure/build日志保留为interrupted attempt；最终instrumented profile改为-O0 -g1、单-j1，保持address+undefined/framepointer/pinnedSVS/source与全部回归，不抑制报告。降低optimization以限制巨大Core TU构建资源，lineinfo用于诊断；不是性能比较。独立normal/native gates全部结束后才开始serialbuild；动态运行不与build并发。Persistent动态预算900s、worker900s、importunit180s/integration180s、In-App120s，各1次，必要失败先fix/review再freshlog重跑。
