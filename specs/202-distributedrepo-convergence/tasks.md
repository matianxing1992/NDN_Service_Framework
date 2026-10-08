# Tasks: DistributedRepo Contract and Implementation Convergence

**Input**: [spec.md](spec.md), [plan.md](plan.md), [gap-audit.md](gap-audit.md), [validation-matrix.md](validation-matrix.md)
**Branch**: `Repo`
**Progress Timestamp**: 2026-10-08 12:24 -05:00
**Execution mode**: IMPLEMENTATION_AUTHORIZED；用户已要求在Repo完成全部任务。旧Spec192/193保持停止；本registry记录当前实施与分层验证。

## Logical Batches

| Batch | Outcome | Tasks | Exit |
| --- | --- | --- | --- |
| B1 | 当前source/ideal/API baseline | T001 | 完整entry/source/target/traceability，未知范围明确 |
| B2 | failed fetch destination安全 | T002 | 两nativeAPI真实故障回归及静态门 |
| B3 | source loss后真实target ledger | T003 | 真实worker顺序和immutable aggregate |
| B4 | legacy publish恢复耐久性 | T004 | 两replay分支barrier及source/no-replace |
| B5 | metadata/object/oracle/binding全部适用边界 | T005–T008 | 各行为独立closing证据，稳定后停止增长 |
| B6 | currentcandidate完整convergence/source/suites | T009 | freshaudit+所有affected/dynamic/closure通过 |
| B7 | 完整多节点two-roundqualification | T010 | matrix全项+子场景+reproduction PASS |

## Execution Progress

| Task | Status | Depends on | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| T001 | DONE | user scope | api-inventory逐header/overload→TU/target/output/case；owner overlay分类、DB002–004前置及只读release PASS。150-input完整snapshot/manifest保存；实际rebuild/runtime资格明确归T009。 | 2026-10-08 13:12 -05:00 |
| T002 | PARTIAL | T001 | v3 readonlyPASS/buildPASS；In-App完整closing两次exit0，Persistent第一次exit0、第二次运行中；独立asan-ubsan构建中，dynamic尚未闭合。见destination-batch.md。 | 2026-10-08 14:15 -05:00 |
| T003 | NOT_STARTED | T001 | NOT_RUN：仅DB003设计；G002worker分支尚未修复。 | 2026-10-08 12:24 -05:00 |
| T004 | NOT_STARTED | T001 | NOT_RUN：仅DB004设计；G003postpublish/replay sync未修复。 | 2026-10-08 12:24 -05:00 |
| T005 | NOT_STARTED | T001,T002,T003 | NOT_RUN：DB005，A19文档跨plane纠正和O17当前完整收口待实施。 | 2026-10-08 12:24 -05:00 |
| T006 | PARTIAL | T001 | oracle/phase offsets/finalverification及offline反例已实现；初审3P2已修复，v2完整快照只读复审中，尚未运行offline/currentnetwork。 | 2026-10-08 14:15 -05:00 |
| T007 | NOT_STARTED | T001,T003 | NOT_RUN：DB007公开object/local/network/parser边界收口；不凭旧case PASS省略入口。 | 2026-10-08 12:24 -05:00 |
| T008 | NOT_STARTED | T001,T002,T004,T005 | NOT_RUN：DB007Python/DI/parity/import/retired完整currentsource验证。 | 2026-10-08 12:24 -05:00 |
| T009 | NOT_STARTED | T002–T008 | NOT_RUN：overlay/source/binary/profile冻结、全部affected suites/dynamic与freshconvergence。 | 2026-10-08 12:24 -05:00 |
| T010 | NOT_STARTED | T009 | NOT_RUN：59case全部子场景/新回归，同source-family两轮完整资格与cross-run/reproduction。 | 2026-10-08 12:24 -05:00 |

## Detailed Execution Progress

| Substep | Parent | Concrete outcome / path | State | Scoped evidence | Blocker / next action | Reuse / rerun trigger |
| --- | --- | --- | --- | --- | --- | --- |
| T001.a | T001 | gap-audit.md当前source findings | VERIFIED_STATIC | CodeGraph/actualsource、Core admission overlay、只读release PASS | 产品缺陷由T002–04修复 | source或acceptedcontract变更重审 |
| T001.b | T001 | api inventory、TU/target/source baseline | VERIFIED_STATIC | api-inventory.md；150-input complete ZIP SHA b756347c…；原116-files archive明确PARTIAL | finalrebuiltartifact/loadeddeps由T009验证，不计旧binary PASS | 新入口/构建source变化 |
| T002.a | T002 | API.cpp两fetch的commit/rollback | IMPLEMENTED | 实际red+fixedsnapshot v2静态PASS；postrename rollback、fatalcleanup、acceptedcleanup和leafsymlink | closing native与asan-ubsan，不将静态PASS算DONE | destination/deadline/cleanup语义变化 |
| T003.a | T003 | Node.cppsourcefailure target状态 | NOT_STARTED | NOT_RUN；DB003 | 真实Btimeout/Csourcefailureregression | ledger/worker/recovery变化 |
| T004.a | T004 | Importer两existing目标分支durability | NOT_STARTED | NOT_RUN；DB004 | actualpostrenameparent-sync故障/重放 | importer/manifest/sourceprofile变化 |
| T005.a | T005 | artifact/segments/offline与OriginalData平面 | NOT_STARTED | NOT_RUN；DB005 | 源码符合 accepted contract再验证 | metadata/auth/hint/oracle变化 |
| T006.a | T006 | signedSync+native attempt对照 | IMPLEMENTED | v2 snapshot 60be34cc…；真实session producer、distinctRequest、expectedC/B/A、签名/链/跨capture反例 | readonly复审后offline验证；当前network仍T009/T010 | oracle、SVS协议/调用顺序变化 |
| T007.a | T007 | publicobject/local/network/codec覆盖 | NOT_STARTED | NOT_RUN；DB007 | 入口/负例/registration当前核对 | codec/API/profile变化 |
| T008.a | T008 | binding/DI/compat/import实际caller | NOT_STARTED | NOT_RUN；DB007 | currentPython模块/sourcehash与真实生产结果 | native静态库/pybind/DI变化 |
| T009.a | T009 | source/runtime/profile sealed family | NOT_STARTED | NOT_RUN；candidate合同 | 合法owner source收口及全部affectedconsumers | 任一behaviorplane变化 |
| T009.b | T009 | suite/dynamic+freshaudit | NOT_STARTED | NOT_RUN | 先静态组合门，再对应测试/动态，不忽略外部ABI报告 | 对应changeinvalidation矩阵 |
| T010.a | T010 | finalfullmatrix/cross-run双轮 | NOT_STARTED | NOT_RUN | T009 PASS前禁止执行资格 | family/profile/validation变化 |

## Current Checkpoint

2026-10-08 14:15 -05:00 — Repo分支，用户已授权全部Spec202实施。T001 DONE；T002 localizedrepair通过静态/build和当前normalclosing第一轮，In-App第二轮通过，Persistent第二轮及独立asan-ubsan构建进行中。T006 oracle/接线/反例已实现，初审控制问题修复后等待v2复审。G002/G003尚未修复；产品readiness和finalqualification仍BLOCK。旧A06_LARGE仅历史diagnostic；未运行新的资格round，不继承旧PASS。Context Mode健康检查已通过，但authority检索为空，继续使用仓库文档/source作为authority。

## Batch Quality Record

**Batch growth decision**: 一批只写Spec202可审查文档与旧Spec停止/指针同步，不修改产品、harness或测试源码。
**Scope**: 当前baseline/ideal/gaps/contracts/task/validation/supersession；read-onlycode review沿用canonicalreview-agent。
**Coverage matrix**: gap-audit.md记录五laneactualfiles/queries及未观测范围；review.md记录本轮doc一致性复审。
**Changed gate**: 以实际source派生begin预算、post-rename失败边界、attemptCount和signedSyncmetadata替代旧不正确推断。
**Static**: 当前产品BLOCK；文档review/structure待review.md结果。
**Compile/link, runtime/test, dynamic**: NOT_RUN（本轮未授权执行）。
**Batch Retrospective**: static发现三产品缺陷/跨plane文档和oracle误判；compile/link未观测；runtime/test只读取旧manifest；unobserved为fullcurrentqualification/powerloss。
**Closing decision**: 文档稳定后CLOSED_FOR_PLANNING；产品实现OPEN_FOR_NEXT_BATCH，需明确实施授权和各Design binding前置，无自动开始编码。

## Design binding

任务细节由contracts的DB001–007及candidate合同唯一维护。每次编码前核对当前source/实际signature/重要owner字段及failure收尾；下表列精确受影响owner，普通localdetail由实现者决定。

## Phase 1: Setup and Baseline

- [x] T001 固定实际baseline和ideal边界：api-inventory.md逐header/overload→TU/target/output→case，按owner登记dirty overlay和非源码artifact，明确DB002–004路径/worker/import前置；150-input完整snapshot保留实际source。**Design binding**: DB001 STATIC_BASELINE_CLOSED，focused repair release PASS；final rebuilt source/runtime closure留T009，不以旧binary或checkbox替代qualification。

## Phase 2: User Story 2 - Safe Consumer Commit

- [ ] T002 [US2] 修复`ArtifactRepositoryApi.cpp`两种fetch的destination commit/rollback；同一任务增加真实nativepostrename EIO/late-sync、旧文件/absence、rollbackfailure-stopretry和acceptedcleanupfailure回归，注册`tests/wscript`focused selectors并保留首次red/closing结果。**Design binding**: DB002 READY_FOR_IMPLEMENTATION afterT001路径前置核对；public签名不变，helper内部ownedpaths/FD/accept-state；默认asan-ubsan，不放宽post-syncdeadline。Closing oracle为真实旧inode/bytes/absence及zeroverified/恢复材料，不仅错误文本。

## Phase 3: User Story 1 - Honest Publication

- [ ] T003 [US1] 修复`RepoNode.cpp::runPublishOperationPass` sourcefailure对pendingtarget的attempt区分，使用既有三Provider workerintegrationfixture真实B请求→timeout→C源读取失败验证UNKNOWN/count1与terminalaggregate/seedreceipt不变；focused回归和证据同任务。**Design binding**: DB003 READY_FOR_IMPLEMENTATION afterT001；无schema/wire变化，重要字段state/attemptCount/failureCode/receipt保留nativeowner；asan-ubsan，新增并发ownership才tsan。

## Phase 4: User Story 4 - Recover Durable Import

- [ ] T004 [US4] 在`LegacyRepoImporter.cpp`两条verifieddestinationreplay分支补parentdurability屏障，保持no-replace/source/config/inventory验证；用真实nativeimporterpostpublishsync失败、再次失败和successreplay回归明确ambiguouspublication，保留完整target和source。**Design binding**: DB004 READY_FOR_IMPLEMENTATION afterT001；影响nativelegacyintegration/unit与productionCLI sourceclosure，无新增schema/ABI；closing为实际屏障hit/sourcehash/idempotency，不宣称powerloss。

## Phase 5: User Story 3 - Correct Metadata and Original Data

- [ ] T005 [US3] 在`RepoClient`/`RepoNode`/nativeAPI真实caller核对full-reference receipt/location/availability与OriginalData路径，按DB005消除A19跨plane要求；保留O07真实缺段、A20offline及O17全部metadata/hint/wire/fault/cache/import/auth子场景，修复实际违约owner并保留正确rawData加密边界。**Design binding**: DB005；status CONDITIONAL untilT001逐入口核对，existingpublicsignatures不变；无实际缺陷不重写owner。验收不把metadata grant当rawInterestgrant或hint当crypto来源证明。

## Phase 6: User Story 6 - Observe Actual Offline Attempts

- [ ] T006 [US6] 在`spec193_packet_oracle.py`实现签名SyncInterest mapping/IPreassembly oracle并注册runnerA21结果，caller/controller一致、publication去重/冲突拒绝、nativeordinal推断分类；旧pcap分析与新candidate运行分别记录。**Design binding**: DB006 READY_FOR_IMPLEMENTATION afterT001；test-onlyofflinePython解析，无Repo observer/API；dynamicnone理由化，签名/parametersdigest实际negativecases不能省。

## Phase 7: User Story 4 - Preserve the Public Object Surface

- [ ] T007 [US4] 对`RepoClient.hpp/.cpp`、`RepoProtocol.cpp`、nativeobject/localhandlers和`tests/minindn/run_spec192_real_cpp_repo.py`完成公开入口/模式/codec负例收口；保留O01–16/L01–04/N01–10及相关artifactwrapper，真实local/network/profile分明，parser兼容先定契约后修实际违约，不泛化重写。**Design binding**: DB007+DB001；status CONDITIONAL untilactualinventory/boundary frozen；codec实质变化需要boundedparser-fuzz，native断言为oracle。

## Phase 8: User Story 5 - Native Python and DI Reuse

- [ ] T008 [US5] 对Repo pybind/facade、`pythonWrapper/src/ndnsf/_ndnsf.cpp`和DIresolver真实调用完成P01–06/crosslanguage/冷暖权限/nativeowner寿命/legacyimport/退休APIzero-sideeffect收口；若native静态库改变重建所有受影响Pythonconsumer。**Design binding**: DB007；status CONDITIONAL untilT001/T002/T004/T005 owner边界冻结；不增加Python状态机，不用Python fixture模拟C++行为或networkparity。

## Phase 9: User Story 6 - Seal and Audit the Candidate

- [ ] T009 [US6] 对实际sourceoverlay、Waf所有affectedtargets/output、nativePython/依赖库、effectiveprofiles、harness/validation合同建立可复现sealedcandidate和mutationclosure；明确承接既有In-App reopen、配置拒绝、capability discovery、seed/idempotency/restart、archive隔离等生命周期/API conformance，不将其归入仅修worker的T003。五lane组合静态后完成全部affected C++unit/生产integration/Pythonconformance及适用dynamiccard，freshdesign-codeconvergence要求零controllinggap。**Design binding**: candidate-and-validation合同；code/source+build/runtime+config各plane有owner；T002–08稳定前不启动formal整套验证，保持首失败/Changedgate及未抑制sanitizer分类。

- [ ] T010 [US6] 用T009冻结的共同source+声明profilecomposition，完整执行`validation-matrix.md`59case及网络子场景两轮未跟踪五/六/七hostMiniNDN；R01–R07在最终candidate的native/dynamic门独立关闭，不要求移植syscall fault到network harness。保留cross-run seed/read新进程持久性、每节点抓包、有限child、actualloadedlibraryhash、storage/consumer/wire与boundedcleanup，完成reproduction/公开docs并仅在全项满足后关闭Spec202。**Design binding**: candidate-and-validation合同+matrix；重复执行是wholequalification出口而非任意补跑，任何OPEN/不匹配禁止DONE。

## Dependencies & Execution Order

T001→T002/T003/T004/T006；T002/T003→T005；T003→T007；T002/T004/T005→T008；T002–08→T009→T010。独立owner修复理论可并行read-only审查，但实施采用单master；nativeAPI和integration同文件冲突不并行写。任何新增实际缺陷进入正确owner和closingregression，不拆成write/run/log行政task。

## Implementation Strategy

先以T002/T003修复两个HIGH作为最小安全出口，再T004恢复耐久性；既有正确能力不从零重做。source、文档、harness、配置变化只按失效矩阵复核受影响门；最终完整资格必须统一candidate。停止旧Spec意味着执行与状态更新归属202，不需要将带192/193的共享源码/test文件机械改名。
