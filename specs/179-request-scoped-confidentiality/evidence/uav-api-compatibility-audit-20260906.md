# UAV-Experimental / NAC-ABE：旧应用 API 兼容性审计

日期：2026-09-06。范围：旧应用不使用 Controller 在线授予/撤回权限，升级后是否需要改源码。

## 结论

**不能承诺所有旧应用零修改升级。** 普通动态服务注册、请求、构造和静态策略加载入口保留了；应用不必为了链接新框架而主动调用 `grant/revoke` 或 NAC 刷新 API。但公开辅助接口、聚合初始化和重载函数取址存在真实的源码破坏；启动、ACK 和 Controller 状态有效期也影响没有启用在线授权功能的应用。

**旧应用全面兼容的验收结论：BLOCK。** 主要阻断是静态授权状态缺少自动续期：Controller 不改权限而持续运行时，状态到期会拒绝业务。此前短时授权/撤权回归通过，不能证明这个长期静态应用场景通过。本次审计不修改生产代码，也不撤销原回归的实际通过记录。

## 比较基线与方法

| 对象 | 基线 | 当前审计代码 |
|---|---|---|
| NDNSF 主要比较 | `37f8a468`，2026-08-27 UAV 建立检查点 | `UAV-Experimental` / `f87d2887` |
| NDNSF 辅助历史比较 | 本地 `main/master` 快照 `9636a2f3`，2026-08-13 | 先比较到 `37f8a468`，区分 UAV 之前的变化 |
| NAC-ABE 个人分支原版 | `1cc17d9` | `Experimental` / `c3aafa6` |
| NAC-ABE 官方快照 | UCLA-IRL `58f3948` | `c3aafa6` 已包含该提交 |

这些是固定 Git 快照，不是对远端今天最新版本的声明。NAC 的最后一次官方合并相对 `85547eb` 只改两个生产 `.cpp` 文件，没有新增公开头文件差异；本报告同时审计更早的 Spec179 扩展，不能用“最后一次合并没改头文件”推导全部兼容。

先用 CodeGraph 确认现有调用边界，再检查所有上述范围内的框架头文件差异、NAC 公开声明和相关运行路径。编译探针对同一份旧调用源码分别使用历史/当前头文件。另取历史版本三个 NDNSF App、三个 NAC 示例原文编译，没有为了新 API 改写它们。

## 发现：按影响排序

### H1：静态权限状态 24 小时到期，普通查询不续期

即使完全不调用 `grant()` / `revoke()`，新 Controller 启动也设置固定 24 小时授权窗口。随后 `getPolicyStatus()` 和状态 Interest 回答反复返回同一窗口。有效期只在启动或授权 epoch 推进时重设；普通在线状态刷新没有推进窗口。

依据：`ServiceController.cpp:249–276`、`:507–513`、`:765–782`、`:1642–1675`；`ServiceUser.cpp:4300–4320`；`RevocationState.cpp:146–148`。主要基线没有这些有效期字段及执行检查。

实际后果：在没有授权变更/重启的静态部署中，接近到期时即使能联系 Controller，也只能重新取得原到期时间；到期后已有状态拒绝新受保护转换，新取得的过期状态也不能重新授权。**这是框架行为缺陷，不应要求旧应用定时伪造 grant 或通过关闭验证规避。**

验证：到期边界探针已执行并复现拒绝，输出见执行附录。该探针使用真实 Controller 生成的状态和生产 `RevocationState`，给授权函数传入到期边界时间；不是实际运行 24 小时的网络实验。代码审计还单独核对了状态回答路径没有续期操作。

建议：在保持状态名称/版本不可变、安全过期拒绝及无需撤权时不轮换 ABE 参数的前提下，设计并实现自动状态续期；覆盖无任何授权变更的跨到期连续业务，再覆盖 Controller 失联时仍按期拒绝。

### H2：构造完成不再代表 DKEY 就绪；立即请求可能没有回调

构造函数签名不变，但原来的等待 DKEY 循环已变为启动异步获取后返回（`ServiceUser.cpp:1336–1342`；`ServiceProvider.cpp:1581–1588`）。现在 `prepareRequestControllerVersion()` 在 DKEY 未就绪时拒绝请求（`ServiceUser.cpp:4577–4584`）；基础请求入口在建立 `PendingCall` 和超时处理前直接返回空 `ndn::Name`（`:5304–5305`）。

因此，旧代码如果依赖“构造完成→立即提交一次请求→只等回调”，即使原来已预置静态权限，也可能需要改变启动/提交逻辑。既有 `OnlineGrantWaitsForInitialDkeyAndReportsUnwrapFailure` 回归明确要求该阶段返回空 ID、零发布（`tests/integration-tests/controller-revocation-flow.t.cpp:4354–4382`）。

应用应继续驱动 Face 事件循环并处理空请求 ID，不能把未受理请求当成已经注册了超时回调。当前可见的 DKEY 查询叫 `isNacConsumerReadyForTest()`，不应作为推荐的正式应用就绪接口；框架应补充清晰的生产就绪/受理失败契约。已经正确处理异步受理的应用不需要修改业务 handler。

### H3：ACK handler 签名兼容，但业务 payload 可见性变了

普通配置 Controller 且没有显式 capability 模式的 V2 请求会自动启用请求级保密（`ServiceUser.cpp:5224–5269`）。框架先保存原 payload，再把发现请求中的 payload 清空；Provider 的 ACK handler 仍直接接收这个 `RequestMessage`（`ServiceProvider.cpp:4179`、`:9742`）。业务明文在 Selection 后才恢复给实际执行 handler（`:5448–5449`）。

只根据排队长度、算力、服务状态等生成 ACK 的旧应用通常不改代码。**如果旧 ACK handler 解析 `request.getPayload()` 决定是否承接，需要改设计/代码**：把可公开的准入元数据放在明确的发现元数据中，或把依赖私有业务输入的处理留到选中后的执行阶段。不能为了兼容把私密业务输入重新广播给未选中的 Provider。

这项影响来自新的默认保密流程，和应用是否主动使用在线撤权无关。显式设置 capability 的高级调用还有各自模式分支，不能把以上默认条件扩展成所有模式都一样。

### M1：公开大响应辅助函数被删除，需要修改直接调用者

旧的 `ServiceProvider::makeResponseWithLargeDataOptimization(...)` 是 **public**，并非只有框架内部可见。当前替换为 `makeRequestScopedResponseWithLargeDataOptimization(...)`，还增加 provider、请求密钥及安全绑定参数（`ServiceProvider.hpp:1020–1029`，public 区域直到 `:1212` 才结束）。

直接调用旧名字的应用编译失败，不能仅靠重新链接解决。普通 handler 返回 `ResponseMessage` 的调用方式保留；框架在响应发布内部自动进行大响应优化（`ServiceProvider.cpp:5093–5107`）。推荐应用回到这个高层接口，不要要求应用持有内部请求密钥，也不要恢复旧的服务级响应密钥载体。删除该载体是 Spec179 T012 的明确安全迁移决定。

### M2：三个公开聚合结构在中间加字段，位置初始化失败

| 结构 | 插入位置 | 受影响的旧写法 |
|---|---|---|
| `LargeDataReference` | `objectId` 与 `plaintextSize` 之间加入 `keyScope` | `{name, type, id, 1024, true, digest}` |
| `StreamBinding` | `policyEpoch` 与 `deadlineEpochMs` 之间加入 `controllerVersion` | 原来最后的整数 deadline 被用于初始化 optional 版本 |
| `StreamRequestOptions` | `deadlineEpochMs` 与 `eventKeyGrant` 之间加入 `controllerVersion` | 原来后续窗口/容量参数错位 |

依据：`utils.hpp:123–135`；`InvocationStream.hpp:374–406`、`:763–781`。三个案例均有基线通过/当前失败的编译探针。默认构造后按字段赋值的写法不受这个位置问题影响。未来优先评估把新增可选字段放到原字段序列末尾或提供稳定构造入口；这仍不等于 ABI 兼容。

应用迁移例子：

```cpp
LargeDataReference ref;
ref.dataName = name;
ref.objectType = type;
ref.objectId = id;
ref.plaintextSize = 1024;
ref.encrypted = true;
ref.digest = digest;
```

这只修复 C++ 初始化方式。旧的服务级大响应 wire 格式不能因此恢复为受支持的安全路径。

### M3：新增重载破坏无目标类型的成员函数取址

下面三种旧表达式现在都存在重载歧义：

```cpp
auto f = &ndn::nacabe::ParamFetcher::fetchPublicParams;
auto g = &HybridMessageCrypto::cacheReceiveKey;
auto h = &HybridMessageCrypto::cacheWrappedSendKey;
```

普通调用 `fetchPublicParams()`、`cacheReceiveKey(keyId, epochId, key)`、`cacheWrappedSendKey(keyId, key)` 都保留。受影响的是上述 `auto`、没有消歧的 `std::bind`、模板/语言绑定注册等取址方式。

例如 NAC 可改为：

```cpp
void (ndn::nacabe::ParamFetcher::*f)() =
    &ndn::nacabe::ParamFetcher::fetchPublicParams;
```

或用 lambda 明确调用旧参数列表。NAC 无参数真实成员函数已由先前修复恢复，但新增重载依然使无类型取址歧义；不能称为任意旧源码完全兼容。依据：NAC `src/param-fetcher.hpp:40–46`；NDNSF `HybridMessageCrypto.hpp:53–73`。

## 哪些应用通常不需要改 API 调用

| 旧应用用法 | 是否必须修改源码 | 附加条件 |
|---|---|---|
| 普通 `ServiceUser/ServiceProvider/ServiceController` 构造 | 签名不要求 | 重新核对异步就绪，见 H2 |
| `addService`、旧 ACK handler 适配器、`addHandler<RequestT, ResponseT>` | 通常不要求 | ACK 不依赖私有 payload；typed payload 继续满足原序列化接口 |
| 普通 `RequestService`、已有 Targeted 入口 | 高层签名保留 | 就绪、Controller 状态、受理失败处理和新协议部署要求仍适用 |
| Controller 静态策略文件、`start/run` | API 调用不要求 | H1 必须修复；状态存储与新状态路由必须可用 |
| NAC Consumer / Producer / CacheProducer / Authority 普通构造与操作 | 通常不要求 | 完整重编译；不要依赖旧错误类型或错误缓存行为 |
| 不调用在线 grant/revoke/refresh | 不必新增这些调用 | 这不是关闭框架内部状态验证与新保密流程的开关 |
| 上述 M1/M2/M3 特殊调用 | 需要 | 详见逐项迁移说明 |

当前 NDNSF 实现自身调用了扩展后的 NAC API，因此即使应用源码没有调用新 NAC 方法，也必须使用这份配套依赖；官方原版头文件/库不是当前 NDNSF 的可直接替代依赖。

## 重编译、协议与 NAC 行为边界

**必须干净重编译所有依赖对象。** 两个项目的公开完整 C++ 类都增加了状态，NDNSF 消息类也增加字段。不能只替换 `.so`，也不能以相同 SONAME 或链接成功证明 ABI 兼容。包括依赖它们的库、可执行文件和 Python 原生扩展。当前匹配构建为 `build-clang-spec179-official` 与 `.deps/nac-abe-spec179-official`；先前已验证实际 ELF 解析路径和头文件一致性。本次没有重新验收 Python 扩展。

**旧网络节点不能视为可任意混用。** 新节点需要签名 PolicyStatus、ControllerVersion 和请求级密钥绑定；旧 Controller 不提供该状态时，新运行时会拒绝调用。没有保留一个“不使用在线撤权便自动退回旧协议”的通用兼容开关。部署应使用匹配的 Controller/User/Provider 版本并更新必要路由/信任规则；旧 capability 模式与自定义 wire 收发代码需单独评估。此次没有运行混合版本网络矩阵。

NAC 普通应用不调用新刷新 API 时，无需跟随 NDNSF 管理授权 epoch；但以下行为变化仍存在：

- 新鲜 DKEY 发现依赖 Authority，离线首次获取/刷新不能保证成功；不能依赖旧缓存长期代替在线发现。
- OpenABE 失败被规范化为 `NacAlgoError` / 标准异常；捕获底层 enum 或比较精确错误文本的代码可能需要改。
- 解密缓存绑定参数与私钥，换错钥匙不能继续借用已有缓存成功；这是安全修复。
- 官方分段修复尊重显式 segment size；已经缓存的 CK 保留原分段，完整 CK 名称保留 typed segment。默认普通 produce/consume 调用签名不变。
- 新清缓存 API 的取消、Face 线程所有权和进程期 OpenABE worker 的生命周期限制见 NAC `docs/experimental-compatibility.md`。尤其不应把 `.so` 热卸载、fork 后直接调用或并行密码吞吐当作已保证的兼容能力。

## 更早 main 的迁移不能混入本次归因

`9636a2f3 → 37f8a468` 已发生部分 freshness 参数由 `const milliseconds&` 改按值传入、协作方法新增参数（部分带默认值）、受保护协作辅助方法签名变化。普通调用可能仍可编译，但旧精确成员函数指针或调用内部 protected 方法的派生类不一定兼容。这部分是 UAV 分支检查点之前的流式/协作工作，不能归因于最近在线授权修复。

Direct、静态生成 stub、split service/function 名等历史迁移按现有项目契约处理；本次两个近期基线并没有把它们作为仍受支持的普通 API。不能据此要求所有普通旧应用改用另一套业务调用模型。

## 执行证据与限制

编译脚本：[api-compatibility-probes-20260906.py](api-compatibility-probes-20260906.py)。固定 Clang 10、C++17、最多两个并行编译任务，仅语法/模板实例化检查，不冒充完整链接与网络运行。

执行命令：

```bash
python3 specs/179-request-scoped-confidentiality/evidence/api-compatibility-probes-20260906.py
```

结果目录：`results/api-compatibility-audit-20260906/`。最终矩阵、到期边界探针和原始错误诊断均已核对；下方执行附录给出最终计数。

保留的设置失败：最初 typed 探针误用了不满足原 protobuf 风格接口的 `std::string`；随后 NAC 探针的 include 路径顺序使 NAC algo 的裸 `common.hpp` 错误命中 NDNSF 同名头文件。它们在历史版本也失败，不能算本次 API 破坏。已修正为原接口形状及 NAC 自身优先的 include 路径，保留 `setup-attempts/` 并完整重跑。

既有匹配版本证据：NAC 46 用例、NDNSF 183 单元/74 集成以及 18 个 MiniNDN 场景，详见 [官方合并回归报告](nac-abe-official-merge-20260905.md)。本轮核对了日志和当前生产代码未改的事实，没有重复整套网络活动。它们支持已有场景的正确性，不能替代旧源码编译、24 小时静态运行、任意第三方应用或混合版本兼容性验证。

工作流：Context Mode 两层 health 与精确权威源检索通过；CodeGraph 索引当前；Spec Kit strict 结构检查通过（41 FR、22 SC、22 tasks/21 complete，T014 外部发布仍待办）；使用迁移/安全/证据审计维度；GSD health 通过并记录独立审计状态。ARS 不适用于本次纯工程 API 审计。

下一步顺序：先修 H1 自动续期并验证静态权限跨有效期业务；明确 H2 生产就绪/受理失败契约；再收敛 M1–M3 的兼容策略和迁移说明，运行相同旧源码矩阵及受影响的授权/分段回归。H3 的保密边界应保留，不能用恢复明文发现来消除差异。本次没有进行这些生产修复，没有提交 PR 或推送。

## 执行附录：最终结果

`summary.json` 记录 NDNSF `f87d2887b41ecb338f7fd5aaf084afb0448faa9e`、NAC `c3aafa6ec5a566879942107c7b20855659c9dfb9`、Clang `10.0.0-4ubuntu1`。40 次编译覆盖 17 种源码：17 个历史基线全部通过，6 个额外官方 NAC 基线全部通过；当前头文件下 10 个通过、7 个确定的源码兼容失败。最终脚本退出 0 表示基线设置有效，不表示当前头文件全部兼容。

| 探针/原始应用 | 历史基线 | 当前 |
|---|---|---|
| 普通动态 API（含旧 ACK 适配、typed addHandler/RequestService） | 通过 | 通过 |
| 旧大响应辅助函数 | 通过 | **编译失败：函数已删除** |
| `LargeDataReference` 位置初始化 | 通过 | **编译失败：int → string** |
| `StreamBinding` 位置初始化 | 通过 | **编译失败：int → optional 版本** |
| `StreamRequestOptions` 位置初始化 | 通过 | **编译失败：后续参数错位** |
| `cacheReceiveKey` 无类型取址 | 通过 | **编译失败：重载歧义** |
| `cacheWrappedSendKey` 无类型取址 | 通过 | **编译失败：重载歧义** |
| 上述两个 Hybrid 普通调用 | 通过 | 通过 |
| NAC `fetchPublicParams()` 普通调用 | 个人/官方通过 | 通过 |
| NAC `fetchPublicParams` 无类型取址 | 个人/官方通过 | **编译失败：重载歧义** |
| NAC `fetchPublicParams` 精确类型取址 | 个人/官方通过 | 通过 |
| 旧 `App_User` / `App_Provider` / `App_ServiceController`（3 个） | 全部通过 | 全部通过 |
| 旧 KP AA / Consumer / Producer 示例（3 个） | 个人/官方全部通过 | 全部通过 |

六个应用文件逐一与 `git show <baseline>:examples/<file>` 比对，字节完全相同。typed 合成探针使用满足原 `ParseFromArray/SerializeToString` 约束的最小 payload 类型，只验证编译，不声称其空实现验证了业务序列化。

到期探针源码：[static-policy-expiry-probe-20260906.cpp](static-policy-expiry-probe-20260906.cpp)。使用当前匹配头文件构建并链接现有共享库，编译命令在 `expiry-build.log`；执行前 `ldd` 确认选择当前框架 build 与隔离 NAC prefix，记录在 `expiry-ldd.log`。运行使用进程内 PIB/TPM 和结果目录中的独立 Controller 状态，不改系统时钟。

```text
lifetime_ms=86400000 getter_keeps_expiry=1 installed=1
before_allowed=1 expired_allowed=0 reason=controller_status_expired
refresh_accepted_after_expiry=0
```

`expiry-run.log` / 退出 0 的含义是**成功复现缺陷**：重新获取的固定窗口状态在到期边界不能再安装；不是自动续期验收通过。没有执行 24 小时真实网络 soak，也没有声称已修复 H1。
