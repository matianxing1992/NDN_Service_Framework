# B4 Legacy Publication Barrier

**Members**: T004 / DB004；production importer及真实artifact legacy fixture，test-only syscall/lock pause。无生产hook、schema/sourceprofile/no-replace变更。

**Regression / exact branches**: 实际首次import在renameat2(RENAME_NOREPLACE)后target已可见时，按parent dev/ino、publishedDirectory筛选fsync EIO；应报告directory barrier failure，完整COMMITTED目标保留、source DB/WAL/SHM/CAS摘要不变。相同verifiedtarget重放再次EIO必须继续失败，健康重放SyncDelay1ms hook真实命中一次后成功。没有onItemCommitted第二次callback，manifest/inventory不变，重新打开native In-App fetch正确bytes。

**Locked replay**: test-only --wrap=flock在第一个实际import的absent检查之后、第一次kernel flock之前暂停，第二个实际import同config/source正常取得锁并发布。随后arming parent-sync EIO并释放第一个caller；实际lockedDestinationExists分支验证target后必须命中同屏障并失败。不是手写manifest或模拟importer状态。fd/proc pathname只选择exact test lock，pause sharedstate锁与bounded30s，RAII release，无生产faultenv。每个callback只increment atomic，两个source snapshot均来自真实importer。

**Minimal repair**: 两条verifyExistingDestination + verifySourceUnchanged之后、return之前各增加fsyncDirectory(destination.parent_path())，保持publishNoReplace的现有新发布屏障；不删除完整target，不把所有postpublish异常称为unexposed INCOMPLETE。

**Gate card**: Spec192LegacyRepoImportUnit/retriesPublishedDestinationParentBarrierBeforeAcknowledgment，red1、closing完整unit2、现有import integration1、asan-ubsan完整unit1；unit预算120s、integration180s、dynamic180s。Normal/ASAN build不与integration并发，source family最终统一于T009。testmain仅测试目标注册wrap；production importer无wrap。

**Status**: redfixture IMPLEMENTED、未修复Importer；静态review待冻结，未运行新build/red。T002 independentclosing仍运行。

**Runtime checkpoints**: firstred exit201，5预期断言精确定位两replay屏障缺失；fixed两parent fsync staticPASS，完整unit closing1/2 exit0。首次完整integration exit201，3failures：existingRepo profile还原payload后nativeManifest.packetNames使FilesystemBackend::isMetadataOnly拒绝getRange，连onItemCommitted都未到达；不能把unitPASS关闭全scope。

**Changed gate / materialization design**: importedobject的完整verifiedbytes用native packetNames空、segmentCount1，ndn-segmented-data标签变为已有object类型；sourceManifest hash、原packetNames/packetSHA留importitem，原signedData另存ndn-data-wire不改wire。增加actualsegmented-type sourceprofile/nativebyte+wire/provenance/replay回归，既有interruption/resume全integration仍须通过。此字段转换改变destinationinventory；旧错误候选INCOMPLETE保持hash拒绝，不自动删除/重写。新closing静态review前不重建。

**Migration static BLOCK / Changed gate**: 审查发现validateManifestIdentity未比较destinationInventoryDigest/itemCount/byteCount，旧stage会在laterfailure catch里被新manifest重写。现将三字段加入resume前身份比较（outside staging mutation try/catch），保持原canonicalexisting-state-conflict。新增persistedinput probe从真实当前importer interruption生成stage，再将完整bytes对象的native字段/声明inventory还原为旧metadata-only布局；这是显式构造的历史layout负输入，不冒称运行旧binary。assert旧stage所有regularfile SHA和sourcebundle都不变、target不存在；新增packetSha256.front实际wireSHA断言。最终closing静态待复审。

**Final normal closing**: migration snapshotdc2c1781…五lanePASS。build-closing-materialization-migration.log exit0；完整unit final-closing1/2各exit0、完整integration5case exit0，包括已实现的真实barriers/concurrentlockedreplay、转换后的native bytes/原signedwire、currentinterruption/resume及oldlayout拒绝。Production importer CLI已重建；真实CLI/profile/network结果和最终ASAN门仍pending，T004不因此DONE。
