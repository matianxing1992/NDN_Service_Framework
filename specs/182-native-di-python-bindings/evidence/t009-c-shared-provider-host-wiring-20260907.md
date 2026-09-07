# T009-C Shared Provider Host Wiring — 验收执行证据（2026-09-07）

Status: DONE。Selector：CPP(Spec182ProviderHost/*)（6 cases，新 suite
`Spec182ProviderHost`，宿主于 di-native-provider-host.t.cpp）。实现：
`NativeInferenceProvider`（host 单例 + 固定 lease 入口 + 共享 lease 表 +
每 target 实例路由）完整落地，examples/DI_NativeProviderExecutable.cpp
迁移为单一 host.serve 注册路径（CD-014：executable/CLI/绑定共用同一
host，不保留第三套初始化路径）。真实 Core scoped 注册/request/selection
全链驱动；collab handler 真实执行与真实 NFD 多入口留 T016。

## 实现描述

按冻结 CD-014/O-004 落地（不新增 wire 字段、不改变 lease 协议、不引入
第三套服务注册路径）：

1. **`HostState` 单例**（NativeInferenceProvider.cpp，opaque 于 hpp）：
   providerName / providerBootId / workerSlots（compute-slot 范围 =
   host 资源，取首次 serve 的 max(1, workerCount)，后续 serve 不得
   重塑）/ sharedLease（SharedExecutionLeaseState，host boot epoch）/
   fixedLease（固定 lease 入口的 Core registration）/ routerMutex /
   targets map（serviceName → Target{core 注册, lease 实例, draining}）。
   固定入口、collab handler、registration closure 全部 capture host
   的 shared_ptr 链——即使 provider 对象先被销毁，Core 未 detach 前
   表与 router 仍存活。
2. **首次 serve 发布单固定 lease 入口**：`addScopedService`(
   `EXECUTION_LEASE_SERVICE_NAME`, 默认 accepting ack handler,
   `makeLeaseRouter(host)`, NormalAndTargeted)。注册失败 → m_host
   reset + rethrow（不残留半初始化 host）。
3. **`makeLeaseRouter`**：decodeLeaseOperationRequest → routerMutex 下按
   `operation.targetServiceName` 查 target → miss =
   LEASE_SERVICE_MISMATCH；target draining 且 op ∉ {Abort, Release} =
   LEASE_TARGET_DRAINING（Abort/Release 继续路由，让在途记录走完
   清理）；否则 `target->lease->handle(context, payload, now)`。wire
   Response 恒 status=true，错误一律在 encoded LeaseOperationResponse
   内返回——异常绝不跨 Core 回调。
4. **serve() fence 次序**：serviceName/allowedRoles 非空 →
   invalid_argument；host stopped → runtime_error；boot 身份一致性
   （非空 localProviderName/providerBootId 必须等于 host、workerCount
   必须等于 host 槽位范围）→ invalid_argument；同名 active target →
   logic_error（在 config 检查之前）；executionLeaseTargetService 非空
   时必须 == serviceName 且 executionLeaseTable 必须为空 → host 注入
   `&host->sharedLease->table`；table 非空而无 target → invalid_argument
   （lease 表 host-owned，调用方只绑定 target）。runtime 先组装
   （makeNativeProviderCollaborationRuntime，runnerFactory 仍必需），
   再调 runtimeObserver seam，最后 addScopedCollaborationHandler 安装；
   draining 窗口由 guarded handler 覆盖（Core closed/generation gate
   是权威 fence）。安装成功后在 routerMutex 下 replace draining record
   ——re-serve 不继承旧 target 的 lease 实例/draining fence/bindings。
5. **`NativeServiceRegistration`**（move-only RAII，CD-014 签名）：
   close() 幂等——先抬 draining fence（固定入口对新
   Prepare/Commit/Renew 拒、Abort/Release 仍路由），再 core->close()；
   closed() = core closed；valid() = handle 曾绑定注册（close 后仍
   true，与 Core ServiceRegistration close-后-valid=false 不同——
   冻结语义）；generation()/serviceName() 转 core。
6. **stop() 幂等**：先关固定入口，再遍历 targets 抬 fence + 关 core；
   stop 后 serve → runtime_error；dtor 同 stop。host 从不 close 共享
   ServiceProvider/Face、从不 join I/O 线程。
7. **example 迁移**（DI_NativeProviderExecutable.cpp）：provider 改
   shared_ptr（LocalServiceProvider 经 make_shared），serve 提前到
   installTask（注册面线程约束：Face 线程或 event loop 前）；main
   detach 后、processEvents 前等待 serveCompleted cv/atomic（catch 也
   signal，失败不使 main 死等）；旧 exec-lease 单实例注册块删除，
   替换为 `providerHost->serve(nativeService, config)`；config 不再注入
   executionLeaseTable（host 注入）；ackHandler/runtimeObserver 经
   NativeServiceDefinition seam 保留原 readiness/capacity/evidence
   接线；registration 由 main thread 的 nativeRegistration 持有
   （detached thread 退出不 close）。

## 执行命令与结果

```
./waf -o build-nac182 build --targets=unit-tests -j2   # rc=0
./build-nac182/unit-tests --run_test='Spec182ProviderHost/*' --log_level=error
#   rc=0；Running 6 test cases ... *** No errors detected
#   （.codex-tmp/t009c-provider-host.log）
./build-nac182/unit-tests --run_test='Spec182Registration/*' --log_level=error
#   rc=0；Running 6 test cases ... *** No errors detected —— T009-A suite
#   同文件回归（.codex-tmp/t009c-registration-legacy.log）
./build-nac182/unit-tests --run_test='Spec182SharedLease/*' --log_level=error
#   rc=0；Running 3 test cases ... *** No errors detected —— T009-B suite
#   同文件回归（.codex-tmp/t009c-shared-lease-legacy.log）
./build-nac182/unit-tests --run_test=DiExecutionLeaseService --log_level=error
#   rc=0；Running 3 test cases ... *** No errors detected —— 旧单 target
#   回归（di-execution-lease-service.t.cpp 零改动全绿）
#   （.codex-tmp/t009c-di-lease-legacy.log）
./build-nac182/unit-tests --run_test='!StreamFacade'   # 完整回归
#   rc=0；Running 908 test cases ... *** No errors detected
#   （.codex-tmp/t009c-full-regression.log；首次运行另存
#   .local-tmp/spec182-t009c-full-902.log）
#   StreamFacade 族环境性段错误照旧排除（负结果见 failure-log 2026-09-07）。
./waf -o build-nac182 build --targets=di-native-provider -j2
#   rc=0；'build' finished successfully (3.169s)——增量复验 example 接线
#   （本卡执行中的完整 compile+link rc=0，3m12s/72 steps；
#   .codex-tmp/t009c-example-build.log）
```

回归计数 902 → 908：净 +6 = 新 suite 6 cases；Spec182Registration 6、
Spec182SharedLease 3、DiExecutionLeaseService 3 与其余 suite 数量不变。

## 卡 Steps 对照

- **抽取共享宿主、lease 表与 factory 注入**：HostState 单例 + 首次
  serve 固定入口 + SharedExecutionLeaseState(host boot epoch)（实现
  描述 1/2）；每 target 独立组装 runtime/ExecutionLeaseService 但共享
  table + prepare mutex（槽是 host 资源，不随 serve 相加）；runnerFactory
  仍是 config 必需（host 不代造 factory，CLI/绑定注入）——O-004。
- **serve/close/stop fence 属同一行为单元**：active duplicate →
  logic_error；close 抬 draining → 同名可立即 re-serve（record 替换，
  不继承旧 lease/fence/bindings）；close/stop 幂等；stop 后 serve →
  runtime_error；boot 身份/槽位一致性检查；lease table host-owned
  注入（实现描述 4/5/6）。
- **保留 shared Face 与管理权限隔离**：host 持有 m_provider
  shared_ptr 从不 close/join；provider.reset() 不提前关闭 registration
  （case 4 显式断言）；固定入口 addScopedService + 每 target
  addScopedCollaborationHandler 独立 allowedRoles，双 target 各自权限
  面独立（case 6 双服务经同一入口、permission 分开授予）。
- **generation 验证**：registration.generation() 直通 core
  generation；close→re-serve 后 generation 单调递增（case 3）；
  T009-A 的 pending 代次绑定继续生效（晚到 request 到 Core gate）。
- **CLI 仅构造配置并调 host**：example 迁移为唯一注册入口；wscript
  零改动——unit-tests 经 ant_glob `ndnsf-di/*.cpp` 自动收录 host
  实现，examples/wscript 已列 NativeInferenceProvider.cpp（line 292）。

## 卡 Verify 对照

- **CPP(Spec182ProviderHost/*)**：6/6 全绿，全部走真实
  addScopedService/addScopedCollaborationHandler + 真实 Core
  request/selection 全链，无内部窥探：
  1. **Spec182ProviderHostDualTargetSharedHostFencesDuplicate**：同一
     host serve A+B 均 valid、未 closed、serviceName 匹配、
     generation>0；active 同名重复 serve 被 host gate 拒（logic_error）
     且 sibling B 不受影响。
  2. **Spec182ProviderHostHostConfigConsistencyFences**：改名
     providerName / 换 providerBootId / 改 workerCount 三个后续 serve
     均 invalid_argument（不悄悄重塑 host boot 身份与槽位范围）；
     同名 active duplicate 的 logic_error 先于 config 检查触发。
  3. **Spec182ProviderHostCloseAllowsSameNameReServe**：close 幂等
     （重复 close no-throw、closed() true）；draining record 被替换——
     同名立即 re-serve 成功、generation2 > generation1、旧 handle 保持
     closed、新 registration 的 duplicate 再次被拒。
  4. **Spec182ProviderHostStopClosesAllAndFencesServe**：close A 后 B
     仍开放；外部 provider owner reset 不提前关闭 registration（host/
     closure 链持有）；stop() 关闭全部 target 且幂等；stop 后 serve →
     runtime_error；host.reset() 后残留 registration handle 仍可安全
     close（no-throw）。
  5. **Spec182ProviderHostLateAckAfterCloseHitsCoreBoundary**：真实
     accept R1 经 host-installed def.ackHandler（ackCalls 1、pending
     建立）；close 后晚到 R2（新 user token/requestId）的 ack 决策仍被
     询问（ackCalls 2）、pending 仍记录到 cleanup boundary——Core close
     fence 在 dispatch/execution 层而非 decrypt→ack 层（同
     Spec182Registration selector 1/2 冻结语义）；host 侧断言晚到窗口
     不炸、重复 close 安全。
  6. **Spec182ProviderHostFixedLeaseEntryRoutesRealDispatch**：真实
     Core 全链 dispatch（EXECUTION_LEASE_SERVICE_NAME permission +
     authenticated wire → pending seed → injectRequest → selection
     buffer → OnServiceSelection... → getSelectionExecutionStatus）：
     A+B 并存时 Prepare(A) 路由到 A 的 lease instance → Completed（无
     "exception"）；regA.close() 后 Prepare(B) 仍经共享固定入口 →
     Completed（另一共享服务可用 = PO-014 区分性断言）；draining A 的
     晚到 Prepare 由 router 应答 → Completed（不崩溃、不牵连入口）；
     re-serve A 后 Prepare(A) → Completed（旧 draining record 替换、
     旧 lease 行残留不影响新注册 Core 面）。
- **旧 suite 回归**：Spec182Registration 6（T009-A）、Spec182SharedLease
  3（T009-B）同文件零改动全绿；DiExecutionLeaseService 3（旧单 target）
  零改动全绿——U 文件追加 suite 不扰动前卡行为。
- **真实 NFD 多入口 / collab handler 真实执行**：留 T016（executeOwner），
  I/di-native-provider-host.t.cpp 由 T016 承接（与 T008-A/T009-A/B 的
  I-layer 义务移交惯例一致；不建空占位、不接未验证 build target）。

## 关键发现与修正

1. **build-nac182 configure 漂移**：13:03 重新配置只带 --with-tests，
   丢 --with-examples → examples/wscript:249 的
   `if not bld.env.WITH_EXAMPLES: return` 使 di-native-provider target
   消失（旧 binaries 成孤儿）。补 --with-examples 重新 configure
   （6.6s）恢复，保留冻结 NAC-ABE/ONNX prefixes——非代码问题。
2. **Core 注册面线程约束落到 example**：固定入口与 collab 注册必须在
   Face 事件线程或 event loop 前（installTask 内 serve）；main 线程
   detach 后等待 serveCompleted cv/atomic，catch 分支同样 signal——
   serve 失败时 main 不会永久等待。
3. **双服务单入口路由语义**：固定 lease 入口按 request.targetServiceName
   查 router 表，一次 addScopedService 承载 host 全部 target；未知
   target/draining/内部错误都转 in-band code，wire Response 恒
   status=true——lease 客户端总能 decode 到一个响应（实现描述 3）。
4. **迁移遗留修正**：provider 改 shared_ptr 后 main thread
   （fetchPermissionsFromController/init/setNdnsdMeta）与 markReady
   block（updateNdnsdMeta）两处 dot-style 遗漏 → provider->；测试
   常量 HOST_PROVIDER_NAME 与 T009-B anonymous namespace 冲突 →
   rename NATIVE_HOST_PROVIDER_NAME/NATIVE_HOST_BOOT_ID；serveCompleted
   信号改走 capture 的 lambda（installTask 不直接锁 cv）。
5. **close fence 位置实测（与假设相反）**：曾假设 Core close 拒绝新
   accept/pending；实测 close fence 在 dispatch/execution 层——closed
   后新 request 的 ack 仍被询问、pending 仍建立（保留到 cleanup
   边界）。case 5 改写为断言真实边界（ackCalls→2、pending 建立、注册
   链完好），与 T009-A selector 1/2 冻结语义一致。

## 文件

- [NativeInferenceProvider.hpp](../../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.hpp)
  / [.cpp](../../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.cpp)
  （CD-014 host 单例实现；本卡重写）
- [DI_NativeProviderExecutable.cpp](../../../examples/DI_NativeProviderExecutable.cpp)
  （serve 提前 + host.serve 单一注册路径 + 等待语义；删旧双注册块）
- [di-native-provider-host.t.cpp](../../../tests/unit-tests/di-native-provider-host.t.cpp)
  （新 suite Spec182ProviderHost 6 cases + host config/service/helper）
- [case-manifest.json](../../../tests/fixtures/spec182/case-manifest.json)
  （T009-C 行 file 落位 + 6 named cases 登记）
- [tasks.md](../tasks.md)（T009-C → DONE，含本 evidence 与 case 分布）
- examples/wscript、tests/wscript 零改动（实现描述 7/Steps 末条）

## 残余风险

- collab service 的真实 handler dispatch（准备/执行/证据接线）未被
  U-layer 触达——Spec182ProviderHost 用固定 lease 入口 + host 面
  断言覆盖生命周期，handler 深水区与真实 NFD 多入口（PO-014 L3）由
  T016 的 I/di-native-provider-host.t.cpp 承接；card Verify 末尾已
  明示。
- guarded handler 的 draining fence 与 Core entry detach 之间存在小
  窗口，权威 fence 是 Core closed/generation gate（T009-A selector
  1/2 已覆盖同机制）；host 侧 fence 只防窗口内 dispatch。
- fixed lease entry 在 stop 后仍可能被 in-flight capture 短暂持有
  （router/闭包链共同保活至 Core detach）；stop 幂等 + dtor 兜底，
  无泄漏路径；真实长时间运行复核留 T016。
- 完整回归门排除环境性 StreamFacade 段错误族（负结果见
  docs/failure-log.md 2026-09-07）。
