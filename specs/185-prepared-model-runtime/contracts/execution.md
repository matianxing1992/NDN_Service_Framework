# C-03 Request, Conversation and Provider Execution

## Request Projection

PreparedModel::request 复制 owning Input/options，校验 Runtime、模型/任务/schema、时间和 capability，
从 Package 取 NativeModelRef、splitter、默认 placement、catalog.makePreparation；
绑定一个与该用户/包关联的 NativeInferenceClient。复用该 client 的 operation/notification owner，
不逐请求创建一套线程池。Input repository 通过受保护 Repo fetch 验证 exact name/digest/size，
使用同一 request deadline；不把 DataRef.digest 丢弃成纯字符串名字。

NativeRequestPreparation::prepareInput/inspectModel 的输入、adapter、expectedModel、deadline
检查继续执行；canonical catalog 的 inspection 回调查冻结记录，不再次解析全模型。
选择 additive 的 verified Package 私有接线，保留旧 NativeInferenceClient::request 五参数签名；
禁止只凭 caller 提供“alreadyVerified=true”跳过检查。

调用顺序：输入处理 → native request 分配/编码 → Core Request → ACK_CLOSED →
admission → 按本次 budget enumerate candidate → prepareRoles → placement proposeRoles →
独立 validator → request-scoped artifact publication/binding → grant/seal/Selection →
各 Provider 本地 authenticated assembly/data readiness → execution/Response → native terminal。
这里描述职责顺序，细粒度 grant/seal 顺序以 NativeRequestPlanner 的真实安全契约为准，
实现不能为了图的顺序移动授权校验。没有新增 Core 第五种握手或全局 ASSEMBLING barrier。

placement 保留 `NativePlacementStrategy::proposeRoles`：它接收已验证且冻结的 admitted offers，
包含 request/attempt/ACK closure 身份，返回未授权 proposal；不能退回可伪造的裸 ProviderOfferView。
只保留 RequestOptions.placement 一种覆盖入口，null 使用 Package 默认；无合法策略报配置错误。
manifest 不含一个通用 roles 列表：roles 归各 candidate，随预算与状态契约验证。

## Conversation API

```cpp
struct ConversationOptions {
  std::string conversationId; // empty = native owner allocates
  std::optional<NativeConversationCheckpoint> checkpoint;
};
class Conversation {
public:
  RequestHandle request(Input, const RequestOptions& = {});
  NativeConversationCheckpoint checkpoint() const;
  void exportCheckpoint(const std::filesystem::path&) const;
  void close() noexcept;
};
```

Conversation 持有 Package lease、Runtime State 和唯一 NativeConversationCoordinator 会话引用。
可移动不可复制；同一会话最多一个在途 turn，并发请求报 `CONVERSATION_BUSY`，不隐式排无限队列。
不同会话独立。generation/stream 从已验证任务契约推导默认值；不支持会话的 adapter 在 open
抛 UNSUPPORTED_CAPABILITY，不能伪装成无状态多次请求。
请求端不传 parent receipt/role map/plan，coordinator 从已认证 checkpoint/journal 读取。
checkpoint() 仅返回已提交快照；未提交首轮报 `CHECKPOINT_NOT_READY`。
恢复必须验证 requester、model、task、tokenizer、conversation 与 receipt 绑定，不接受换模型的 checkpoint。
持久 wire 复用现有格式和 owner，禁止新增一个只存内存“已成功”的会话副本。

exportCheckpoint 复用184的私有原子导出 helper，不复制另一套写盘逻辑；失败保留旧文件。
close 拒绝新 turn、取消未提交工作；已经 durable commit 的成功不得降级为取消。
stream final 不自动等于 commit；receipt/COMMIT ACK/journal/FINALIZE/rollback 依现有 native coordinator。
onGenerationEvent 是内部事务相关处理，不能被普通 EventObserver 替代；C-01 observe 仍非权威通知。
recovery/replacement 由既有 runtime policy 和 coordinator 执行，不开放调用者自选 attempt 或任意 parent。

## Provider API

```cpp
struct ProviderConfig {
  std::vector<std::string> nativeArguments;
  std::size_t maxArtifactBytes = 1073741824;
  std::size_t maxArtifactEntries = 8;
  std::chrono::milliseconds assemblyJobTimeout{300000};
};
struct ServiceDefinition {
  std::string serviceName;
  std::vector<std::string> allowedRoles;
};
class ProviderRegistration {
public:
  void close() noexcept;
};
class Provider {
public:
  ProviderRegistration serve(const ServiceDefinition&);
  void stop() noexcept;
  bool drain(std::chrono::milliseconds timeout) const;
};
```

当前Provider配置来自CLI，不能假定存在service profiles。ProviderConfig.nativeArguments是操作员
提供的既有DI_NativeProviderExecutable参数tokens（不含argv[0]），提取同一parser为库函数，
不经shell、不启动子进程；保留其必需参数/校验/相对路径（provider创建时固定cwd）规则。
生产facade拒绝spec180 mutation及legacy/preassembled兼容开关；unknown option与CLI一致拒绝。
Runtime验证配置中的IO/trust身份与State一致；ServiceDefinition.serviceName/allowedRoles必须
匹配该Provider配置允许的service/role集合（非空且无重复），不接受任意扩大能力或AuthenticatedSelection。
nativeArguments是本期兼容配置入口，不另建虚构schema；T011把executable与facade切到同一parser。
内部复用 NativeInferenceProvider / NativeProviderHandler 和 NativeRunnerPreparation 接口，
从 executable 提取组合代码为库 owner，executable 与 façade 共用。原草案抽象 AssemblyFactory
按值字段不合法；本期不另建 public assembly plugin ABI，已有内部 factory 保留可扩展性。

ProviderRegistration 可移动不可复制，析构/close 幂等注销能力并停止接受该注册的新请求，
已接受工作由 Provider owner drain；Provider.stop 停止所有注册并取消未提交工作；drain 表示
本地 handler/worker/lease 清理完成，不宣称远端已经停机。Runtime.close 包含这些 stop。

## Provider Cache Layers

当前已有 protected artifact store/staging lease，不等于 warm runner cache。185 新增的复用限于
可验证的不可变 assembled artifact/runner template；每个请求仍创建可变 runner context。
键包括 source/initializer、canonical graph、role/candidate、recipe、backend ABI、device、precision、
quantization、layout、artifact profile 和安全域；密文对象还绑定 protection epoch 与授权保护身份。
不能仅用 model name 或 cacheDir 判断命中。role-plan 中 request-bound binding/evidence 在每次请求重新验证。
所有命中都先验证当前 Selection/grant/deadline/Provider incarnation，再取 artifact，创建前后保留 guard。
plaintext 只活在当前授权 staging lease；不持久化明文或跨请求共享 KV/session。
如果已有后端不能证明 immutable template 与 mutable runner 分离，则该后端只复用 assembled
artifact，不缓存 live runner；这是明确支持矩阵，不冒充 warm runner 命中。
T009/T010 记录 source_fetch、assembly、template_hit、runner_created、active_lease，分别证明各层。
本期不支持 prepare(prewarm=true)，否则会突破 post-Selection 授权边界。

Provider预算独立于requester cache。maxArtifactBytes同时限制暂存+已发布artifact及template内存，
maxArtifactEntries限制条目，均必须正数；现有store中本owner管理的密文对象也计入，不无限落盘。
同exact key最多一个assembly job，等待者各自带已验证Selection/deadline与独立授权lease。
job的上限取assemblyJobTimeout和创建时有效授权期限较早者；不得借后来的waiter延长旧授权。
共享job只发布可安全共享的密文/不可变artifact，plaintext及request-bound evidence独立生成。
最后waiter退出取消job；验证完成后一次原子发布，失败删临时对象；不同key不覆盖彼此。
只驱逐无lease的LRU条目，全pin且预算不足报CACHE_BUDGET_EXCEEDED；stop取消并drain job。
T010反例还包括并发同key、取消一个/全部waiter、预算刚好/少1字节、全pin、发布前失败。
