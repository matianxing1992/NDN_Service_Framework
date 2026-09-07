# T009-B Shared Execution Lease State — 验收执行证据（2026-09-07）

Status: DONE。Selector：CPP(Spec182SharedLease/*)（3 cases，新增 suite
`Spec182SharedLease`，宿主于 T009-A 的 di-native-provider-host.t.cpp）。
实现：ExecutionLeaseService 增加 host 级共享 lease state
（SharedExecutionLeaseState = Core table + prepare mutex），原四参数
constructor 保留并委托自有 state；非 Prepare 操作先做 lease 行的
target 绑定检查，仍由 Core 完成身份/状态/重放验证。全部通过真实
ExecutionLeaseService::handle + wire（encode/decode）驱动，不以独立
bool 代替。

## 实现描述

按 Host Shared Lease Ownership + Closing Targets 两节冻结设计落地
（不新增 wire 字段、不引入新 lease 协议）：

1. **`SharedExecutionLeaseState`**（ExecutionLeaseService.hpp）：public
   `ProviderExecutionLeaseTable table` + `std::mutex prepareMutex`，
   构造接收 providerEpoch（host boot epoch）。同一 host 的所有 target
   实例共享一份 state，任一 target 不能通过单独重置表绕过已预留资源；
   计算槽是 host 资源，不随 serve 配置相加。
2. **构造面**：新增 overload
   `(providerName, targetServiceName, resolver, shared_ptr<SharedExecutionLeaseState>)`；
   原四参数 constructor 保留并委托
   `make_shared<SharedExecutionLeaseState>(providerEpoch)`，因此每个
   单 target 实例保持 T009-B 前的既有语义（旧 DiExecutionLeaseService
   suite 全绿即回归）。实例仍绑定单 target。成员由值 `m_table` +
   `m_prepareMutex` 替换为共享 `m_sharedState`（空指针构造抛
   invalid_argument）；`table()` 返回 `m_sharedState->table`，签名与
   noexcept 保留。
3. **prepare 串行化**：所有 target 的 resolver + table.prepare 在同一
   host prepare mutex 内执行，resolver 观察与 prepare 之间不能被另一
   target 抢占。
4. **非 Prepare target 绑定检查**：Core 的 commit/abort/renew/release
   签名只有 leaseId/epoch/requester/idempotency，共享表后必须先按
   lease 行归属路由：`table.find(leaseId)` 命中且
   `row.serviceName != m_targetServiceName` → `LEASE_SERVICE_MISMATCH`
   （不返回另一服务 lease 的任何细节）；未知 lease（find 落空）保持
   Core 既有 LEASE_NOT_FOUND/过期/重放处理；随后仍由 Core 检查
   requester/epoch/state/replay——find 命中不替代授权。检查放在
   dispatch 之前，因此跨 target 的重放也到不了 Core 的 replay
   tombstone（不会把 A 的记录结果经 B 的路由回放给调用方）。
5. **关闭不提前释放执行中槽**：lease 行只活在共享 table，target 的
   service 实例析构不触碰行；执行中（Executing）行一直占槽直到其
   owner 流在安全点 Release/Abort（Prepared/Committed 可 Abort，
   Committed/Executing 可 Release）。重复 serve 同一 target 共享同一
   state 时，新实例不能继承/盗用旧 Executing 槽。

## 执行命令与结果

```
./waf -o build-nac182 build --targets=unit-tests -j2   # rc=0（多轮，22s 级；
#   configure 复用 build-nac182 树，参数同 T008-B/T009-A evidence；零警告）
./build-nac182/unit-tests --run_test='Spec182SharedLease/*'
#   rc=0；Running 3 test cases ... *** No errors detected
#   （.codex-tmp/t009b-shared-lease.log）
./build-nac182/unit-tests --run_test=DiExecutionLeaseService
#   rc=0；Running 3 test cases ... *** No errors detected —— 旧单 target
#   回归（di-execution-lease-service.t.cpp 原 suite 零改动全绿）
#   （.codex-tmp/t009b-di-lease-regression.log）
./build-nac182/unit-tests --run_test='!StreamFacade'   # 完整回归
#   rc=0；Running 902 test cases ... *** No errors detected
#   （.codex-tmp/t009b-full-regression.log）
#   StreamFacade 族环境性段错误照旧排除（负结果见 failure-log 2026-09-07）。
```

raw 输出见 `.codex-tmp/t009b-shared-lease.log`、`t009b-di-lease-regression.log`、
`t009b-full-regression.log`。回归计数 899 → 902：净 +3 = 新 suite 3
cases；DiExecutionLeaseService 3 cases 与其余 suite 数量不变。

## 卡 Steps 对照

- **shared table/prepare mutex constructor**：SharedExecutionLeaseState
  承载两者；新 overload 接收 shared_ptr；原四参数 constructor 保留并
  委托自有 state（见实现描述 1/2）。
- **非 Prepare 操作 target 绑定检查**：commit/abort/renew/release 四路
  在 dispatch 前对已存在 lease 行做 serviceName 归属核对，跨 target 返回
  LEASE_SERVICE_MISMATCH（不泄露行细节）；prepare 不查（新行无 leaseId）。
- **复用 Core 身份/状态/重放验证**：绑定检查只路由不授权——requester
  mismatch / stale epoch / 状态迁移 / idempotent replay 全部仍由 Core
  表操作判定（case 2 显式断言 LEASE_REQUESTER_MISMATCH、
  LEASE_STALE_EPOCH、replay OK 与 replay 状态再验证）。
- **关闭不能提前释放执行中槽**：无新增 Core/DI 释放逻辑——共享 state
  下 target 实例析构与共享表解耦，Executing 行由 owner 流在安全点
  Release（case 3）；Prepared 槽由 owner Abort（case 1）。

## 卡 Verify 对照

- **CPP(Spec182SharedLease/*)**：3/3 全绿，全部走真实 handle + wire
  （encodeLeaseOperationRequest → handle → decodeLeaseOperationResponse）
  与真实 Core ProviderExecutionLeaseTable（含 validateAndActivate 生产
  激活路径与 table().find() 观察），无内部窥探。
  1. **Spec182SharedLeaseCrossServiceConflict**：同 host 两个 target
     解析到同一物理槽 → 只允许一个 Prepare 成功，另一 target 与同
     target 二次请求均 LEASE_CAPACITY_REJECTED（retryAfterMs==100，
     FIFO 等待队列）；host epoch 共享（`&table()` 同址、
     providerEpoch()==host-epoch）；owner Abort 自己的 Prepared lease
     后另一 target 同请求重试成功（lease-2）；对称方向：槽易主后原
     等待者重试成功（lease-3），新等待者再被拒——等待队列全程真实。
  2. **Spec182SharedLeaseTargetBinding**：A 的 Executing lease 上，B
     路由的 Commit/Abort/Renew/Release（即使 requester 是真实 owner）
     全部 LEASE_SERVICE_MISMATCH 且响应不带 leaseId/state/conflictKeys
     （无跨服务细节泄漏）；行状态未被动过（find 仍 Executing）；未知
     leaseId 走 Core → LEASE_NOT_FOUND（非 target 判定）；同 target
     上错误 requester → LEASE_REQUESTER_MISMATCH、stale epoch →
     LEASE_STALE_EPOCH（Core 授权未被 find 替代）；同 target 幂等
     replay（行状态未变的 commit replay、settled 后 release replay）
     仍成功；owner Release 后 Released 行上 B 的 Commit 依旧 mismatch。
  3. **Spec182ClosingServicePreservesSharedLeaseOwner**：A 的 service
     实例析构（close）后其 Executing lease 行仍留在共享 state（find
     Executing、epoch 不变）；B 不能操作该行（Abort/Release →
     LEASE_SERVICE_MISMATCH）也不能占该槽（新 Prepare 等待拒绝）；
     重新 serve A（新实例同 state）同样不能盗用旧 Executing 槽；
     A target 的 owner 流 Release 旧行成功后槽释放，B 的新 Prepare
     成功——关闭 A 不影响 B 继续 Prepare/执行，执行中槽只在安全
     释放点归还。
- **旧单 target 回归**：di-execution-lease-service.t.cpp 的
  DiExecutionLeaseService 3 cases（wire roundtrip、authenticated
  prepare + 伪 context、malformed/未知 schema fail-closed）零改动全绿
  ——原四参数 constructor 委托路径语义与 T009-B 前一致。
- **跨服务 NFD/真实双服务 PO-014**：留 T016（execution-units
  executeOwner 一致）；本卡 T009 的 ExecutionLeaseService 级真实双
  target 行为由 T009-C host 接线后在 T016 端到端复核。

## 关键发现与修正

- **boost 1.71 的逗号组合 selector 在此二进制不可靠**：
  `--run_test='Spec182SharedLease/*,DiExecutionLeaseService/*'` 报
  "no test cases matching filter"；单 suite selector 各自正常。验收与
  后续回归按单 selector 分开执行（manifest selector 保持冻结值
  Spec182SharedLease/*）。
- **frozen waitlist 的 FIFO 语义使"弃探针"残留队列位**：case 1 首版
  用不同 requestId 重试遭队列前位拦截（LEASE_CAPACITY_REJECTED）。
  修正为等待者以同 requestId 重试（真实客户端语义），并在对称腿复用
  同一等待者——队列行为由此被精确验证而非绕过。
- **Core replay 的 state 再验证语义**：成功 prepare/commit 的
  idempotency 在底层行状态迁移后重放会得 LEASE_INVALID_TRANSITION
  （非等待拒绝）——case 1 末腿改用新 request/idempotency 表达"新请求
  进队"，同 target replay 正向用例放在行状态未变的窗口（commit 后
  立即重放 / Release settled 后重放）。pre-check 位于 dispatch 前，
  跨 target 请求到不了 replay tombstone（case 2 的 Released 行上 B
  重放 commit 仍 MISMATCH）。
- **已存在的跨 target 行不因行 Released/Aborted 失去绑定**：Core 不
  擦行（Aborted/Released 保留供 replay/观察），find 命中持续拒绝
  另一 target——测试在 Executing 与 Released 两态都断言。

## 文件

- [ExecutionLeaseService.hpp](../../../NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionLeaseService.hpp)
  （SharedExecutionLeaseState；新 shared_state overload constructor；
  m_table/m_prepareMutex → m_sharedState）
- [ExecutionLeaseService.cpp](../../../NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionLeaseService.cpp)
  （SharedExecutionLeaseState ctor；两 constructor（委托）；handle
  prepare 走共享 mutex、非 Prepare find-then-route 绑定检查、全部
  table 操作走共享 state；table() 重定向）
- [di-native-provider-host.t.cpp](../../../tests/unit-tests/di-native-provider-host.t.cpp)
  （新 suite Spec182SharedLease 3 cases + wire/context/请求构造 helper）
- [case-manifest.json](../../../tests/fixtures/spec182/case-manifest.json)
  （T009-B 行 file 落位）
- [tasks.md](../tasks.md)（T009-B → DONE，含本 evidence 与 case 分布）
- ndn-service-framework/ExecutionLease.hpp/.cpp、旧 lease 测试文件零改动

## 残余风险

- find-then-dispatch 非原子：行在 find 与 Core 操作之间过期/清理时，
  Core 自己重新判定（NOT_FOUND/EXPIRED/replay 状态再验证），不会
  放行跨 target 操作，但错误码可能落到 Core 语义——由 Core 负责、
  非本卡可闭合的竞态窗口，T016 真实双服务下可复核。
- replay tombstone 在行被过期清理后仍保留成功结果（无 serviceName
  维度）；行存在时 pre-check 已挡跨 target，行消失后同 idempotency
  重放会命中 tombstone——需知道 A 的 leaseId+idempotencyKey 才能触
  发，属 Core replay 数据结构的既有边界，若 T016 暴露真实跨服务重放
  问题再在 Core 层收敛（不在本卡扩大 DI 侧）。
- 完整回归门排除环境性 StreamFacade 段错误族（负结果见
  docs/failure-log.md 2026-09-07）。
- T009-C host 接线将把固定 lease 入口按 request.targetServiceName
  路由到各 target 实例；"重复 serve 不覆盖固定 lease handler"与
  router active/draining 收尾在 T009-C 实现并验证。
