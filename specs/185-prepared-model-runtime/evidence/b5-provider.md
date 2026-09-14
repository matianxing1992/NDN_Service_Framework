# B5 Provider Assembly Evidence

## T009 static gate failure

时间：2026-09-13（America/Chicago）。

冻结快照：`.codex-tmp/spec185-b5-t009-static-v2`；base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `e6c5d5d7816883923e6e943a8251267e752282c29e6c5c7041153567374a0d92`；paths manifest SHA256 `091ccfa21868e20029af4ac783feef6af6397831f1c9dc684f5cc8a797190965`。

官方 `review-agent` 结果：`STATIC_FAIL`。审查覆盖五 lane，未运行构建或测试。

- P1：Provider 未完成实际 Core/证书发布/IO worker 初始化，memory KeyChain 与 ServiceProvider owner 不一致，不能形成生产请求闭环。
- P1：Provider handler 固定空 runner factory 且没有 `runnerPreparationFactory`，authenticated Selection 会以 `DI_PROVIDER_ASSEMBLY_FACTORY_MISSING` fail closed。
- P1：ServiceDefinition roles 未与 ProviderConfig 的已验证允许集绑定。
- P1：Provider drain/async drain 立即返回，未构成已接受工作、Face、worker 和 timer 的本地屏障。
- P1：`Runtime::provider(config)` 已有 Provider 时静默忽略不兼容配置。
- P2：Provider 异常未映射为稳定 `DiError`；CLI 静默丢弃 `--plan`/`--manifest`/`--artifact-cache-dir` 并接受编排 flag，语义不完整。
- P2：C++ fixture 没有 authenticated Selection、权限/证书、assembly、wrong identity/epoch/grant 或已接受工作清理 oracle。

T009 保持 `NOT_STARTED`/未验收；修复后须重新冻结包含全部新增文件的快照并复审，不能将本次静态失败计为通过。

## T009 repair review failure

修复快照 `.codex-tmp/spec185-b5-t009-static-v3` 经官方 `review-agent` 复审仍为 `STATIC_FAIL`，未构建/未运行：

- P1：Provider drain 超时后 `stopped` 使后续 stop 提前返回，joinable IO 线程可能在 State 析构时触发 terminate。
- P1：Runtime provider-only drain 忽略 Provider false 结果，错误设置 `Drained`/通知成功。
- P1：wrapper 自建 controller identity 未复用 Controller permission/bootstrap 链，authenticated Selection 启动契约未闭合。
- P2：manifest 仅解析，未参与 assembly/source identity 验证；serve 后 startIo 异常无 registration 回滚；关键 authenticated/permission/assembly/清理 oracle 缺失。

T009 仍为 `PARTIAL`，必须修复后重新冻结审查。

## T009 repair review failure (v4)

修复快照 `.codex-tmp/spec185-b5-t009-static-v4`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `dc0de4...`；paths manifest SHA256 `318e48...`）经官方 `review-agent` 复审仍为 `STATIC_FAIL`，未构建/未运行。审查覆盖五 lane，发现：

- P1：受保护 Selection 没有在 `NativeProviderHandlerConfig` 中接线 `protectedRuntimeFactory`，非明文 Selection 会稳定失败为 `DI_PROTECTED_RUNTIME_FACTORY_MISSING`。
- P1：Provider drain 仍忽略 provider drain 结果；异步 API 实际同步执行并返回空订阅。
- P1：`stopIo` 解锁后再 join，两个并发 stop/drain 可能对同一 IO 线程重复 join。
- P1：controller certificate 的测试 fallback 仍应明确隔离外部证书/测试入口，不能成为默认生产身份来源。
- P2：C++ fixture 没有 authenticated Selection/assembly 的真实 oracle，重复注册的错误映射也未有独立断言。

本轮随后修复了受保护工厂接线、Runtime drain 结果传播、Provider 异步生命周期 worker 和 IO 线程身份判定；这些修复必须在新的不可变快照中重新审查，v4 本身不能计为静态通过。

## T009 repair review failure (v5)

修复快照 `.codex-tmp/spec185-b5-t009-static-v5`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `680fc58b6fa015ab807c0dfef152ce78f6c7f8ff71f74576346378ce0a548457`；paths manifest SHA256 `cbdf116df46253988b66fe5e9c66854e5a565a952b59509f79fc20c654c63098`）经官方 `review-agent` 复审仍为 `STATIC_FAIL`，未构建/未运行。审查发现：

- P1：IO 启动后 `Provider::serve` 仍从公开线程直接调用要求 Face 线程的 native serve，必须在 Face 线程执行并在锁外等待。
- P1：Provider `stop()` 仍同步等待最多 300 秒，`Runtime::close()` 因而违反 non-blocking close 契约；应拆分停止接收与显式 drain/join 屏障。
- P2：T009 fixture 缺失有效 authenticated Selection/assembly、wrong provider/epoch/grant 和 source/assembly/runner counter oracle。
- P2：fixture 丢弃 `drainAsync` 返回的 Subscription，可能在 callback 投递前取消订阅。

本轮 v5 仅完成静态审查；修复后必须重新冻结快照并复审，不能将该结果计为通过。

## T009 repair review failure (v6)

修复快照 `.codex-tmp/spec185-b5-t009-static-v6`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `f9bbe699d52f3c4b38bff5a10bfb8a16f52f12afc453a17de0ab322c7a22c863`；paths manifest SHA256 `cbdf116df46253988b66fe5e9c66854e5a565a952b59509f79fc20c654c63098`）经官方 `review-agent` 复审仍为 `STATIC_FAIL`，未构建/未运行。发现：

- P1：`ProviderCounters` 声明、定义和 fixture 调用类型不闭合，当前快照会在编译/链接门失败。
- P1：Face dispatch 无条件等待；并发 `requestStopIo` 可停止 IO 后使 serve 永久等待，且 `serveMutex` 跨等待。
- P1：registration close 后的 ServiceProvider 异步 detach 没有纳入 drain cleanup barrier，可能在 IO 线程退出时仍未完成。
- P2：正向 authenticated Selection→grant/assembly/runner/Response 及 wrong provider/epoch/grant C++ oracle 仍缺失。
- P2：`startIo` 状态检查与获取 IO 锁分离，serve/stop 可交错重新启动 worker。

本轮 v6 仅完成静态审查；修复后必须重新冻结快照并复审，不能将该结果计为通过。

## T009 repair review failure (v7)

修复快照 `.codex-tmp/spec185-b5-t009-static-v7`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `e0cb83e5f2756ca41a59ee8dc56a79049707fc2c71122fac13156ee5b41f9731`；paths manifest SHA256 `cbdf116df46253988b66fe5e9c66854e5a565a952b59509f79fc20c654c63098`）经官方 `review-agent` 复审仍为 `STATIC_FAIL`，未构建/未运行。v6 的 P1 已修复；剩余 P2 为：

- `serveMutex` 仍跨 Face condition wait，可能阻塞并发 serve/stop 路径，需释放锁后等待并以可取消的 pending-call 集合收敛。
- serve/start/stop 的部分异常在稳定错误映射之外直接抛出，需统一为契约规定的 `DiError` 边界。
- T009 C++ fixture 仍未实际执行 authenticated Selection → grant/assembly/runner → Response，也没有 wrong provider/epoch/grant 与正向计数器 oracle。

本轮 v7 仍仅完成静态审查；修复并重新冻结快照前不得构建或将 T009 计为通过。

## T009 repair review failure (v8)

修复快照 `.codex-tmp/spec185-b5-t009-static-v8`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `eb22acc043347bae93da0c2968af9c7b24ed37a60fab7e93076fdc89c1c4aedb`；paths manifest SHA256 `cbdf116df46253988b66fe5e9c66854e5a565a952b59509f79fc20c654c63098`）经官方 `review-agent` 复审仍为 `STATIC_FAIL`，未构建/未运行。发现：

- P1：fixture 仍以 `std::invalid_argument` 捕获已改为 `DiError("INVALID_ARGUMENT")` 的 allow-list 错误。
- P1：生产代码的 `NDNSF_SPEC185_ALLOW_LOCAL_CONTROLLER` 回退未以 test-only 编译 seam 隔离。
- P1：正向 fixture 直接构造 Native handler，尚未经过新增 `Provider::serve` facade 和 authenticated grant/assembly 路径。
- P2：wrong provider/epoch/grant 与 `Provider::counters()` 正向计数 oracle 缺失。
- P2：IO 线程调用 `Provider::drain()` 时可能在 worker join 完成前返回成功。
- P2：service/role 名称 canonicalization 的底层异常仍可能逃逸稳定 `DiError` 映射。

本轮 v8 仍仅完成静态审查；修复并重新冻结快照前不得构建或将 T009 计为通过。

## T009 repair review failure (v9)

修复快照 `.codex-tmp/spec185-b5-t009-static-v9`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `6982125a9bd314ab8ed20f19b0458594ab2d27429733498c5f70ccacb0395cef`；paths manifest SHA256 `cbdf116df46253988b66fe5e9c66854e5a565a952b59509f79fc20c654c63098`）经官方 `review-agent` 复审仍为 `STATIC_FAIL`，未构建/未运行。发现：

- P1：ProtectedRuntime factory lambda 使用 `nowMs` 但未捕获，测试翻译单元确定无法编译。
- P1：`ProviderConfig::Impl` 在匿名命名空间中定义，解析自由函数直接访问私有实现及 `m_impl`，命名空间和访问控制均不闭合。
- P2：wrong provider/epoch/grant 仅调用 `validateProtectedRuntimeBinding`，没有通过 Provider 生产 ingress、grant factory 和 assembly 路径。
- P2：`makeProviderRunnerFactory` 与 `installNativeProtectedGrantFactory` 位于统一 `DiError` 映射 try 之外，初始化异常可能逃逸为原生标准异常。

本轮 v9 仍仅完成静态审查；上述问题修复后必须重新冻结快照并复审，T009 保持 `PARTIAL`。

## T009 repair review failure (v10)

修复快照 `.codex-tmp/spec185-b5-t009-static-v10`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `d312bedd6e81c24bfd66c0efa1e3c8e922f2896b39e62448af85b7ea15c4a957`；paths manifest SHA256 `cbdf116df46253988b66fe5e9c66854e5a565a952b59509f79fc20c654c63098`）经官方 `review-agent` 复审仍为 `STATIC_FAIL`，未构建/未运行。发现：

- P2：Provider-only `Runtime::drain`/`drainAsync` 调用 `Provider::drain` 前没有把 Runtime 从 `Open` 转为 `Closing`；Provider 失败后 Runtime 仍报告 `Open`，底层却已停止。
- P2：正向 fixture 通过 test preparation/runner factory 伪造 runner spec，并断言 `sourceFetches == 0`，没有覆盖 `NativeCanonicalOnnxAssembler` 的真实 source-fetch/assembly 路径。

本轮 v10 仍仅完成静态审查；上述生命周期和真实 assembly oracle 修复后必须重新冻结快照并复审，T009 保持 `PARTIAL`。

## T009 repair review pass (v11)

修复快照 `.codex-tmp/spec185-b5-t009-static-v11`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `35f610ab144392a39ce9284f2cbed599cee9650967ed5b2892fba0055230d4cc`；paths manifest SHA256 `cbdf116df46253988b66fe5e9c66854e5a565a952b59509f79fc20c654c63098`）经官方 `review-agent` 复审为 `STATIC_PASS`，无 P0/P1/P2/P3；本轮仍未构建或运行。

- `Runtime::drain` 和 `Runtime::drainAsync` 在调用 Provider 前于锁内执行 `Open -> Closing`；Provider 失败时不会错误发布 `Drained`。
- `ProductionAssemblerFetchesCanonicalSourceAfterSelection` 直接调用生产 `prepareNativeCanonicalOnnxRole`，获取 canonical root 与 source，执行真实 OA02 `runNativeOnnxAssemblyWorkerAt`，并断言 root/source 各抓取一次及 assembly metadata。
- 审查覆盖生产调用链、状态/所有权/并发、安全/兼容/构建接线和 C++ fixture/oracle 五 lane；端到端动态结果仍未观测。

T009 通过逐任务静态门，待 B5 组合审查及批末 compile-link/runtime-test；在此之前保持 `PARTIAL`。

## T010 static gate failure (v12)

任务快照 `.codex-tmp/spec185-b5-t010-static-v12`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `4e02e64aa105c6f0daad63d6569a77b6a0632c32655fa514d01f7f81e828157e`；paths manifest SHA256 `e3d201006b37df7bc37b5bc7a33282b5bb8ec832e421c6239e05a1227d8c4589`）经官方 `review-agent` 返回 `STATIC_FAIL`，未构建/未运行。发现：

- P1：creator waiter 的取消谓词直接取消共享 job，误伤仍有效的其他 waiter，违反“单 waiter 不能取消其他 waiter”。
- P1：Provider 缓存完整 request/grant projection，并在非 protected 路径缓存持久 plaintext runner path，越过 request-bound metadata 和 staging lease 边界。
- P2：lease 与 eviction 的异常安全记账不闭合；构建后的 cache key 未携带/校验 canonical source identity；缺少生产 cold/hit、并发 waiter、独立取消、revoke hot-cache 的 C++ oracle；内部 `ProviderArtifactCache.hpp` 被 broad DI header glob 安装。

T010 保持 `PARTIAL`；修复后必须重新冻结完整任务快照并复审，未进入构建。

## T010 static gate failure (v14)

任务快照 `.codex-tmp/spec185-b5-t010-static-v14`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `5e5360331bb3a4ccad5e7e45fae4c201b96a208266fdd93b1ea6778aba60c063`；paths manifest SHA256 `f5d3f026958f519cd91b5253901f4680743797bcfda46e2b59fa692adcb47e93`）经官方 `review-agent` 返回 `STATIC_FAIL`，未构建/未运行。审查覆盖五 lane，发现：

- P1：`Provider.cpp` 的 `protectedRuntime` 分支直接调用 assembler，未经过 `ProviderArtifactCache::acquireWithRunner`；受保护 immutable artifact/template 复用、独立 lease 与生产 cold/hit 计数尚未接线。
- P1：cache creator 的取消谓词只观察 shared job 状态；creator 是唯一 waiter 时不会注销并触发 last-waiter cancellation，仍会继续 assembly/publish。
- P1：`publish` 的 eviction、可抛对象复制和预算记账不在异常原子事务内，分配异常可留下已驱逐条目或错误 `chargedBytes`；rollback 本身也可能再次抛出。
- P2：`maxArtifactBytes` 只计最终 artifact 的 `ciphertextBytes`，未覆盖并发 assembly 的暂存目录、模型缓冲和 template metadata 的 admission/释放预算。

该结果只证明静态缺口，compile-link/runtime-test 尚未观察；T010 保持 `PARTIAL`。修复后必须重新冻结包含生产代码、fixture、构建注册和证据变更的不可变快照，再请求同范围复审。

## T010 static gate failure (v17)

任务快照 `.codex-tmp/spec185-b5-t010-static-v17`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `3c3c2f23bf05bcf4c67bb1ab3bc12831dcf301b2b85867b214693166c47a8955`；paths manifest SHA256 `fa22c87e954ee744cc74d578137b05f84308ce5ddb9eba58897443770c845325`）经官方 `review-agent` 返回 `STATIC_FAIL`，未构建/未运行。审查确认 protected ciphertext cache、每请求 ProtectedRuntime staging、预构造 Entry/node extraction rollback、assembly/template reservation 和 grant suffix 设计已接线，但仍发现：

- P1：已取消 job 仍可被新请求加入，且 creator 在取消后继续计入 waiter；last-waiter cancellation 线性化不完整。
- P1：admission 在已有可驱逐 LRU 条目时直接因 `chargedBytes > max-reservation` 拒绝，使 publish 的 LRU 驱逐不可达。
- P2：runner template 字节逐项计算仍可能无符号回绕。
- P2：cache hit 未按当前 projection 的 `maxAssembledBytes` 重新校验已缓存 artifact。
- P2：grant name/digest 只由 Provider helper 拼接，cache API 边界没有验证 `key.protectionIdentity` 与 projection 的 grant binding。
- 低概率异常边界：`stop() noexcept` 内构造 `exception_ptr` 可能在分配失败时终止。

此外，审查指出 v17 `changes.diff` 未包含未跟踪的 `tests/integration-tests/di-prepared-provider.t.cpp`，下一次快照必须显式纳入完整 fixture 内容。T010 保持 `PARTIAL`。

## T010 static gate failure (v18)

任务快照 `.codex-tmp/spec185-b5-t010-static-v18`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `5148f685d204488284dacd35059ceb1b57d582308a9f88cefc6612fc099d1570`；paths manifest SHA256 `fa22c87e954ee744cc74d578137b05f84308ce5ddb9eba58897443770c845325`）经官方 `review-agent` 返回 `STATIC_FAIL`，未构建/未运行。审查确认 v17 的溢出、命中上限、grant suffix、creatorActive 和锁内 LRU 修复；仍发现 cancelled flight 被移除后可能同 key 双重 assembly，LRU admission 删除后再分配 job 不具异常原子性，creator/stop waiter 错误边界不稳定；protected fixture 已纳入但仍未覆盖取消 flight、异常分配和精确压力边界。

## T010 static gate failure (v19)

任务快照 `.codex-tmp/spec185-b5-t010-static-v19`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `4f540954cfc7ce17d706b94ba2b4f36d71d54f31242492d83052b205aba35b94`；paths manifest SHA256 `fa22c87e954ee744cc74d578137b05f84308ce5ddb9eba58897443770c845325`）经官方 `review-agent` 返回 `STATIC_FAIL`，未构建/未运行。审查确认完整 fixture 和 protected cold/hit、ciphertext、grant/epoch 隔离已进入快照；仍有：admission LRU 驱逐不具异常原子性，cancelled flight 在旧 job 终态前直接阻断新请求，creator 仍可能覆盖 stop 的 `RUNTIME_CLOSED`。T010 保持 `PARTIAL`。

## T010 static gate failure (v20)

任务快照 `.codex-tmp/spec185-b5-t010-static-v20`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `401806b9cf727a100160f7d132c83a3cf8fc277626ed0490d8df0dff1addd374`；paths manifest SHA256 `fa22c87e954ee744cc74d578137b05f84308ce5ddb9eba58897443770c845325`）经官方 `review-agent` 返回 `STATIC_FAIL`，未构建/未运行。cancelled generation barrier、stop 错误优先级、grant/epoch 和 protected cache oracle 已闭合；唯一 P1 是 admission 在先删除部分 LRU 后发现剩余条目 pinned，异常路径未恢复已删除节点和 `chargedBytes`。T010 保持 `PARTIAL`。

## T010 static gate failure (v21)

任务快照 `.codex-tmp/spec185-b5-t010-static-v21`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `0860c03bde8083910f3d593eebce2a421ea1753014ef0bffe671d382b8fcaad6`；paths manifest SHA256 `fa22c87e954ee744cc74d578137b05f84308ce5ddb9eba58897443770c845325`）经官方 `review-agent` 返回 `STATIC_FAIL`，未构建/未运行。两阶段 victim 预选和异常原子性已成立，但循环以总 `chargedBytes` 而非实际 `chargedBytes - available` 为停止条件；混合 pinned/unpinned 条目即使可回收容量足够也会错误 `CACHE_BUDGET_EXCEEDED`。protected 默认 Provider 端到端仍为未观测项。T010 保持 `PARTIAL`。

## T010 static gate pass (v22)

任务快照 `.codex-tmp/spec185-b5-t010-static-v22`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `09a43f776dda24a165e8219f349647653fde8e37aa03da23c56561938bc6208a`；paths manifest SHA256 `fa22c87e954ee744cc74d578137b05f84308ce5ddb9eba58897443770c845325`）经官方 `review-agent` 返回 `STATIC_PASS`，无 P0/P1/P2/P3，未构建/未运行。审查确认：

- protected Provider 默认路径经过 `ProviderArtifactCache`，缓存只保存密文和 metadata-only runner template；每个请求以 `ProtectedRuntime` 独立解密到受保护 staging，明文 path 不进入 cache。
- single-flight cancelled generation 等待旧 job 终态，creator/其他 waiter 的取消互不误伤；stop 的 `RUNTIME_CLOSED` 优先级稳定。
- grant/provider/epoch exact-key、当前 assembled budget 重验、assembly 暂存与 template metadata 预算、两阶段 LRU victim 预选及异常原子性闭合。
- C++ fixture 覆盖 cache pin/eviction、protected ciphertext cold/hit、grant/epoch identity isolation；mixed pinned/unpinned budget 反例和默认 `Provider::serve` protected assembler→cache→decrypt/staging→runner 仍为 `unobserved`。

五 lane 静态覆盖 production entry/callers、implementation/state/concurrency、C++ fixture/oracle、build/source closure、migration/evidence；compile-link、runtime-test 和 sanitizer 仍未执行。T010 仅因静态门通过，保持 `PARTIAL`，须经 B5 组合门及批末验收后才能 `[x]`。

## B5 composition review

最终组合快照 `.codex-tmp/spec185-b5-composition-v1`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `af588dc33c64a393ceaf8ccdf098cdd81ac91160b7cc393298716a4f0483cfc5`；paths manifest SHA256 `bc8404f437f811040549920741059f90f4b7fca6eb89cb01871e005f498d5e59`）经官方 `review-agent` 返回 `B5_COMPOSITION_PASS`，无 P0/P1/P2/P3。审查确认 Runtime/Provider ingress、authenticated assembly、artifact cache、Face/stop/drain、single-flight/LRU/lease、grant/epoch identity、protected per-request staging 及 DI source closure 形成稳定批次边界。

批次增长决策：`STOP_GROWTH`；closure decision：`CLOSED_FOR_VALIDATION`。五 lane 均静态覆盖，默认 Provider→protected assembler→decrypt/staging→runner 的完整运行路径仍为 `unobserved`，compile-link/runtime-test/sanitizer 待批末验证；T009/T010 保持 `PARTIAL`。

## B5 normal compile boundary v1

批次组合门之后首次共享构建使用既有 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、系统优先 PATH 与 `-j4`，目标仅为 `spec185-provider-assembly`。构建于 2026-09-13 16:37 -05:00 返回 `rc=1`；原始输出见 `.codex-tmp/spec185-b5/normal-build-v1.log`，资源观测见 `.codex-tmp/spec185-b5/normal-build-v1.vmstat.log`，退出码见 `.codex-tmp/spec185-b5/normal-build-v1.rc`。

首个控制性边界是 C++ fixture 对 `RequestMessage::setPayload(ndn::Buffer&, size_t)` 传入临时 `ndn::Buffer`，编译器在 `tests/integration-tests/di-prepared-provider.t.cpp:658` 和 `:723` 报 non-const lvalue reference 绑定错误；生产 Provider/cache 源码尚未进入链接。已将两处改为具名可变 Buffer，T010 保持 `PARTIAL`，必须经 v23 静态复审后重建。

## T010 repair review pass (v23)

冻结快照 `.codex-tmp/spec185-b5-t010-static-v23`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `709c7450ae15e589cae36eb3afe952709307ed805572a11a0338e1ddd111d7e2`；paths manifest SHA256 `ef1bf777336013212b5b38da9bc80152bf4be45575a87da05d5cfabc9432da1c`）经官方 `review-agent` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。两处 fixture 已使用具名可变 `ndn::Buffer` 满足 `RequestMessage::setPayload(ndn::Buffer&, size_t)`；五 lane 复核无控制性缺陷。compile-link/runtime-test/sanitizer 仍未观测，T010 保持 `PARTIAL`，需重试 B5 共享构建。

## B5 normal compile boundary v2

v23 复审后重试同一 `-j4` 目标 `spec185-provider-assembly`，在生产源码编译阶段返回 `rc=1`。首个边界为 `ProviderArtifactCache.cpp:166` 的匿名 `makeLease` 无权调用 `ProviderArtifactLease` private constructor，第二个独立边界为 `Provider.cpp:1203` 缺少 `NativeRunnerPreparation.hpp` 声明；原始输出见 `.codex-tmp/spec185-b5/normal-build-v2.log`，退出码见 `.codex-tmp/spec185-b5/normal-build-v2.rc`，资源见 `.codex-tmp/spec185-b5/normal-build-v2.vmstat.log`。已将 lease 构造移入 `ProviderArtifactCache` 成员并补充 include，T010 仍为 `PARTIAL`，需新快照静态复审后重建。

## T010 repair review pass (v25)

冻结快照 `.codex-tmp/spec185-b5-t010-static-v25`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `6fbbcdbf2a1f9fed4681306b98b4d62dfacc66fc4f05ed5f81d92b4960867cd6`；paths manifest SHA256 `ef1bf777336013212b5b38da9bc80152bf4be45575a87da05d5cfabc9432da1c`）经官方 `review-agent` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。projection 五项 binding、worker 构建树候选路径和 maxArtifactEntries victim 预选修复均通过静态复核，未发现新控制性缺陷；compile-link/runtime-test/sanitizer 仍未观测，T010 保持 `PARTIAL`，可重试 B5 构建。

## T010 repair review pass (v24)

冻结快照 `.codex-tmp/spec185-b5-t010-static-v24`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `63b95b9c23dddea22ae1fae6ffa14d84ce22259bc3b9805650ff3ec16d50fca4`；paths manifest SHA256 `ef1bf777336013212b5b38da9bc80152bf4be45575a87da05d5cfabc9432da1c`）经官方 `review-agent` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。lease 构造权限、失效迭代器避免、runner preparation 声明及 v23 fixture 修复均闭合，五 lane 无新增控制性缺陷。compile-link/runtime-test/sanitizer 仍未观测，T010 保持 `PARTIAL`，可重试 B5 共享构建。

## B5 normal compile pass v3

v24 复审后在既有 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、系统优先 PATH 下以 `-j4` 构建目标 `spec185-provider-assembly` 成功，耗时 `59.408s`，返回 `rc=0`。候选二进制为 `build-spec185-b0c-normal/spec185-provider-assembly`，SHA256 `c70c7855a8d506c6376b9b03ac3eeb78a9070935566aedeb77fefb79bf512c79`；完整输出、退出码和资源记录分别见 `.codex-tmp/spec185-b5/normal-build-v3.log`、`.rc`、`.vmstat.log`。仅有既有第三方头/未使用值 warning，无编译或链接错误；下一步运行 B5 C++ selector。

## B5 normal compile pass v4

v25 复审后同一 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、`-j4` 目标 `spec185-provider-assembly` 成功，耗时 `18.793s`，返回 `rc=0`；候选 SHA256 `91c92e422b4870c039bcc0416114c0d5a8847875a6d39ee776d127ec750758d6`。完整输出、退出码和资源记录见 `.codex-tmp/spec185-b5/normal-build-v4.log`、`.rc`、`.vmstat.log`。生产与 fixture 已链接；唯一新增 warning 是 `ProviderArtifactCache.cpp:537` 未使用 `found`，将在下一修复门清理。

## B5 selector invocation boundary v1

首次 selector 枚举命令误传 `--list_content=tests`，Boost.Test 返回参数错误 `rc=200`，未执行任何用例；原始输出见 `.codex-tmp/spec185-b5/selector-list-v1.log`，退出码见 `.codex-tmp/spec185-b5/selector-list-v1.rc`。该记录属于测试入口参数边界，不计入产品行为结果；随后改用无值的 `--list_content`。

## B5 normal runtime boundary v1

候选 `build-spec185-b0c-normal/spec185-provider-assembly`（SHA256 `c70c7855a8d506c6376b9b03ac3eeb78a9070935566aedeb77fefb79bf512c79`）运行完整 `Spec185ProviderAssembly` selector 于 2026-09-13 16:53 -05:00 返回 `rc=201`：8 个用例中 3 通过、5 失败（54/59 assertions）。首个产品边界是真实 authenticated projection fixture 缺少完整 execution/dataflow/device binding，导致两项 `V3 Selection role/assembly/dataflow/device binding mismatch`；其次两个真实 assembler oracle 找不到同一构建树的 `DI_NativeOnnxAssemblyWorker`；cache pinned-entry 反例实际启动了第三次 build，断言期望 2、实为 3。完整原始输出和退出码见 `.codex-tmp/spec185-b5/normal-run-v1/output.log`、`rc`。已开始修复 fixture 绑定、worker 候选路径和 cache entry-admission，T009/T010 保持 `PARTIAL`，不能把其余 3 个通过用例外推为批次完成。

## T010 repair review pass (v26)

冻结快照 `.codex-tmp/spec185-b5-t010-static-v26`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `20a231c4fcb2419ba6b4834ee2c4a694401a8f27ee8b521159e56bf2fe7cd578`；paths manifest SHA256 `ef1bf777336013212b5b38da9bc80152bf4be45575a87da05d5cfabc9432da1c`）经官方 `review-agent` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。仅删除未使用局部变量，未改变 makeLease、锁、发布、reservation、异常或 lease 生命周期；五 lane 无回退。compile-link/runtime-test/sanitizer/压力并发仍未观测，T010 保持 `PARTIAL`，可重建后重跑 B5 selector。

## B5 normal compile pass v5

v26 复审后同一 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、`-j4` 目标 `spec185-provider-assembly` 成功，耗时 `9.594s`，返回 `rc=0`；候选 SHA256 `00b7cc48c9fa9830ab6755cf35cc4c23c32942f3ea8350a486488622444f36b9`。原始输出、退出码和资源见 `.codex-tmp/spec185-b5/normal-build-v5.log`、`.rc`、`.vmstat.log`；未见新增 warning。下一步运行完整 C++ selector。

## B5 normal runtime boundary v2

候选 `build-spec185-b0c-normal/spec185-provider-assembly`（SHA256 `00b7cc48c9fa9830ab6755cf35cc4c23c32942f3ea8350a486488622444f36b9`）第二次完整 `Spec185ProviderAssembly` selector 返回 `rc=201`：4/8 用例通过、4/8 失败（55/59 assertions）。cache pin/eviction 与 protected cache 已通过；剩余边界是两个 authenticated fixture 的 `onnxruntime` device binding 仍与 V3 校验不一致，以及两个 OA02 fixture 的伪 `recipeDigest` 被 worker 正确拒绝为 `DI_NATIVE_ONNX_RECIPE`。原始输出和退出码见 `.codex-tmp/spec185-b5/normal-run-v2/output.log`、`rc`。已修复设备绑定模式和 canonical recipe digest，T009/T010 保持 `PARTIAL`，待静态复审/重建/重跑。

## T010 repair review pass (v27)

冻结快照 `.codex-tmp/spec185-b5-t010-static-v27`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `c7e3dfa2ca8c0e68e481ad9a26af64e9adf303e3cc9a0a129d5224ab1b30e503`；paths manifest SHA256 `ef1bf777336013212b5b38da9bc80152bf4be45575a87da05d5cfabc9432da1c`）经官方 `review-agent` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。assembler fixture 已补 adapterVersion、canonical recipe digest 和 `SINGLE_DEVICE/cpu` binding；provider projection 同步满足 V3 parser，五 lane 无回退。compile-link/runtime-test/sanitizer/压力并发仍未观测，T010 保持 `PARTIAL`，可重建重跑。

## B5 normal compile pass v6

v27 复审后同一 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、`-j4` 目标 `spec185-provider-assembly` 成功，耗时 `21.696s`，返回 `rc=0`；候选 SHA256 `1792d5d2d212e0dbd43347cffe240546c7694584e395db1f199c5457676bf62a`。原始输出、退出码和资源见 `.codex-tmp/spec185-b5/normal-build-v6.log`、`.rc`、`.vmstat.log`；无新增 warning。下一步运行完整 C++ selector。

## B5 normal runtime boundary v3

候选 `build-spec185-b0c-normal/spec185-provider-assembly`（SHA256 `1792d5d2d212e0dbd43347cffe240546c7694584e395db1f199c5457676bf62a`）运行完整 `Spec185ProviderAssembly` selector 于 2026-09-13 17:18 -05:00 返回 `rc=201`：5/8 用例通过、3/8 失败（62/66 assertions）。Provider 配置、Provider-only drain、cache pin/eviction、protected cache 与 provider/epoch/grant substitution 已通过；首个动态边界是 authenticated Provider fixture 的 ACK candidate 集合为空（`candidates.size()==0`，随后 fixture abort），另外两个生产 assembler oracle 在 worker 子进程前以 `DI_NATIVE_ONNX_RECIPE` 拒绝 canonical recipe digest。完整原始输出和退出码见 `.codex-tmp/spec185-b5/normal-run-v3/output.log`、`.rc`。本次不计 T009/T010 完成；先核对 fixture 的真实 Selection/ACK 绑定和 canonical recipe digest 生成，再静态复审、重建和重跑。

## T010 repair review pass (v28)

冻结快照 `.codex-tmp/spec185-b5-t010-static-v28`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `18b7a227b8fc7c9da356f1b3fe4d70a7a61c2ffd7f8afb00dc2c857d549da71c`；paths manifest SHA256 `e4145af951e8a3b2078a50a3ec9a120fbcdad570146e019237abb9cbe53f15a5`）经官方 `review-agent` 返回 `T010_STATIC_PASS`，无 P0/P1/P2/P3。fixture 只由 `RequestService` response callback 设置完成状态，原始 publication 仍只处理 ACK，避免在 Provider counter 更新前提前结束 pump；正向 Selection→assignment→Provider→runner→Response、三类身份拒绝、counter/lifetime 断言及五 lane 无回退。compile-link、runtime-test、sanitizer 和压力并发未在静态门执行；T010 继续 `PARTIAL`，待批末验证。

## B5 normal compile pass v7

在既有 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、系统优先 PATH 下以 `-j4` 单独重建 worker target `di-native-assembly-worker` 成功，耗时约 `21.074s`，返回 `rc=0`；新的 `DI_NativeOnnxAssemblyWorker` 身份见 `.codex-tmp/spec185-b5/normal-build-v7.worker.sha256`，完整输出、退出码和资源见 `.codex-tmp/spec185-b5/normal-build-v7.log`、`.rc`、`.vmstat.log`。该重建修复了 selector 误用旧 worker 二进制造成的 `DI_NATIVE_ONNX_RECIPE` 边界；随后仍需运行修复后的 authenticated fixture。

## B5 normal runtime boundary v4

使用 v7 worker 与候选 `build-spec185-b0c-normal/spec185-provider-assembly` 运行完整 selector 于 2026-09-13 17:21 -05:00 返回 `rc=201`：7/8 用例通过，唯一失败为 authenticated fixture。正向 Selection/assignment/Response 已到达，但 fixture 在原始 response publication 出口早于 Provider counter 更新，`runs/assemblies/runnersCreated` 仍为 0；随后三种拒绝循环因异步 request-id/ACK 处理在最后一次出现空 candidate 并 abort。该边界由 v28 静态修复，原始输出和退出码见 `.codex-tmp/spec185-b5/normal-run-v4/output.log`、`.rc`；不计 T009/T010 完成。

## T010 repair review pass (v29)

冻结快照 `.codex-tmp/spec185-b5-t010-static-v29`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`；`changes.diff` SHA256 `93680dac4669fbcd88d9801b1131cddba1a17cbda34c3b646039b2d647caa24d`；paths manifest SHA256 `e4145af951e8a3b2078a50a3ec9a120fbcdad570146e019237abb9cbe53f15a5`）经官方 `review-agent` 返回 `T010_STATIC_PASS`，无 P0/P1/P2/P3。fixture 通过生产 `canonicalOnnxSourceIdentity` 派生 graph/initializer digest，并传入 assembler/cache projection，使 source identity 与 OA02 recipe 校验一致；v28 response callback completion 修复保持。五 lane 无回退；compile-link、runtime-test、sanitizer 和压力并发未在静态门执行。

## T010 static boundary v30

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v30` 返回 `STATIC_FAIL`。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `e4d6ef09f80c288e6ddb429219015816ff8de12277524693e62da3c2ac6eed7c`，paths SHA256 为 `e4145af951e8a3b2078a50a3ec9a120fbcdad570146e019237abb9cbe53f15a5`。首个控制性问题是 fixture 的异步 response/failure 状态仍存在未统一同步的跨线程读写；另一个是受控失败/空候选路径在 `pumpUntil` 返回前未等待请求终止，旧回调仍可能访问已重置的请求状态。五 lane 的其余覆盖、compile-link/runtime-test/sanitizer 结果均未形成 PASS；不得构建，T010 保持 `PARTIAL`。修复要求是每个请求使用独立共享状态并统一锁保护，且等待 terminal callback 或执行有屏障的取消/排空。

## T010 repair review pass v31

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v31` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `bcbd1b4fdcd0b75760219b6b5a1ec2f6e717c913171348086ab6b0823bb15eb2`，paths SHA256 为 `e4145af951e8a3b2078a50a3ec9a120fbcdad570146e019237abb9cbe53f15a5`。每个请求拥有独立 probe state，callback 和测试线程通过同一 mutex 访问；request ID、assignment projection 和 probe 按请求值捕获；负向路径等待 timeout/response terminal；异步 callback 不执行 Boost.Test 断言。五 lane 覆盖闭合，但 compile-link/runtime-test/sanitizer 仍未执行，T009/T010 保持 `PARTIAL`。

## B5 normal runtime boundary v6

候选 `build-spec185-b0c-normal/spec185-provider-assembly` SHA256 `a4dcee113eb57a4bd1bdd5101377eb9048c6fa1f13f4cfc2174e29fd879de1b8`，worker SHA256 `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e`；完整 `Spec185ProviderAssembly` selector 返回 `rc=201`，7/8 用例通过。唯一动态边界是 `AuthenticatedSelectionRunsProviderAssemblyRunnerAndResponse` 在 fixture 异步回调中的 `BOOST_REQUIRE_EQUAL` 触发 SIGSEGV（地址 `0x80`，最后 checkpoint `di-prepared-provider.t.cpp:668`）；其余 Provider、assembler、artifact cache、protected binding 用例通过。原始输出、退出码、候选和 worker 身份见 `.codex-tmp/spec185-b5/normal-run-v6/`。该结果不计 T009/T010 完成，需使用 v31 静态修复后重新 compile-link/runtime 验证。

## B5 normal compile pass v10

v31 静态复审后，在既有 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、系统优先 PATH 下以 `-j4` 仅构建 `spec185-provider-assembly` 成功，耗时约 `26.097s`，返回 `rc=0`。候选二进制 SHA256 为 `4261653e2829172da85a5b1dd1983b8753327ac7a2e9d745d6764e8f0589a878`；编译命令显示 fixture 重新编译并使用 `/usr/bin/g++ -B/usr/bin`。完整日志、退出码和资源记录见 `.codex-tmp/spec185-b5/normal-build-v10.log`、`.rc`、`.vmstat.log`。compile-link PASS；runtime-test/sanitizer 待执行。

## B5 normal runtime boundary v7

候选 `spec185-provider-assembly` SHA256 `4261653e2829172da85a5b1dd1983b8753327ac7a2e9d745d6764e8f0589a878`、worker SHA256 `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e` 的完整 selector 返回 `rc=201`，13 项断言失败。正向路径已发布 REQUEST 并看到 Selection，但 User PubSub fixture 未将 Response publication 转给 `handleDecryptedResponseByName`，因此 probe 进入 timeout、response 未见、assembly/runner counters 为 0；三个身份替换请求也未见 Selection/终态，测试最后在 `facade.drain(2000ms)` 失败并 SIGABRT。原始输出、退出码和候选身份见 `.codex-tmp/spec185-b5/normal-run-v7/`。该边界先修复 response ingress 及正向响应与 counters 的联合等待，再静态复审、重建和重跑；T009/T010 继续 `PARTIAL`。

## T010 static boundary v32

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v32` 返回 `STATIC_FAIL`。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `85ee49bd46b02a086a8fd5ccbccdbef9dfbce7fcfd00b4a47dc9ca58d1df47c4`，paths SHA256 以该快照 `paths.sha256` 为准。P1 是 fixture 将生产 SVS `HybridMessageEnvelope` 原始 wire 直接传给只接受已解密 `ResponseMessage` TLV 的 `handleDecryptedResponseByName`，绕过 `ServiceUser::OnResponse` 的 decryptHybridMessage 路径；因此新增 response oracle 无效。不得构建，T010 保持 `PARTIAL`。修复要求是复用现有生产 Response 解密入口，或先完成生产解密再调用 decrypted API。

## T010 repair review pass v34

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v34` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。快照 base 为 `775d0687d97eeb6c9f053d8d78c8c01b31864f91`，changes SHA256 为 `b3dfd8ca116daccd9dbe7f0b0208acaf03e75cef0b1114076d7929c0166ba978`，paths SHA256 为 `e4145af9511a3b2078a50a3ec9a120fbcdad570146e019237abb9cbe53f15a5`。仅新增同步测试线程的 failure 文本诊断；读取和写入均经 probe mutex，无异步 Boost.Test 断言，v33 的生产解密路径与 key setup 保持不变。五 lane 静态闭合；compile-link/runtime-test/sanitizer 待执行。

## T010 repair review pass v33

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v33` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。快照 base 为 `775d0687d97eeb6c9f053d8d78c8c01b31864f91`，changes SHA256 为 `662ec380015e18b2bebd65eb97956599f5cca7fc7ea55ee58ecda1aa7676511a`，paths SHA256 为 `e4145af9511a3b2078a50a3ec9a120fbcdad570146e019237abb9cbe53f15a5`。fixture 移除原始 HybridMessageEnvelope 到 decrypted API 的直传，改由生产 `ServiceUser::OnResponse`/`OnRequestAck` 处理；并预置 Provider ACK/RESPONSE 与 User SELECTION 的同源测试密钥，正向联合等待 response、terminal 和 assembly/runner counters，负向请求保持独立 request ID 与终态等待。五 lane 静态闭合；compile-link/runtime-test/sanitizer 仍未观测，T009/T010 保持 `PARTIAL`。

## B5 normal compile pass v11

v33 静态复审后，在既有 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、系统优先 PATH 下以 `-j4` 仅构建 `spec185-provider-assembly` 成功，耗时约 `19.738s`，返回 `rc=0`。候选二进制 SHA256 为 `c280d8104d060959ffbf98c72653c3eab246fe47b326ed1b7d6b35f25bbad0fb`；fixture 重新编译并完成链接，完整日志、退出码和资源记录见 `.codex-tmp/spec185-b5/normal-build-v11.log`、`.rc`、`.vmstat.log`。compile-link PASS；runtime-test/sanitizer 待执行。

## B5 normal runtime boundary v8

候选 `spec185-provider-assembly` SHA256 `c280d8104d060959ffbf98c72653c3eab246fe47b326ed1b7d6b35f25bbad0fb`、worker SHA256 `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e` 的完整 selector 返回 `rc=201`，13 项断言失败。v33 的同源 ACK/RESPONSE/SELECTION 测试密钥已预置，但正向 probe 仍未见成功 response、assembly 或 runner counter；三个负向请求也未见 Selection，测试末尾 `facade.drain(2000ms)` 失败并 SIGABRT。原始输出、退出码和候选身份见 `.codex-tmp/spec185-b5/normal-run-v8/`。当前仅能确认失败发生在 selection/response 之前，需先取得 probe failure 的同步诊断，再重新静态复审。

## B5 normal compile pass v12

v34 静态复审后，在既有 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、系统优先 PATH 下以 `-j4` 仅构建 `spec185-provider-assembly` 成功，耗时约 `20.603s`，返回 `rc=0`。候选二进制 SHA256 为 `a51d769e5fd224a0c54398b7324ba697b1a6f3d8d04439fd16370f64c7d99f35`；完整日志、退出码和资源记录见 `.codex-tmp/spec185-b5/normal-build-v12.log`、`.rc`、`.vmstat.log`。compile-link PASS；runtime-test 待执行。

## B5 normal runtime boundary v9

候选 `spec185-provider-assembly` SHA256 `a51d769e5fd224a0c54398b7324ba697b1a6f3d8d04439fd16370f64c7d99f35`、worker SHA256 `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e` 的完整 selector 返回 `rc=201`，13 项断言失败。同步诊断确认正向 Response callback 收到错误 `Provider lacks controller-authorized collaboration role /Backbone`；Provider admission 在 role permission 检查处拒绝，故 response/counters 未产生。三个负向请求随后同样无法形成 Selection/终态，最后 `facade.drain(2000ms)` 失败并 SIGABRT。原始输出、退出码和候选身份见 `.codex-tmp/spec185-b5/normal-run-v9/`。需在 fixture 中补齐 `/service/ROLE/Backbone` Provider permission 并保留服务级 permission，再静态复审、重建和重跑。

## T010 repair review pass v35

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v35` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `85d1aaa312d42d65afbd927a04ff5b3d10bac3e69d02de8d879fd09f76bd4cc2`，paths SHA256 为 `e4145af951e8a3b2078a50a3ec9a120fbcdad570146e019237abb9cbe53f15a5`。fixture 保留服务级 Provider permission 并增加与生产构造规则一致的 `/Inference/Spec185ProviderOracle/ROLE/Backbone` role permission，覆盖 Provider authenticated Selection admission；未放宽生产检查，五 lane 静态闭合。compile-link/runtime-test/sanitizer 待执行。

## B5 normal compile pass v13

v35 静态复审后，在既有 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、系统优先 PATH 下以 `-j4` 仅构建 `spec185-provider-assembly` 成功，耗时约 `20.028s`（Waf 内部 `19.815s`），返回 `rc=0`。候选二进制 SHA256 为 `b4af4a1c50d18a5f5ba9f008416d3501b53673293afafc8266e0c226f2219d2f`；完整日志、退出码、资源记录和候选身份见 `.codex-tmp/spec185-b5/normal-build-v13.log`、`.rc`、`.vmstat.log`、`.binary.sha256`。compile-link PASS；runtime-test 待执行。

## B5 normal runtime boundary v10

候选 `spec185-provider-assembly` SHA256 `b4af4a1c50d18a5f5ba9f008416d3501b53673293afafc8266e0c226f2219d2f`、worker SHA256 见 `.codex-tmp/spec185-b5/normal-run-v10/worker.sha256` 的完整 selector 返回 `rc=201`，8 个用例中 7 个完成，`AuthenticatedSelectionRunsProviderAssemblyRunnerAndResponse` 失败并触发 `facade.drain` 后 SIGABRT。新增 role permission 在 bootstrap 后通过 `applyPermissionResponse` 替换 Provider permission 表并触发权限刷新副作用，正向请求未见 Selection/Response/assembly/runner，三个拒绝循环也未见 Selection；原始输出、退出码及候选身份见 `.codex-tmp/spec185-b5/normal-run-v10/`。compile-link 仍有效，runtime-test 未通过；应把 role grant 纳入 fixture bootstrap permission wave，避免运行中替换授权表。

## T010 repair review pass v36

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v36` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `b42967c6cbffd8b814e3bf84ddd70dcfe7075e449fc1e895d8c715ec29846130`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。`providerRoles` 默认空并在初始 bootstrap ProviderPermission wave 生成精确 `/service/ROLE/Backbone` grant；Spec185 fixture 不再在 bootstrap 后替换权限表。五 lane 静态闭合；compile-link/runtime-test/sanitizer 待执行。

## B5 normal compile pass v14

v36 静态复审后，在既有 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、系统优先 PATH 下以 `-j4` 仅构建 `spec185-provider-assembly` 成功，耗时约 `21.350s`（Waf 内部 `21.189s`），返回 `rc=0`。候选二进制 SHA256 为 `7f22f35a282f8e624f6a7630e992807a5333e812d636928c102110ec8ffd5f09`；完整日志、退出码、资源记录和候选身份见 `.codex-tmp/spec185-b5/normal-build-v14.log`、`.rc`、`.vmstat.log`、`.binary.sha256`。compile-link PASS；runtime-test 待执行。

## B5 normal runtime boundary v11

候选 `spec185-provider-assembly` SHA256 `7f22f35a282f8e624f6a7630e992807a5333e812d636928c102110ec8ffd5f09`、worker SHA256 见 `.codex-tmp/spec185-b5/normal-run-v11/worker.sha256` 的完整 selector 返回 `rc=201`，7/8 用例通过；`AuthenticatedSelectionRunsProviderAssemblyRunnerAndResponse` 正向请求未见 Selection/Response/assembly/runner，三个拒绝循环同样未见 Selection，最后 `facade.drain(2000ms)` 失败并 SIGABRT。v36 已将 role grant 纳入 bootstrap wave，但本次仍未闭合请求 ingress；原始输出、退出码和候选身份见 `.codex-tmp/spec185-b5/normal-run-v11/`。T009/T010 保持 `PARTIAL`，需继续定位 ACK/Selection 边界。

## B5 diagnostic runtime boundary v12

在不改变候选源码的条件下，以 `NDN_LOG=ndn_service_framework.ServiceProvider=TRACE:ndn_service_framework.ServiceUser=TRACE:ndn_svs.SVSPubSub=TRACE` 运行正向 selector，候选 SHA256 `7f22f35a282f8e624f6a7630e992807a5333e812d636928c102110ec8ffd5f09` 返回 `rc=201`。日志确认 REQUEST、ACK、Selection、Provider execution 均已进入生产路径；首个失败为 `validateNativePreparedRunnerSpec` 返回 `DI_PROVIDER_ASSEMBLY_PATH_UNSAFE`，因为 fixture runner spec 使用 `oracle.onnx` 而契约要求绝对路径文件名 `model.onnx`。Selection status 随后为 `Failed`，未发生 assembly/runner/Response；原始输出、退出码及候选身份见 `.codex-tmp/spec185-b5/diagnostic-run-v12/`。该诊断不计 runtime PASS，需补齐合法路径和 assembly identity metadata 后重新静态审查。

## T010 static boundary v37

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v37` 返回 `STATIC_FAIL`。控制性 P1 是 preparationFactory 删除了 `provider`、`boot`、`plan`、`artifact` metadata，但同一 fixture 的 `makeProviderOracleRunnerFactory` 仍通过 `spec.metadata.at(...)` 读取这些字段；test seam 未经过生产绑定 helper 自动补字段，`runnerFactory->create(spec)` 会抛 `std::out_of_range`，正向 assembly/runner/Response 无法发生。v37 未构建/运行；路径及完整 assembly identity metadata 已通过静态检查，恢复四个字段后需重新冻结复审。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `551f5fc99078f8b2c0d624d5cc64f2828d63cffb9e534d34384648aa1a398884`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。

## T010 repair review pass v38

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v38` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `a5b997272d954b3a403b106562185761da5003e55785046c82f54ab4754c13b7`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。runner preparation seam 恢复 `provider/boot/plan/artifact` 四项 metadata，同时保留合法 `model.onnx` 路径与完整 projection assembly identity；五 lane 静态闭合。compile-link/runtime-test/sanitizer 待执行。

## B5 normal compile pass v15

v38 静态复审后，在既有 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、系统优先 PATH 下以 `-j4` 仅构建 `spec185-provider-assembly` 成功；Waf 日志显示耗时 `38.016s`，返回 `rc=0`。候选二进制 SHA256 为 `7f94e4505797f9f04fc704f874cc0da1bc1ff59cbbc84f9bb7f6b451651fd0ee`；完整日志、退出码、资源记录和候选身份见 `.codex-tmp/spec185-b5/normal-build-v15.log`、`.rc`、`.vmstat.log`、`.binary.sha256`。compile-link PASS；runtime-test 待执行。

## B5 normal runtime boundary v12

候选 `spec185-provider-assembly` SHA256 `7f94e4505797f9f04fc704f874cc0da1bc1ff59cbbc84f9bb7f6b451651fd0ee`、worker SHA256 见 `.codex-tmp/spec185-b5/normal-run-v12/worker.sha256` 的完整 selector 返回 `rc=201`，7/8 用例通过；正向 ACK/Selection/Response 已形成，但 `assemblies` 计数仍为 0，三个身份替换负向用例未见 Selection，末尾 `facade.drain(2000ms)` 失败并 SIGABRT。原始输出、退出码和候选身份见 `.codex-tmp/spec185-b5/normal-run-v12/`。compile-link 有效，runtime-test 未通过；需 TRACE 定位 assembly 失败和负向请求边界。
## B5 diagnostic runtime boundary v13

在不改变候选源码的条件下，以 `NDN_LOG=ndn_service_framework.ServiceProvider=TRACE:ndn_service_framework.ServiceUser=TRACE:ndn_svs.SVSPubSub=TRACE` 运行 v15 候选，候选 SHA256 为 `7f94e4505797f9f04fc704f874cc0da1bc1ff59cbbc84f9bb7f6b451651fd0ee`，worker SHA256 为 `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e`，返回 `rc=201`。TRACE 确认正向 REQUEST、ACK、Selection、Provider execution、Response publication、User 解密和 callback 均已闭合；正向唯一失败是测试 `ProviderCounters.assemblies` 仍为 `0`，因为 test preparation seam 覆盖了生产 preparation wrapper，未进入生产 metrics 计数点。三个负向请求的首个失败均在 Provider REQUEST admission：请求 ID 使用 `/spec185-provider-reject-/N` 被解析为 service `/Inference/Spec185ProviderOracle/spec185-provider-reject-`、requestId `/N`，因此在服务级 permission 检查处被拒绝，未进入 Selection。原始日志、退出码和候选身份见 `.codex-tmp/spec185-b5/diagnostic-run-v13/`。该诊断不计 runtime PASS；下一步在 test seam 保持生产计数语义，并让每个负向 ID 使用单一 NDN name component，随后重新静态审查、构建和运行。

## T010 repair review pass v39

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v39` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `2633d87dfd3c0200808183ec2f30d344a73e9b90cab2ec8bfa337b33799d7d0d`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。审查确认 test preparation seam 仅在成功返回 spec 后递增 `ProviderMetrics::assemblies`，失败路径不会虚增；负向 request ID 使用单一 NDN component；v38 的完整 seam/assembly identity metadata、`model.onnx` 路径和 provider validator 约束均未回退。五 lane 全部 covered。未执行 compile-link/runtime-test/sanitizer；T009/T010 继续 `PARTIAL`。
## B5 normal compile pass v16

v39 静态复审后，在既有 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、系统优先 PATH 下以 `-j4` 仅构建 `spec185-provider-assembly` 成功，耗时 `21.664s`，返回 `rc=0`。候选 SHA256 为 `a111e193ef01c07793089e86c284e60611f2f236a01d5c96c6b2d699be20ae6e`；完整日志、退出码、资源记录和候选身份见 `.codex-tmp/spec185-b5/normal-build-v16/output.log`、`.rc`、`.vmstat.log`、`.binary.sha256`。compile-link PASS；runtime-test/sanitizer 待执行。

## B5 normal runtime boundary v13

v16 候选完整 `Spec185ProviderAssembly` selector 返回 `rc=201`，`105/107` assertions 通过。正向 REQUEST、ACK、Selection、Provider assembly seam、runner、Response publication、User 解密及三类 provider/epoch/grant substitution rejection 均通过；唯一失败为 `facade.drain(2000ms)` 返回 false，随后测试 abort。候选 SHA256 为 `a111e193ef01c07793089e86c284e60611f2f236a01d5c96c6b2d699be20ae6e`，worker SHA256 为 `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e`；原始输出、退出码和身份见 `.codex-tmp/spec185-b5/normal-run-v13/`。本结果不计 runtime PASS；待修正 fixture 的 stop/drain 调用出口后重新静态审查、构建和运行。

## T010 repair review pass v40

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v40` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `19578fd3dd2d237c3e409bf7671d758223b2cad384f8d41b6e0ee60736a00a54`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。审查确认 `registration.close()` → `facade.stop()` → `facade.drain(2000ms)` 先升 Provider stopped fence，再等待 borrowed Face 的 IO barrier/join；registration、NativeInferenceProvider 和 cache 的重复关闭保持幂等，`drain` 仍传播失败结果。五 lane 全部 covered；未执行 compile-link/runtime-test/sanitizer，T009/T010 继续 `PARTIAL`。

## B5 normal compile pass v17

v40 静态复审后，在既有 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、系统优先 PATH 下以 `-j4` 仅构建 `spec185-provider-assembly` 成功，耗时 `22.994s`，返回 `rc=0`。候选 SHA256 为 `8683a0902628355bb261b6923bd091a9146a80c8cf31d58c29f50b11e779bde8`；完整日志、退出码、资源记录和候选身份见 `.codex-tmp/spec185-b5/normal-build-v17/output.log`、`.rc`、`.vmstat.log`、`.binary.sha256`。compile-link PASS；runtime-test/sanitizer 待执行。
## B5 normal runtime pass v14 (first repeat)

v17 候选完整 `Spec185ProviderAssembly` selector 首次修复后运行返回 `rc=0`，8/8 test cases、106/106 assertions 通过；正向 authenticated assembly/runner/Response、三类 provider/epoch/grant rejection、Provider-only lifecycle、canonical assembler/source fetch、cold/hit cache、protected binding substitution 和 stop→drain 均通过。候选 SHA256 为 `8683a0902628355bb261b6923bd091a9146a80c8cf31d58c29f50b11e779bde8`，worker SHA256 为 `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e`；原始输出、退出码和身份见 `.codex-tmp/spec185-b5/normal-run-v14/`。按 C-04 每 case 两次，尚需独立 sanitizer。

## B5 normal runtime pass v15 (second repeat)

使用与 v14 完全相同的 `spec185-provider-assembly` 候选和 worker，第二次完整 `Spec185ProviderAssembly` selector 返回 `rc=0`，8/8 test cases、106/106 assertions 通过。正向 authenticated assembly/runner/Response、三类 provider/epoch/grant rejection、Provider-only lifecycle、canonical assembler/source fetch、cold/hit cache、protected binding substitution 和 stop→drain 均再次通过。候选 SHA256 仍为 `8683a0902628355bb261b6923bd091a9146a80c8cf31d58c29f50b11e779bde8`，worker SHA256 仍为 `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e`；原始输出、退出码和身份见 `.codex-tmp/spec185-b5/normal-run-v15/`。C-04 normal repeat lane 已闭合；独立 sanitizer 及批次组合收口仍待完成，T009/T010 保持 `PARTIAL`。

## B5 normal compile pass v18

v44 静态复审和 B5 composition v3 通过后，在既有 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、系统优先 PATH 下以 `-j4` 仅构建 `spec185-provider-assembly` 成功，耗时约 `45.925s`，返回 `rc=0`。候选 SHA256 为 `dbdd7117fb2756049e35768c5535e797d7edb60563a69ede5712729bfe80a53b`；完整日志、退出码、资源记录和候选身份见 `.codex-tmp/spec185-b5/normal-build-v18/output.log`、`.rc`、`.vmstat.log`、`.binary.sha256`。compile-link PASS；runtime-test 已完成两次，独立 sanitizer 待执行，T009/T010 保持 `PARTIAL`。

## B5 normal runtime pass v16 (first repeat after v44)

v18 候选完整 `Spec185ProviderAssembly` selector 返回 `rc=0`；输出报告 `Running 8 test cases` 与 `*** No errors detected`，所有 selector 用例通过。正向 authenticated assembly/runner/Response、三类 provider/epoch/grant rejection、Provider-only lifecycle、canonical assembler/source fetch、cold/hit cache、protected binding substitution 和 stop→drain 均通过。候选 SHA256 为 `dbdd7117fb2756049e35768c5535e797d7edb60563a69ede5712729bfe80a53b`，worker SHA256 为 `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e`；原始输出、退出码和身份见 `.codex-tmp/spec185-b5/normal-run-v16/`。按 C-04 仍需同候选第二次运行及独立 sanitizer，T009/T010 保持 `PARTIAL`。

## B5 normal runtime pass v17 (second repeat after v44)

使用与 v16 完全相同的 v18 候选和 worker，第二次完整 `Spec185ProviderAssembly` selector 返回 `rc=0`；输出再次报告 `Running 8 test cases` 与 `*** No errors detected`，所有 selector 用例通过。正向 assembly/runner/Response、三类身份拒绝、canonical source、cache cold/hit、protected binding substitution 与 stop→drain 均再次通过。候选 SHA256 仍为 `dbdd7117fb2756049e35768c5535e797d7edb60563a69ede5712729bfe80a53b`，worker SHA256 仍为 `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e`；原始输出和身份见 `.codex-tmp/spec185-b5/normal-run-v17/`。C-04 normal repeat lane 已闭合；独立 sanitizer 与 B5 批次组合收口仍待执行，T009/T010 保持 `PARTIAL`。

## B5 sanitizer runtime boundary r1

独立 `build-spec185-b3-asan-ubsan-fast` 配置以 `-j4` 重建 `spec185-provider-assembly` 成功，候选 SHA256 为 `fef9915218f1e355eb8494ca44d4472990ffa2241b443aebdf5edda4158b70d3`；随后以严格 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` 运行完整 selector。业务断言先到 `*** No errors detected`，但退出阶段 `LeakSanitizer` 报告 27 个间接泄漏、4518 bytes，首个保留栈位于 Provider-only `Runtime::open` 构造触发的 NAC-ABE `SegmentFetcher`/Face 资源；selector `rc=134`，因此不计 sanitizer PASS。单独 Provider-only selector 重现同一边界，原始输出、退出码、候选和 worker 身份见 `.codex-tmp/spec185-b5/asan-build-v1/`、`.codex-tmp/spec185-b5/asan-run-r1/`、`.codex-tmp/spec185-b5/asan-run-r1-provider-only/`。已定位为 Provider stop 后 Face-bound `ServiceProvider`/native host 释放时序，待 T010 v41 静态复审后重建；T009/T010 保持 `PARTIAL`。

## T010 static boundary v41

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v41` 返回 `STATIC_FAIL`。控制性 P1 是新增 `releaseStoppedResources()` 虽在 `State::mutex` 下移动 `nativeHost`/`serviceProvider`，但 `serve()`、`stop()`、`drain()` 仍有锁外 shared_ptr 成员读写；Provider 可复制时，关闭线程的 reaper release 可能与另一线程读取成员形成数据竞争或空对象/UAF。未构建/未运行；快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `77279e336168c11e0d975277b18ddde609e26cb773d6c8fb0409b96bbae4a1d5`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。已要求统一状态锁快照并等待 active serve 退出，T009/T010 保持 `PARTIAL`。

## T010 repair review pending v42

v41 P1 修复后冻结 `.codex-tmp/spec185-b5-t010-static-v42`，base 仍为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `a34d571969369c93d1034519149132e657baa6a67b887139aa5ec2df91ec542e`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`；已重新派发官方只读静态复审，未构建/运行，T009/T010 保持 `PARTIAL`。

## T010 static boundary v42

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v42` 返回 `STATIC_FAIL`。控制性 P1 是 `requestStopIo()` 的无 IO 线程分支在持有 `ioMutex` 时调用 `releaseStoppedResources()`，而并发 `serve()` 可持有 `serveMutex` 后等待 `ioMutex`，形成 `ioMutex`→`serveMutex` 与 `serveMutex`→`ioMutex` 的循环等待。未构建/未运行；快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `a34d571969369c93d1034519149132e657baa6a67b887139aa5ec2df91ec542e`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。已将释放调用移出 `ioMutex` 临界区并待 v43 复审，T009/T010 保持 `PARTIAL`。

## T010 repair review pending v43

v42 P1 修复后冻结 `.codex-tmp/spec185-b5-t010-static-v43`，base 仍为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `298a423a68526720c76507fa6af111e804f76f8742901d83d24904f32121d375`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`；已重新派发官方只读静态复审，未构建/运行，T009/T010 保持 `PARTIAL`。

## T010 static boundary v43

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v43` 返回 `STATIC_FAIL`。控制性 P1 是异步 Face 回调的 `invokeNativeServe` 以引用捕获 `definition`、`nativeConfig` 和 `this`；stop 可在回调完成前唤醒 waiter，造成栈对象悬空。另有 P2：self-thread `stopIo` 的 detached reaper join 后只设置 `ioStopped`，未调用 `releaseStoppedResources()`，可能长期保留 Face-bound owners。未构建/未运行；快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `298a423a68526720c76507fa6af111e804f76f8742901d83d24904f32121d375`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。已改为共享拥有型 serve payload，并让 self-thread reaper 释放 owners，待 v44 复审，T009/T010 保持 `PARTIAL`。

## T010 repair review pending v44

v43 P1/P2 修复后冻结 `.codex-tmp/spec185-b5-t010-static-v44`，base 仍为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `f408627be979ebab47410e9684273d3d15d1e96e5cdd0f4fb73aaf3b1f1d4b94`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`；已重新派发官方只读静态复审，未构建/运行，T009/T010 保持 `PARTIAL`。

## T010 repair review pass v44

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v44` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `f408627be979ebab47410e9684273d3d15d1e96e5cdd0f4fb73aaf3b1f1d4b94`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。审查确认 `ServeInvocation` 完整拥有 State/definition/config，queued callback 无悬空引用；self-thread reaper 和无线程分支均在 IO join/解锁后释放 owners，`serveMutex`/`State::mutex`/`ioMutex` 无反向锁序，`resourcesReleased` 幂等且 borrowed test seam 生命周期保持。待修复候选的 normal/sanitizer compile-link 与 runtime，T009/T010 保持 `PARTIAL`。

## B5 composition review pass v3

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-composition-v3` 返回 `B5_COMPOSITION_PASS`，无控制性缺陷。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `f408627be979ebab47410e9684273d3d15d1e96e5cdd0f4fb73aaf3b1f1d4b94`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。五 lane 覆盖 Provider→Selection→preparation/runner→Response、cache/lease/grant/epoch/protected staging、ServeInvocation/stop/drain/reaper 锁序、fixture/oracle/build closure 和 evidence/compatibility；Batch growth=`STOP_GROWTH`，static closure=`CLOSED_FOR_VALIDATION`。本门未新增动态结论，v44 后 sanitizer/LSan 重跑、压力竞态及默认受保护 Provider 端到端路径仍未观察，T009/T010 保持 `PARTIAL`。
## T010 repair review pass v58

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v58` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `96bac0ce661a17040eef74f7f567623c561f1cade663ca7ad62051d13cd93f8a`，review paths SHA256 为 `16a571c552cbcfc6e243c9ec1be67a898d44d6daa2a246e052cc66cb13ae2bad`。复审确认 reaper target 在启动前预分配、Unrecovered 仅做 noexcept move 且线程入口有异常边界；同步 fallback、`stopIo()`、`requestStopIo()` 在 `!ioRunning` 时发布 `ioStopped`/`ioStopping`/`ioWork` 终态并清除 deferred cleanup；`reaperOwner` 使用 `weak_ptr`，`orphanWorker` 保留未回收 target，owned/borrowed Face shutdown 语义和 T009 ServeInvocation 接线保持。五 lane 静态覆盖；compile-link/runtime/sanitizer 与 join/detach/分配异常注入仍未观察，T009/T010 保持 `PARTIAL`，待 B5 最终组合门。

## B5 composition review pass v4

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-composition-v4` 返回 `B5_COMPOSITION_PASS`，无 P0/P1/P2/P3。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `da6bb83c34146e49d1bd9f940888ef78e6a8b7c1c1aa3d4baa1b94d4cf605221`，paths SHA256 为 `f7b75521abb26897614ecc0cb794d952f2aeadbe01351be5d9f9910ee229315c`。组合审查确认 Provider ingress→ACK/Selection→canonical/protected preparation→cache/lease→runner→Response，single-flight/LRU/grant/epoch/protected staging，ServeInvocation、owned/borrowed Face、stop/drain/reaper 终态和锁序，以及 C++ fixture/oracle/build/source closure 均静态闭合。五 lane 已覆盖；完整生产 assembler 动态组合、compile-link、runtime-test 和 sanitizer qualification 仍未观察。Batch decision=`STOP_GROWTH / CLOSED_FOR_VALIDATION`，仅表示静态组合门通过，不计运行资格；T009/T010 保持 `PARTIAL`，进入批末验证。

## B5 normal compile boundary v19

组合 v4 静态门后，使用既有 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、系统优先 PATH、`CC=/usr/bin/gcc`、`CXX=/usr/bin/g++` 和 `-j4` 仅构建 `spec185-provider-assembly`，于 2026-09-13 21:35 -05:00 返回 `rc=1`。首个控制性边界为 `ProviderReaperOwner::~ProviderReaperOwner()` 的 `finish(*target)` 类型错误：`finish` 要求 `std::unique_ptr<std::thread>&`，而 `*target` 是 `std::thread`；生产/fixture 尚未进入链接。原始输出、退出码和资源记录见 `.codex-tmp/spec185-b5/normal-build-v19/output.log`、`.rc`、`.vmstat.log`。已修正为 `finish(target)`；T009/T010 保持 `PARTIAL`，需受影响静态复审后重建。

## T010 repair review pass v59

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v59` 返回 `STATIC_PASS`，并确认 B5 组合无回归，无 P0/P1/P2/P3。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `ee947bf824280faddb72b997173e0c65aa8dec6009bce7d5bc61663b0b48931b`，review paths SHA256 为 `e164c9b92ec65dd3708dfaf36909df97f5c5a93b833f7b877f943a589bfdabd5`。`ProviderReaperOwner::~ProviderReaperOwner()` 已按 `finish(std::unique_ptr<std::thread>&)` 契约调用 `finish(target)`；owner、异常、终态和组合五 lane 静态覆盖。compile-link/runtime/sanitizer 未在本门执行；T009/T010 保持 `PARTIAL`，允许重建。

## T010 repair review boundaries v60-v62

官方 `review-agent` 对 v60（changes `4ad8d34da882caff2dc40286a036e28273b54efaff1dd1b101be58178181ddd7`）发现 P1：borrowed Face worker 在 stopped context 退出后未发布 `ioStopped/ioFailed`，后续 `serve()` 可永久等待。v61（changes `8722f0949cb86dd5c33fb69212b0f60facec6cb560f7dc654b56477e46a3bc01`）修复后，v62（changes `5b46bc41d30cb9d113c015fdd7fab52ae29170b73ef1b809f7601c830de3e6eb`）复审再次发现 P1：stop marker 投递后 context 才 stopped 时，worker 继续等待而 `stopIo()` 可能无界 join。三次均未构建/运行，不能计为通过；paths SHA256 均为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。

## T010 repair review pass v63

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v63` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `f841d07333edb6cd034d2d0d38d3fa6167571edbdebd7a961fcb0ae99b5513ba`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。borrowed stopped context 在 `ioStopping` 下直接设置 `ioStopRequested` 并退出，serve 对 stopped borrowed context fail-closed；五 lane 静态闭合。compile-link/runtime/sanitizer 待执行，T009/T010 保持 `PARTIAL`。

## T010 repair review boundaries v60-v62

官方 `review-agent` 对 v60（changes `4ad8d34da882caff2dc40286a036e28273b54efaff1dd1b101be58178181ddd7`）发现 P1：borrowed Face worker 在 stopped context 退出后未发布 `ioStopped/ioFailed`，后续 `serve()` 可永久等待。v61（changes `8722f0949cb86dd5c33fb69212b0f60facec6cb560f7dc654b56477e46a3bc01`）修复后，v62（changes `5b46bc41d30cb9d113c015fdd7fab52ae29170b73ef1b809f7601c830de3e6eb`）复审再次发现 P1：stop marker 投递后 context 才 stopped 时，worker 继续等待而 `stopIo()` 可能无界 join。三次均未构建/运行，不能计为通过；paths SHA256 均为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。

## T010 repair review pass v63

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v63` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `f841d07333edb6cd034d2d0d38d3fa6167571edbdebd7a961fcb0ae99b5513ba`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。borrowed stopped context 在 `ioStopping` 下直接设置 `ioStopRequested` 并退出，serve 对 stopped borrowed context fail-closed；五 lane 静态闭合。compile-link/runtime/sanitizer 待执行，T009/T010 保持 `PARTIAL`。

## B5 normal compile pass v21

v63 静态门后，使用既有 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、系统优先 PATH、`CC=/usr/bin/gcc`、`CXX=/usr/bin/g++` 和 `-j4` 仅构建 `spec185-provider-assembly`，于 2026-09-14 返回 `rc=0`，耗时 21.573 秒。候选二进制 SHA256 为 `67ebec894a8320fec491e2891b6f23dab6209910b7546308cf61c01d14520f9e`，worker SHA256 为 `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e`。原始输出及资源记录见 `.codex-tmp/spec185-b5/normal-build-v21/`。

## B5 normal runtime boundaries v19-v20

同一 v21 候选运行完整 `Spec185ProviderAssembly` 两次均未形成资格通过：v19 在 authenticated Provider 用例触发 `double free or corruption (out)` / `malloc(): invalid size (unsorted)`，返回 `rc=134`；v20 在同一用例触发内存访问权限错误，返回 `rc=201`。gdb v22 的首个信号位于主线程 `ndn::Buffer` shared_ptr 引用计数路径，调用栈经过 `ndn::svs::SVSPubSub` 回调；Provider borrowed Face worker 同时泵共享 `io_context`。当前只能确认动态内存/并发边界，尚未归因或修复；T009/T010 保持 `PARTIAL`，不得重跑完整资格直到 sanitizer 或最小化诊断明确首个生产边界。原始日志见 `.codex-tmp/spec185-b5/normal-run-v19/`、`normal-run-v20/` 和 `.codex-tmp/spec185-b5/diagnose-v22/gdb-auth.log`。

## B5 sanitizer diagnostic boundary v23

在 v63 静态门后，使用系统优先 PATH 和 `-j4` 重建既有 `build-spec185-b3-asan-ubsan-fast` 的 `spec185-provider-assembly`，Waf 返回 `rc=0`，耗时 58.135 秒。严格 ASan/UBSan 运行 authenticated selector 返回 `rc=1`；ASan 首个报告为主线程在 `ndn::svs::Fetcher::onData` 复制 `Fetcher::QueuedInterest` 时读取已由 Provider worker 线程释放的 `std::function`（heap-use-after-free）。调用链为 `DummyClientFace::receive` → `SVS Fetcher`，并行线程由 `Provider::startIo()` 在 borrowed Face 共享 `io_context` 上执行 `run_one_for()`。原始报告见 `.codex-tmp/spec185-b5/asan-auth-v1/output.log`；gdb 保留完整双线程分配/释放栈于 `.codex-tmp/spec185-b5/diagnose-v23/gdb-asan-auth.log`。该诊断把首个边界归因到 fixture/borrowed Face 的并发事件循环接线，尚未修复或复测，T009/T010 继续 `PARTIAL`。

## T010 static boundary v64

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v64` 返回 `STATIC_FAIL`。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `c27660baa2dff9fa6ead55ce8df2e4187f0d5a11a8d4abc68a726376b15ed9ba`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。

- P1：`bootstrap()` 在 native Provider worker 启动前依据 `providerFacesHaveDedicatedIoWorkers` 跳过 Provider Face 泵送，deferred NAC/AA Data 可能永远不被处理。应保持 bootstrap 始终泵送，只有 `serve()` 启动 worker 后才进入 dedicated 路径。
- P1：deferred bridge 下 `m_activeFaults`、`m_bridgeStats` 和 pending packet optionals 会由 User Face 与 Provider worker 并发读写；仅 AA 计数器使用 atomic 不足。应以状态锁保护快照/事务，释放锁后再调用目标 Face，`bridgeStats()` 返回同步快照。

未构建或运行；T009/T010 保持 `PARTIAL`，修复后需 v65 受影响复审。

## T010 static boundary v65

官方 `review-agent` 对 v65 冻结快照返回 `STATIC_FAIL`。v64 的两个 P1 已闭合：bootstrap 始终泵送 Provider Face，deferred bridge 状态由 `m_bridgeMutex` 保护且 Face 投递在锁外。新增 P2：`forwardInterest()` 在 `duplicatePackets=true` 时 `forwardedInterests` 少记实际重复投递；`reorderPackets` 与 duplicate 组合时遗漏 `duplicatedPackets`；stream-interest drop 的限额先解锁读取再递增，多个线程可能超过配置上限。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `2e235f9dd8b14e5b145e8ea69e248a2ba9231b06944f951cd7ca7c0334d339a1`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。未构建或运行，T009/T010 保持 `PARTIAL`，修复后需 v66 复审。

## T010 static boundary v66

官方 `review-agent` 对 v66 复审确认 v65 的两个 P1 和计数修复已闭合，但新增 P2：`reorderPackets && duplicatePackets` 的 Interest/Data 投递顺序变为 `current,pending,current,pending`，旧实现契约为 `current,current,pending,pending`；计数虽正确，fault wire/oracle 语义发生回退。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `3a0912a41b3f7716b429c029ae240c08460209b5bd3d0d9510e26a04d66e8233`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。未构建或运行，T009/T010 保持 `PARTIAL`，修复后需 v67 复审。

## T010 repair review pass v67

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v67` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `d344dd4984836aefdf062be7e44c1b5fea8b10f31994bf41b8b4b6e64d029c14`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。Interest/Data reorder+duplicate 已恢复 `current,current,pending,pending` 旧线序，实际投递计数与 duplicated 计数一致，stream-interest drop 限额在同一锁内复检；bootstrap dedicated-worker 切换、deferred bridge 状态锁、锁外 Face 调用、默认 fixture compatibility 和五 lane 均通过。compile-link/runtime/sanitizer 尚未执行，T009/T010 保持 `PARTIAL`，允许批末验证。

## B5 normal compile pass v22

v67 静态门后，既有 `build-spec185-b0c-normal` 以系统优先 PATH、`CC=/usr/bin/gcc`、`CXX=/usr/bin/g++`、`.lock-spec185-b0c-normal` 和 `-j4` 仅重建 `spec185-provider-assembly`，返回 `rc=0`，耗时 22.509 秒；原始输出见 `.codex-tmp/spec185-b5/normal-build-v22/`。

## B5 normal runtime pass v21-v23

同一 normal 候选（SHA256=`67ebec894a8320fec491e2891b6f23dab6209910b7546308cf61c01d14520f9e`）先运行 authenticated focused selector v21，返回 `rc=0`；随后完整 `Spec185ProviderAssembly` v22、v23 各运行 8 cases，均返回 `rc=0`、`*** No errors detected`。认证请求闭合 ACK/Selection→assembly→runner→Response，三类 provider/epoch/grant substitution 在 assembly 前拒绝，Provider-only lifecycle、assembler/cache 与 stop→drain 均通过。原始日志见 `.codex-tmp/spec185-b5/normal-run-v21/`、`normal-run-v22/` 和 `normal-run-v23/`。

## B5 sanitizer compile/runtime pass v3/v2-v3

v67 静态门后，既有 `build-spec185-b3-asan-ubsan-fast` 以系统优先工具链和 `-j4` 重建 `spec185-provider-assembly`，返回 `rc=0`，耗时 31.025 秒；完整 selector v2、v3 各运行 8 cases，均返回 `rc=0`、无 ASan/UBSan/LSan 报告和 `*** No errors detected`。这两次验证覆盖修复后的 borrowed Face dedicated worker、deferred bridge、Provider assembly/cache/runner/Response 和 stop/drain；原始记录见 `.codex-tmp/spec185-b5/asan-build-v3/`、`asan-run-v2/`、`asan-run-v3/`。此前 v23 UAF 诊断仍保留为修复前失败边界，不被新 PASS 覆盖。

## B5 composition review pass v5 and closure

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-composition-v5` 返回 `B5_COMPOSITION_PASS`，无 P0/P1/P2/P3。快照 base 为 `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，changes SHA256 为 `d344dd4984836aefdf062be7e44c1b5fea8b10f31994bf41b8b4b6e64d029c14`，paths SHA256 为 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。组合门覆盖 T009 Provider ingress/Selection/Response、T010 protected preparation/cache/runner、leases/staging、dedicated/borrowed Face 与 deferred bridge、stop/drain/reaper、fixture/oracle、build/source closure 和 migration/evidence 五 lane；Batch growth=`STOP_GROWTH`，static closure=`CLOSED_FOR_VALIDATION`。结合 normal compile/runtime v22/v21-v23 和 sanitizer compile/runtime v3/v2-v3 的真实 C++ 验收，B5 T009/T010 完整闭合。
