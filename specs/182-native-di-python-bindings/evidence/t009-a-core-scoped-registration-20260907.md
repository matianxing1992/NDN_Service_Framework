# T009-A Core Scoped Registration — 验收执行证据（2026-09-07）

Status: DONE。Selector：CPP(Spec182Registration/*)（6 cases，全部为本卡
新增 suite `Spec182Registration`）。
实现：在 Core `ServiceProvider`（ndn-service-framework）内增加
service-neutral scoped registration RAII 与 registration-generation
绑定/栅栏（fence）机制，全部通过真实 Core dispatch/worker/Face 路径
验证，不以独立 bool 代替。

## 实现描述

按 Host Registration Generation Decision + Provider Lifetime Control +
Request and Collaboration State Ownership 三份 Read，落地四组机制
（均不引入 DI 类型、不新增 wire 形状）：

1. **move-only RAII `ServiceProvider::ServiceRegistration`**：新增
   `addScopedService(serviceName, ackHandler, requestHandler, ServiceMode)`
   与 `addScopedCollaborationHandler(serviceName, roles, ackHandler,
   handler)`，返回持有 `RegistrationState`（serviceName/generation/
   closed atomic）的 handle。`close()` 一次性消费 handle：`m_state.reset()`
   + `state->closed.store(true)` + weak `RegistrationControl` 锁定 +
   Face 线程 post cleanup（闭包在 provider 已析构时经 owner 复检变
   no-op）。`closed()`/`valid()`/`generation()`/`serviceName()` 查询、
   析构自动 close。provider 析构批量 close 全部存活 entry。
2. **RegistrationControl 与锁外 capture 析构**：provider 持
   `shared_ptr<RegistrationControl>`（owner + mutex）；close 的 cleanup
   闭包只 capture control 与 state，post 后 provider 死亡时闭包不再
   触碰 provider（use-after-free 防护）；drain/detach/替换都在 control
   锁内完成，handler 所有者（RegisteredService）在锁外析构。
3. **pending/协作代次绑定**：正向 ACK commit（
   `finishAckDecisionOnEventLoop`）对带 registrationState 的注册把
   `pendingKey=requester/service/requestId → RegistrationState` 写入
   `m_pendingRegistrationStates`（与 `pendingRequests` 同锁同生命周期；
   `cleanupPendingRequestState` 一并释放，代码注释固化"绑定与 pending
   生命周期精确同步"）；collaboration 绑定独立于
   `m_collaborationRegistrationStates`（仅 prepare 写、controller 失效
   擦）。dispatch 前对"当前 entry 代次 vs 绑定代次"做
   `fencePendingRegistrationExecution`：绑定缺失（targeted accept /
   无早期声明）放行、mismatch 拒 `registration generation changed
   before execution` 并丢绑定、匹配则 claim-and-erase（首次 dispatch
   后执行期栅栏改为捕获 state）。
4. **同步 fallback 与晚到结果全覆盖**：dispatch 双路径同 fence——worker
   `dispatchRequestExecutionAsync` 入口（closed gate + fence，post 前
   同步执行）与 pool-0 inline `gateInlineRequestExecution`（同 gate）；
   已排队任务的 worker 内再查 closed；late ACK 决策落 Face 时若注册
   已 close，`finishAckDecisionOnEventLoop` 的 closed gate 把正向降级
   为负向（不 store pending、不发正向 ACK）；collab dispatch
   （`dispatchCollaborationExecutionAsync`）同 gate（5339-5356 族：
   "collaboration registration closed/generation changed"）。
   legacy 注册路径（无 registrationState 的 entry）行为不变。

## 执行命令与结果

```
./waf -o build-nac182 build --targets=unit-tests -j2   # rc=0（两轮，均
#   22s 级；configure 复用 build-nac182 树，参数同 T008-B evidence；
#   编译零警告）
./build-nac182/unit-tests --run_test='Spec182Registration/*'
#   rc=0；Running 6 test cases ... *** No errors detected
#   （.codex-tmp/t009a-focused.log；套件级 6/6 全绿）
./build-nac182/unit-tests --run_test='!StreamFacade'   # 完整回归
#   rc=0；Running 899 test cases ... *** No errors detected
#   （.codex-tmp/t009a-full-regression.log）
#   注：裸跑仍见环境性 StreamFacade 族 PredictiveProviderExactWireValidation-
#   AndAtomicFlush 段错误 rc=139，与 T006-B/C/D/T007-A/B/T008-A/B 同族，
#   负结果已记 failure-log 2026-09-07；回归 gate 固定排除该族。
```

raw 输出见 `.codex-tmp/t009a-focused.log`、`t009a-full-regression.log`。
回归计数 893 → 899：净 +6 = 新 suite 全部 6 cases；无其它 suite 数量
变化。

## 卡 Steps 对照

- **service-neutral scoped registration RAII**：handle 即
  `ServiceRegistration`（move-only，close 消耗），注册按
  serviceName 字符串/Name 建 `RegisteredService`/collab 记录，不承载
  任何 DI 类型字段。
- **RegistrationControl**：`shared_ptr` 双归属（provider 成员 +
  handle 弱引用），close 与 provider 析构的竞态以 owner 复检收敛。
- **pending/协作代次绑定**：accept commit 写 request pending 绑定；
  collaboration binding 与 request pending 分离存储、独立生命周期；
  fence 是 dispatch 唯一放行判据（不新增"已注册 bool"）。
- **保留 legacy API**：既有 addService/注册路径不改签名，无
  registrationState 的 legacy entry 语义不变（无绑定即放行）。
- **同步 fallback、晚到结果、锁外 capture 析构**：如上实现描述 2/4；
  6 cases 分别覆盖晚到 ACK commit（close 窗口）、晚到旧代次
  Selection（reregister 后）、worker/同步两条 dispatch 面、协作
  pending cleanup 边界、provider 析构竞态、cleanup 精确 key。
- **不引入 DI 类型或新 wire**：全卡只动
  ServiceProvider.hpp/.cpp + 测试；无 wire/DI include（git diff 验证）。

## 卡 Verify 对照

- **CPP(Spec182Registration/*)**：6/6 全绿，gate 在
  `tests/unit-tests/di-native-provider-host.t.cpp`（tests glob 自动拾取，
  无 wscript；case-manifest T009-A 行落位 file + suite selector + 6
  named cases，见文件清单）。全部断言面是可观察状态（executions
  计数、`hasPendingRequestForTokenTest`、`getSelectionExecutionStatus`
  的 state/message 精确串、handle.closed()/valid()），不用独立 bool。
  1. **CloseBeforeAckFinish**（ack 窗口）：ack worker 决策后、commit
     未落 Face 时 close；真实 ack 决策（sleep 400ms）+ Face pump 后
     pending 不 store、request handler 不执行——closed gate 在
     `finishAckDecisionOnEventLoop` 把正向降级负向。
  2. **OldSelectionAfterReregister**（worker 面 fence）：真实 accept
     （worker decode + io-post commit 落地）→ close → 同服务重注册 →
     gen1 旧 token 的 selection 在 worker dispatch 入口被代次 fence
     拒：`Failed` + message 含 "generation changed"，两代 executions
     均 0。
  3. **InlineDispatchFence**（同步面 fence）：同一 fence 覆盖 pool-0
     inline dispatch（inline 不能绕过代次绑定）；正向对照：重注册后
     新 userToken 请求 accept + selection 执行成功（gen2 executions
     ==1）。
  4. **SiblingAfterPendingCleanup**（collab cleanup 边界）：role A
     pending 经真实 cleanupPendingRequestState 清理后 collab 代次绑定
     保留；重注册后兄弟 role B dispatch 被 collab mismatch gate 拒
     （"collaboration registration generation changed"）。
  5. **CloseAfterProviderDestruction**（锁外 capture 析构）：provider
     析构后 handle.closed()==true、valid()==false、close() no-throw、
     move 语义完整——close 闭包捕获 control/state 而非裸 provider。
  6. **CleanupDoesNotEraseSuccessor**（cleanup 精确 key）：同 requester
     两个独立 userToken 请求先后 accept；cleanup 第一个只释放自己的
     pending 与绑定，第二个的 selection 仍执行成功。
- **通过真实 Core dispatch/worker/Face 检查**：非直接调 dispatch
  gate——case 1/2/4 启用真实 ack/handler worker（setAckThreads(1)/
  setHandlerThreads(1)），请求与 selection 走
  `OnRequestDecryptionSuccessCallbackV2` → decode → ack 决策 →
  finishAckDecisionOnEventLoop（Face 线程 io post）→ dispatch
  worker post 全链；case 2/3/6 用真实 wire（encode+decode 往返）注入
  并验证 replay hash（独立 userToken 必要性的实证）；case 4 的
  cleanup 用真实 `cleanupPendingRequestState`。
- **跨服务 NFD 用例留 T016**：与 manifest T009-A executeOwner 一致，
  多 provider/NFD 真实网络路径属 T016 集成。

## 关键发现与修正

- **case 2（worker 面）初版 Queued 而非 Failed 的根因是编排而非
  实现**：`OnRequestDecryptionSuccessCallbackV2` 在 handler pool>0 时
  把 decode post 到 worker，worker 内 decode 完成后 commit 经
  `boost::asio::post(io)` 落 Face 线程——accept 绑定写入依赖 io
  pump。`hasPendingRequestForTokenTest` 探测被 token 种子预置而失真
  （种子先于 accept commit 写入 pendingRequests），drain 只保证
  decode 完成不保证 commit。修正编排 = drain（decode 完成）+ pump
  io（commit 落 Face）后再 close；修后 fence 同步拒、6/6 全绿。
  测试文件注释与 helper 注释同步修正（"accept 链同步完成"仅对
  pool=0 成立）。
- **close() 的 cleanup 是 io-post 闭包**：`closed` atomic 翻转即可被
  任何 dispatch 线程观察；entry 拆除（detach）走 Face 线程，闭包
  只持有 RegistrationControl + state，provider 先死时闭包 no-op
  （case 5 锁定）。
- **绑定与 pending 同生命周期**：`m_pendingRegistrationStates` 释放点
  只有 `cleanupPendingRequestState`（精确 pendingKey）与 fence 的
  claim/mismatch 两处；close/detach/reregister 不触碰绑定——旧代次
  selection 的拒绝依据正是跨 close 保留的绑定（case 2/3 语义）。
- **key 构造统一**：绑定与 fence 均用 requester/service/requestId
  三元 pendingKey；`Name(uri)` parse 与 `Name` 拷贝对普通名等价，
  case 3/6 双面验证（否则 cleanup 宽擦/窄擦或 key 分叉都会使
  case 6 或 case 4 失败）。

## 文件

- [ServiceProvider.hpp](../../../ndn-service-framework/ServiceProvider.hpp)
  （RegistrationState/RegistrationControl/ServiceRegistration 类型、
  addScopedService 双载/addScopedCollaborationHandler、m_useTokens
  默认 true 保持、m_pendingRegistrationStates/
  m_collaborationRegistrationStates 数据成员）
- [ServiceProvider.cpp](../../../ndn-service-framework/ServiceProvider.cpp)
  （addScoped*/drainClosedRegistrations/close 闭包/detachClosedRegistration；
  finishAckDecisionOnEventLoop closed gate + 绑定写；dispatch 双路径
  fence + collab fence；cleanupPendingRequestState 绑定释放；
  RegistrationControl 竞态收敛）
- [di-native-provider-host.t.cpp](../../../tests/unit-tests/di-native-provider-host.t.cpp)
  （新：Spec182Registration suite 6 cases + wire/selection/inject/collab
  测试入口与 drainHandlerWorker 同步 helper）
- [case-manifest.json](../../../tests/fixtures/spec182/case-manifest.json)
  （T009-A 行 file 落位；existingSuite/existingCases 语义与 T007-B
  新建 suite 先例一致）
- [tasks.md](../tasks.md)（T009-A → DONE，含本 evidence 与 case 分布）

## 残余风险

- close() 的 entry 拆除依赖 Face 线程 pump；真实生产路径 io 常驻，
  但任何"close 后立即析构 provider 且 io 已停"的边界由 RegistrationControl
  owner 复检兜底（case 5），T016 真实 NFD 生命周期可再核对。
- case 1 的 400ms ack sleep 与 case 2 的 300ms io pump 是真实异步链的
  有界等待；若 CI 机负载使 worker/io 调度超窗，case 会先失败在
  waitUntil/断言（而非误绿），时间窗需随环境校准。
- 完整回归门排除环境性 StreamFacade 段错误族（负结果见
  docs/failure-log.md 2026-09-07）；focused/回归 suites 与全量（排除
  该族）均绿。
- T009-C 的 host 抽取将复用本卡的 RegistrationControl/绑定机制；
  若 host 与多 provider 共享 Face 的编排暴露新竞态，fence/绑定策略
  在该卡收敛，不在本卡扩大。
