# B3 Source Failure Ledger

**Members/design**: T003 / DB003；Node.cpp runPublishOperationPass与已有finalizer、实际Persistent integration worker。无wire/schema/线程owner变更；不吸收T002 destination逻辑。

**Frozen regression**: 在RecoveredPendingPublishTargetGetsOneDurableReceipt现有三Provider fixture末尾，普通seed注册接受真实beginPublishFile并保存seedreceipt；释放/重新打开seed以启动恢复worker。B已观测STORE后删除真实seed committedpayload，hold response至8s worker超时之后；C本地materialization失败且STORE零请求。B UNKNOWN/attempt1/unknowncode；C FAILED/sourceinvalid；seedreceipt1且identity/bytes不变，B/C receipt0。独立SQLite aggregate中B FAILED/unknown，不是SKIPPED。迟到response release/drain后native status和immutableaggregate不变。不直接编辑SQLite模拟此新failure路径。

**Ownership**: B handler只捕获shared hold/counters和value path，无外部Face/Node/User引用；ReleaseAndDrain在node/environment之前释放并等待bounded handlerdone。所有生产worker均用原环境Facepump，stop/release沿fixture既有约束。C的observationalcounter handler若调用返回明确fail，不能伪造未发请求PASS。

**Minimal repair**: 既有finalizer已将pending attempt0→NOT_ATTEMPTED、attempt>0→UNKNOWN并保留unknowncode。ownerSourceFailed提前无条件NOT_ATTEMPTED覆盖使finalizer无法识别pending；删除该冗余预改写，交给现有finalizer，不新增第二状态机。

**Gate card**: native selector DistributedRepoPersistentArtifactApi/RecoveredPendingPublishTargetGetsOneDurableReceipt；normal预算600s，red1、closing2；asan-ubsan1最终source family，预算900s。没有新生产线程owner，不要求新增tsan。独立freshlog，首次失败/changedgate保留；build与integration不同时运行。静态review必须核对bootstrap/service/defaultworker/实际STORE、source读取、finalizer/codec、join/drain与Waf target。

**Status**: red fixture IMPLEMENTED，未修复Node snapshot待冻结review；没有red/build/behaviorPASS。当前T002 independentclosing运行，不启动新build。

**Initial static BLOCK / Changed gate**: 首审P2指出new recovered sourceLossSeed的service handlers保留sharedself，早期异常可能将workerstop延至Provider teardown/bridge断开之后。ReleaseAndDrain增加actualhandler detach与runWithFacePump reset callback，在释放B之后、environment/publisher销毁前执行。增加actualcaller SVS publication observer：B producer由环境indexedName(providerNode,p1)定位，按provider/requester/service/actualrequestId匹配late Response publication，观察到该publication后才比较terminal/aggregate；不把held.done当delivery。新v2复审前不运行red。

**Red checkpoint**: v2缺新flock wrapper定义输入，补immutable main/header后buildlane PASS；v3纳入完整输入。red1 exit201是旧fixturecontrol失败：teardown漏两metadata handlers使旧storageowner仍存活。Changed gate：实际生产register20个service与teardown20个逐名集合比较missing[]extra[]；补DATA_RECEIPT/DATA_LOCATION并只读复审PASS。red2 exit201，4项预期G002失败：B NOT_ATTEMPTED/emptycode、aggregate SKIPPED/emptyreason；B actualSTORE/C sourcefailure和actuallatepublication路径已到达。现删除ownerSourceFailed冗余预改写，让已有finalizer按attemptCount分类，固定source closing review待运行。

**Normal closing**: fixed snapshot897db301…五lanePASS；重新build后完整RecoveredPendingPublishTargetGetsOneDurableReceipt closing1/2各exit0。新sourcefailure及既有恢复/拒绝/reconciliation全部到达，actualB responsepublication后terminal/aggregate仍immutable。T003保持PARTIAL直到同源asan-ubsan和T009 source/runtime closure；normal gate不替代network两轮。
