# Tasks: DistributedRepo Contract and Implementation Convergence

**Input**: [spec.md](spec.md), [plan.md](plan.md), [gap-audit.md](gap-audit.md), [validation-matrix.md](validation-matrix.md)
**Branch**: `Repo`
**Execution mode**: IMPLEMENTATION_AUTHORIZED；用户已要求在Repo完成全部任务。旧Spec192/193保持停止；本registry记录当前实施与分层验证。

<!-- SPEC202 PROGRESS START -->
## Execution Progress

**Updated**: 2026-10-09

| Task | Status | Depends on | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| T001 | DONE | user scope | api-inventory逐header/overload→TU/target/output/case；owner overlay分类、DB002–004前置及只读release PASS。150-input完整snapshot/manifest保存；实际rebuild/runtime资格明确归T009。 | 2026-10-08 13:12 -05:00 |
| T002 | DONE | T001 | source d75cf28e；R01–R05真实destination/postrename/deadline/rollbackfatal/acceptedcleanup回归，完整In-App28/491、Persistent API6/935 normal/dynamic及独立复现PASS；closing-review零controlling gap，R01–05关闭；network由T010验收。 | 2026-10-09 |
| T003 | DONE | T001 | source d75cf28e；R06 early58/58与late302/302均完整normal/dynamic/独立复现PASS；UNKNOWN/count1、NOT_ATTEMPTED/count0/zeroSTORE、seedreceipt/aggregate/reread保留。closing-review允许关闭；首个v2空ACK red原因未确证且保留，不宣称可靠性保证。 | 2026-10-09 |
| T004 | DONE | T001 | source d75cf28e；R07两replay分支parentfsync屏障/故障/重复故障/成功重放、source/inventory/no-replace真实native CLI覆盖。完整unit4/49/integration6/145 normal/dynamic及独立复现PASS；closing-review允许关闭，P05网络留T010。 | 2026-10-09 |
| T005 | PARTIAL | T001,T002,T003 | DB005 STATIC_PASS；当前SDK11 OriginalData native6/79 normal PASS，untraced existingCore lifecycle/签名wire oracle v4及13offline反例保留。当前7个O17网络子场景仍待T009/T010，native PASS不替代network。 | 2026-10-09 |
| T006 | PARTIAL | T001 | oracle接线/反例v2+delta复审PASS；12offline tests PASS，旧两capture reanalysis单列；source登记/当前network仍T009/T010，不能继承旧PASS。 | 2026-10-08 14:27 -05:00 |
| T007 | IN_PROGRESS | T001,T003 | 当前SDK11完整surface/local conformance98case normal1655/dynamic v3 1657断言PASS，DB007 STATIC_PASS，公开inventory保留。见T009；独立复现与network/profile资格待关闭，旧SDK9结果仅历史。 | 2026-10-09 |
| T008 | IN_PROGRESS | T001,T002,T004,T005 | 当前SDK11两binding fresh重建/import、33-file Python117PASS及全部7 native dynamic v3 PASS。见T009；不宣称Python instrumentation，sealed复现与crosslanguage/DI network待资格。 | 2026-10-09 |
| T009 | PARTIAL | T002–T008 | source d75cf28e/688；[SDK11](evidence/candidate-build-20261009-sdk11/)，receipt ce2207fd；normal9/dynamic7 PASS，sealed v5完整9组PASS(native183、Python117、admission41/798s)，原库恢复与INTEGRITY_PASS，10support/hash/mode封存1102项/81094cea。closing-review零controlling gap、可发布网络candidate；旧ACK/support/ENOSPC red保留。T002–004/R01–07关闭；T005–008网络资格及完整136-run/matrix audit待完成。 | 2026-10-09 |
| T010 | IN_PROGRESS | T009 | 冻结d75cf28e/688与声明six profiles；两轮68场景/59ID共136次执行准备完成，release绑定normal/dynamic/sealed/restore/integrity/closing及harness hashes；新run逐actor maps/pcap/finite exits/cleanup/storage/consumer/wire/cross-run验收，未出完整PASS前保持OPEN。 | 2026-10-09 |

## Current Checkpoint

当前候选状态、阻塞与下一步只维护于上表T009；证据与完整资格要求分别由链接及validation-matrix维护。
<!-- SPEC202 PROGRESS END -->

## Design binding

任务细节由contracts的DB001–007及candidate合同唯一维护。每次编码前核对当前source/实际signature/重要owner字段及failure收尾；下表列精确受影响owner，普通localdetail由实现者决定。

## Phase 1: Setup and Baseline

- [x] T001 固定实际baseline和ideal边界：api-inventory.md逐header/overload→TU/target/output→case，按owner登记dirty overlay和非源码artifact，明确DB002–004路径/worker/import前置；150-input完整snapshot保留实际source。**Design binding**: DB001 STATIC_BASELINE_CLOSED，focused repair release PASS；final rebuilt source/runtime closure留T009，不以旧binary或checkbox替代qualification。

## Phase 2: User Story 2 - Safe Consumer Commit

- [x] T002 [US2] 修复`ArtifactRepositoryApi.cpp`两种fetch的destination commit/rollback；同一任务增加真实nativepostrename EIO/late-sync、旧文件/absence、rollbackfailure-stopretry和acceptedcleanupfailure回归，注册`tests/wscript`focused selectors并保留首次red/closing结果。**Design binding**: DB002 READY_FOR_IMPLEMENTATION afterT001路径前置核对；public签名不变，helper内部ownedpaths/FD/accept-state；默认asan-ubsan，不放宽post-syncdeadline。Closing oracle为真实旧inode/bytes/absence及zeroverified/恢复材料，不仅错误文本。

## Phase 3: User Story 1 - Honest Publication

- [x] T003 [US1] 修复`RepoNode.cpp::runPublishOperationPass` sourcefailure对pendingtarget的attempt区分；三Provider fixture覆盖B请求→timeout→C源读取失败及reopen后早期seed missing/corrupt，验证UNKNOWN/count1、未尝试目标/zeroSTORE与terminalaggregate/seedreceipt不变；focused回归和证据同任务。**Design binding**: DB003 READY_FOR_IMPLEMENTATION afterT001；无schema/wire变化，重要字段state/attemptCount/failureCode/receipt保留nativeowner；asan-ubsan，新增并发ownership才tsan。

## Phase 4: User Story 4 - Recover Durable Import

- [x] T004 [US4] 在`LegacyRepoImporter.cpp`两条verifieddestinationreplay分支补parentdurability屏障，保持no-replace/source/config/inventory验证；用真实nativeimporterpostpublishsync失败、再次失败和successreplay回归明确ambiguouspublication，保留完整target和source。**Design binding**: DB004 READY_FOR_IMPLEMENTATION afterT001；影响nativelegacyintegration/unit与productionCLI sourceclosure，无新增schema/ABI；closing为实际屏障hit/sourcehash/idempotency，不宣称powerloss。

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
