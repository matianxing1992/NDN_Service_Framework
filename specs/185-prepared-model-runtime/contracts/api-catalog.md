# C-07 Complete Public API Catalog and Lifecycle

**Status**: PLANNED / NOT_IMPLEMENTED。本表是Spec185目标application/provider API的统一清单，不能当作当前已安装SDK。
当前实现全部声明仍见[源码索引](../api-surface-index.md)及[逐声明清单](../evidence/api-inventory.json)；二者不互相覆盖。
C-01至C-06定义领域行为，本表统一签名、Python映射、所有权和验收条目；发生冲突必须同时修订，不能自行选择一种实现。

## Four Conclusions

**Implementation ownership**：公开名称和下述行为保持不变；[C-09](core-app-boundary.md)规定Runtime/handle/reader/subscription的通用实现由Core提供，DI仅保留领域配置和类型包装。新增Core primitive属于框架API，不是本表的DI应用API；其签名与字段见C-09 CB01/CB02。

1. **Standalone C++**：目标要求独立安装、链接和运行，含User及Provider完整入口、同步/异步、流、会话恢复、取消和清理；T013前必须由外部C++ consumer证明。当前NOT_RUN。
2. **Python Binding**：核心对象直接由现有pybind11模块绑定C++对象；Python只提供snake_case、秒单位、异常、context manager和asyncio适配，不拥有模型准备、规划、授权、缓存或会话状态机。
3. **Lifecycle**：本次补齐析构、退订、读者取消、流失败、局部异步等待和Runtime关闭后的读取行为；完整性是设计验收要求，尚无运行证明。
4. **Usability**：普通用户只需open→prepare(key)→run(input)，进阶才用request/stream/conversation；不必学习内部Native类或填写plan/grant/epoch。易用性目前是可解释的工程判断，T011应交付完整可复制例子，不能以方法数量或命名相似代替证明。

## Entry Points and Notation

application头`ndnsf-di/api.hpp`；Provider头`ndnsf-di/provider.hpp`；命名空间`ndnsf::di`。
Python application包`ndnsf_distributed_inference.api`，Provider包`ndnsf_distributed_inference.provider_api`。
公共值类型由两个umbrella按需复用，不安装测试私有头；C++17，PImpl完整类型/析构定义在库内。

以下`ms=std::chrono::milliseconds`；`Path=std::filesystem::path`；`Bytes=std::vector<uint8_t>`；
`Done<T>=std::function<void(std::exception_ptr,std::optional<T>)>`；
`DrainDone=std::function<void(std::exception_ptr,bool)>`；`ReadDone=std::function<void(std::exception_ptr,std::optional<StreamEvent>)>`。
这些缩写仅压缩表格，不创建新wire类型。Python `timeout_s`均keyword-only，单位和None/0规则见C-05。
Python字段统一snake_case；不可变原生view返回只读值或副本，不借用已销毁C++存储。
方法的参数默认值与类型以C-01/C-03/C-05/C-06及本表共同定义；所有新方法都须有T015 exposure行和T012 binding/不绑定理由。

## Runtime and Model APIs

| ID | C++ signature | Python API | Result / owner / task |
| --- | --- | --- | --- |
| A01 | `static shared_ptr<Runtime> Runtime::open(RuntimeConfig)` | `Runtime.open(config: RuntimeConfig) -> Runtime` | 同一native配置解析及State；T001 |
| A02 | `static shared_ptr<Runtime> Runtime::open(const ProviderConfig&)` | `Runtime.open(config: ProviderConfig) -> Runtime` | 独立Provider-only，user不可用；T009 |
| A03 | `User Runtime::user(UserConfig = {})` | `runtime.user(config=None) -> User` | 同State/principal；T001 |
| A04 | `shared_ptr<const PlacementStrategy> Runtime::placementStrategy(const string& id) const` | `runtime.placement_strategy(id)` | 已注册opaque策略，不给Python算法回调；T016 |
| A05 | `Provider Runtime::provider(const ProviderConfig&)` | `runtime.provider(config)` | 验证与Runtime一致的身份；T009 |
| A06 | `Provider Runtime::provider()` | `runtime.provider()` | 仅已有Provider配置；T009 |
| A07 | `void Runtime::close() noexcept` | `runtime.close()` | 拒绝新任务，取消未提交工作；T002 |
| A08 | `bool Runtime::drain(ms timeout) const` | `runtime.drain(*, timeout_s=5.0) -> bool` | 有限本地清理屏障；T002 |
| A09 | `Subscription Runtime::drainAsync(ms timeout, DrainDone) const` | `await runtime.drain_async(*, timeout_s=5.0) -> bool` | 同native屏障，超时false；T002 |
| A10 | `Runtime::~Runtime() noexcept` | 显式context退出；GC只是最后防线 | 析构close，State继续安全清理；T002 |
| A11 | `PreparedModel User::prepare(const string& key="default", const PrepareOptions&={}) const` | `user.prepare(key="default", *, options=None, timeout_s=None)` | 同步组合prepareAsync.result；T003 |
| A12 | `PreparationHandle User::prepareAsync(const string& key="default", const PrepareOptions&={}) const` | `user.start_prepare(key="default", *, options=None, timeout_s=None) -> PreparationHandle`；`await user.prepare_async(...) -> PreparedModel` | start直接绑定，await仅便利适配；T003/T004 |
| A13 | `PreparationStatus PreparationHandle::status() const` | `preparation.status` | Pending/Ready/Failed/Cancelled；T004 |
| A14 | `PreparedModel PreparationHandle::result() const` | `preparation.result(*, timeout_s=None)` | 按原waiter期限等待；T004 |
| A15 | `PreparedModel PreparationHandle::result(ms timeout) const` | 同上，显式timeout_s | 局部超时不取消waiter；T004 |
| A16 | `Subscription PreparationHandle::onCompletion(Done<PreparedModel>)` | `preparation.on_completion(callback)` | callback(error,model)，迟订阅可回放；T004 |
| A17 | `Subscription PreparationHandle::resultAsync(ms timeout, Done<PreparedModel>)` | `await preparation.result_async(*, timeout_s=None)` | 有限等待，native timer，可退订；T004 |
| A18 | `void PreparationHandle::cancel()` | `preparation.cancel()` | 只取消该waiter；T004 |
| A19 | `const ModelManifest& PreparedModel::manifest() const noexcept` | `model.manifest` | Python只读快照；T003 |
| A20 | `const PreparationReceipt& PreparedModel::receipt() const noexcept` | `model.receipt` | 每次prepare独立receipt；T004 |
| A21 | `ModelCapabilities PreparedModel::capabilities() const` | `model.capabilities` | 只读能力/schema副本；T003 |
| A22 | `RequestHandle PreparedModel::request(Input, const RequestOptions&={}) const` | `model.request(input, *, options=None) -> RequestHandle` | native非阻塞提交，owner保存输入；T005 |
| A23 | `Result PreparedModel::run(Input, const RequestOptions&={}) const` | `model.run(input, *, options=None) -> Result` | request.result便利组合；T005 |
| A24 | `Conversation PreparedModel::openConversation(const ConversationOptions&={}) const` | `model.open_conversation(options=None)` | 同模型native coordinator；T007 |

## Request, Stream and Subscription APIs

| ID | C++ signature | Python API | Result / owner / task |
| --- | --- | --- | --- |
| A25 | `RequestId RequestHandle::id() const` | `handle.id` | native ID；T006 |
| A26 | `RequestStatus RequestHandle::status() const` | `handle.status` | Pending/Succeeded/Failed/Cancelled；T006 |
| A27 | `Result RequestHandle::result() const` | `handle.result(*, timeout_s=None)` | native终态；T006 |
| A28 | `Result RequestHandle::result(ms timeout) const` | 同上，显式timeout_s | 局部超时不取消业务；T006 |
| A29 | `Result RequestHandle::wait() const` | `handle.wait(*, timeout_s=None)` | result的等价alias，新例子优先result；T006 |
| A30 | `Result RequestHandle::wait(ms timeout) const` | 同上，显式timeout_s | 不能返回旧RequestState；T006 |
| A31 | `Subscription RequestHandle::onCompletion(Done<Result>)` | `handle.on_completion(callback)` | 可靠一次完成通知；T006 |
| A32 | `Subscription RequestHandle::resultAsync(ms timeout, Done<Result>)` | `await handle.result_async(*, timeout_s=None)` | native有限等待，等待取消不cancel业务；T006 |
| A33 | `void RequestHandle::cancel()` | `handle.cancel()` | 幂等取消未提交工作，不降级durable成功；T006 |
| A34 | `Subscription RequestHandle::observe(EventObserver)` | `handle.observe(callback) -> Subscription` | 可退订、best-effort多次诊断；T006 |
| A35 | `RequestDiagnostics RequestHandle::diagnostics() const` | `handle.diagnostics` | 副本，不是提交oracle；T006 |
| A36 | `EventReader RequestHandle::events()` | `handle.events() -> EventReader`；`handle.events_async()`为async迭代适配 | 单active reader；T006 |
| A37 | `optional<StreamEvent> EventReader::next(ms timeout)` | `reader.next(*, timeout_s=None)` | 事件/正常EOF/异常；T006 |
| A38 | `Subscription EventReader::nextAsync(ms timeout, ReadDone)` | `await reader.next_async(*, timeout_s=None)` | 同游标、单在途read、可退订；T006 |
| A39 | `void EventReader::close() noexcept` | `reader.close()` | 结束读者，不取消请求；T006 |
| A40 | `EventReader::~EventReader() noexcept` | with退出/GC调用close | 释放reader lease；T006 |
| A41 | `void Subscription::unsubscribe() noexcept` | `subscription.unsubscribe()` | 幂等、不等待已开始callback、不取消业务；T002/T004/T006 |
| A42 | `Subscription::~Subscription() noexcept` | with退出/GC退订 | 必须保留token到需要的通知完成；同上 |
| A43 | `Subscription(Subscription&&) noexcept`、`Subscription& operator=(Subscription&&) noexcept` | 无copy/deepcopy | move assignment先退订旧slot；同上 |

Python reader还提供`__iter__/__next__`与`__aiter__/__anext__`，分别组合A37/A38；只有正常EOF映射StopIteration/StopAsyncIteration。
reader的async adapter不自己存token队列。显式reader建议with管理；提前break后显式close或退出context，不能承诺Python任意迭代器break自动析构。
`Subscription`统一替代初版`CompletionSubscription`设计名，因为它同时服务一次通知、诊断订阅与单次异步读；未实现，无旧产品ABI要迁移。

## Conversation, Input and Provider APIs

| ID | C++ signature | Python API | Result / owner / task |
| --- | --- | --- | --- |
| A44 | `RequestHandle Conversation::request(Input, const RequestOptions&={})` | `conversation.request(input, *, options=None)` | 单会话turn串行；T007 |
| A45 | `ConversationCheckpoint Conversation::checkpoint() const` | `conversation.checkpoint()` | 仅已durable commit快照；T007 |
| A46 | `void Conversation::exportCheckpoint(const Path&) const` | `conversation.export_checkpoint(path)` | C++原子private export；T008 |
| A47 | `void Conversation::close() noexcept` | `conversation.close()` | 拒绝新turn，取消未提交；T007 |
| A48 | `Conversation::~Conversation() noexcept` | with退出/GC调用close | 不自己阻塞等待网络；T007 |
| A49 | `static ConversationCheckpoint ConversationCheckpoint::fromBytes(Bytes)` | `ConversationCheckpoint.from_bytes(data)` | 解析不等于安全验证；T008 |
| A50 | `Bytes ConversationCheckpoint::bytes() const` | `checkpoint.to_bytes()` | owning副本；T008 |
| A51 | `static Input Input::inlineBytes(Bytes payload, Bytes applicationOptions={})` | `Input.inline_bytes(payload, *, application_options=b"")` | owning输入；T005 |
| A52 | `static Input Input::text(string utf8)` | `Input.text(text)` | 有效UTF8及native adapter支持；T005 |
| A53 | `static Input Input::repository(DataRef)` | `Input.repository(reference)` | 完整受保护metadata；T005 |
| A54 | `static DataRef DataRef::fromPublishedMetadata(string canonicalReferenceJson)` | `DataRef.from_published_metadata(canonical_reference_json)` | 结构验证，不授权；T005 |
| A55 | `string DataRef::canonicalMetadata() const` | `reference.canonical_metadata()` | 完整规范化metadata副本；T005 |
| A56 | `static ProviderConfig ProviderConfig::fromFile(const Path&)` | `ProviderConfig.from_file(path)` | 同C++ loader；T009 |
| A57 | `static ProviderConfig ProviderConfig::fromCommandLine(int argc,const char* const* argv)` | 不作为普通Python入口；CLI-only理由记exposure | native CLI组合器；T009 |
| A58 | `ProviderRegistration Provider::serve(const ServiceDefinition&)` | `provider.serve(definition)` | 同native handler；T009 |
| A59 | `void Provider::stop() noexcept` | `provider.stop()` | 停注册并取消未提交；T009 |
| A60 | `bool Provider::drain(ms timeout) const` | `provider.drain(*, timeout_s=5.0)` | 该Provider本地屏障；T009 |
| A61 | `Subscription Provider::drainAsync(ms timeout, DrainDone) const` | `await provider.drain_async(*, timeout_s=5.0)` | 同屏障；T009 |
| A62 | `void ProviderRegistration::close() noexcept` | `registration.close()` | 停新接收，已接收工作仍由Provider负责；T009 |
| A63 | `ProviderRegistration::~ProviderRegistration() noexcept` | with退出/GC调用close | move-only注册；T009 |
| A64 | `DiError::{code,domain,boundary,requestId}() const noexcept -> const string&`、`uint64_t attempt() const noexcept` | `error.code/domain/boundary/request_id/attempt` | C++字段保留；异常message继承what；T005/T012 |

恢复是A49→ConversationOptions.checkpoint→A24，replacement由已配置native policy处理；不另开放resetJournal/setAttempt/commit/rollback给应用。
同Provider配置再次取得provider返回同一owner的轻量handle；不同身份或不兼容重复配置报CONFIG_CONFLICT。
serve同一service重复注册报SERVICE_ALREADY_REGISTERED，避免两个token意外注销同一能力；同服务扩容用明确不同Provider身份。

## Public Value Types and Fields

| Type | Fields / enum values / defaults | Construction and Python form |
| --- | --- | --- |
| RuntimeConfig | nativeConfigPath必填；models=[]；maxPreparedBytes=536870912；maxPreparedEntries=8；preparationJobTimeout=300000ms | C++值初始化；Python keyword构造，duration字段`preparation_job_timeout_s=300.0` |
| ModelRegistration | key、nativeConfigPath必填 | Python key/native_config_path |
| UserConfig | profileName=""，仅空/default | Python profile_name |
| PrepareOptions | cache=UseOrFetch；timeout=300000ms | Python cache、timeout_s；取消用PreparationHandle，见下文 |
| CachePolicy | RequireReady/UseOrWait/UseOrFetch/Refresh | Python同名enum值，不用字符串猜测 |
| PreparationStatus | Pending/Ready/Failed/Cancelled | 只读enum |
| PreparationReceipt | origin；preparationKeyDigest；manifestDigest；elapsed | origin=CacheHit/JoinedInFlight/Fetched/Refreshed；Python elapsed_s |
| ModelManifest | modelName/modelRevision/modelDigest/taskName/canonicalGraphDigest/planningGraphDigest/catalogConfigurationDigest/taskContractDigest/preparationKeyDigest | native构造，只读；Python snake_case |
| ModelCapabilities | inputSchemaJson/outputSchemaJson；inputKinds/outputModes；streaming=false/conversations=false | native构造，只读；能力不是远端资源保证 |
| RequestOptions | timeout=30000ms；ackTimeout=5000ms；placement=null；applicationRequestId=""；outputMode="FULL"；generation/stream=nullopt | Python timeout_s=30.0、ack_timeout_s=5.0，其余snake_case |
| GenerationOptions | maxNewTokens=32，必须>0且符合任务限制 | Python max_new_tokens |
| StreamOptions | enabled=true | 未传整体options时使用已验证任务默认 |
| RequestStatus | Pending/Succeeded/Failed/Cancelled | deadline映射Failed及结构化code |
| Result | payload/requestId/modelDigest/planDigest | 成功只读值，payload为bytes；不返回status=false |
| Event | requestId/payload/terminal=false | best-effort诊断，不是可靠流 |
| StreamEvent | sequence/payload/terminal=false | 验证后的native顺序，只读 |
| RequestDiagnostics | observationDropped=0 | 只读计数；Python observation_dropped |
| ConversationOptions | conversationId=""；checkpoint=nullopt | Python conversation_id、checkpoint；空ID由native分配 |
| ConversationCheckpoint | opaque bytes-backed值 | fromBytes/from_bytes构造；跨模型恢复重新验证 |
| DataRef/Input | opaque owning值 | 仅列出的工厂构造，不借用调用者buffer |
| ServiceDefinition | serviceName、allowedRoles必填非空无重复 | Python service_name、allowed_roles |
| ProviderConfig | opaque validated配置，cache默认见C-06 | fromFile/from_file构造；普通用户不改私有字段 |
| PlacementStrategy | opaque注册句柄 | Runtime查询；没有Python subclass业务算法入口 |
| DiError | code/domain/boundary/requestId/attempt及message | 绑定异常；不要求用户手工构造 |

普通PrepareOptions移除`std::function<bool()> cancelled`，避免新Python API引入轮询业务回调；统一用PreparationHandle.cancel，旧底层端口保持兼容。
Python prepare/start_prepare上的timeout_s覆盖options中的timeout_s；None表示使用options/default，两者不能产生双重deadline。
原生类型/转换错误在Python为TypeError/ValueError，已进入native的业务/生命周期错误为DiError；native code不得被泛化RuntimeError丢失。
这是新API规则，旧compat异常类型/单位不静默改变。

## Lifecycle Matrix

| Object | Copy / ownership | Destruction / close | After closure / concurrency |
| --- | --- | --- | --- |
| Runtime | C++对象不可复制，通过shared_ptr共享；子handle持State，不强持Runtime外壳 | 最后Runtime外壳析构触发close；State仍负责安全取消/join | 新prepare/request/serve报RUNTIME_CLOSED；已有终态可读，drain可重试；close线程安全 |
| User | 可复制，引用同principal/State | 释放本引用，无独立close | Runtime关闭后不能prepare |
| PreparedModel | 可复制，immutable Package lease+State | 最后副本释放lease，不主动撤销已提交请求 | Runtime关闭后manifest/receipt/capabilities可读；request/openConversation拒绝 |
| PreparationHandle | 可复制，同一waiter；不同prepare调用各自waiter | 最后用户handle释放时取消仍Pending的该waiter；订阅不能无限保活已放弃的waiter | result/cancel/status可并发；终态不可逆；READY结果可重复读取 |
| RequestHandle | 可复制，同operation+Package lease | 析构不cancel；operation保留到终态/cleanup | result/状态可重复读；等待超时不改状态，失败不复用ID自动重试 |
| Conversation | move-only，同coordinator会话引用 | 析构close，取消未提交turn | close后request拒绝；已有checkpoint/export允许，未提交首轮报CHECKPOINT_NOT_READY；同会话只允许一turn |
| Provider | 可复制，轻量同Provider owner；Runtime持有服务owner | 释放handle不stop；显式stop或Runtime.close才停服务 | stop后serve拒绝；drain可重试；不影响不同Provider owner |
| ProviderRegistration | move-only，单能力注册token | 析构close，停止新接收 | 既有请求仍由Provider持有；不可copy重复注销 |
| EventReader | move-only，单游标lease | 析构close；释放active-reader slot | close后next报READER_CLOSED；单reader一个在途next/nextAsync，第二个报READ_IN_PROGRESS |
| Subscription | move-only，native slot/token | 析构/显式unsubscribe；不等待已开始callback | 未开始callback不再调用；已开始最多完成当前一次；业务不被取消 |
| 值/配置/结果 | owning可复制值，发布view只读 | 正常值析构 | 不保留网络线程；不借用已释放字节 |

所有move-only类型提供noexcept move constructor/assignment，禁copy；move assignment先关闭/退订自身旧资源。
moved-from只允许析构、赋值、幂等close/cancel/unsubscribe；其他操作报INVALID_HANDLE，不引发未定义行为。
成员调用和同一C++对象存储的析构不能并发；可在独立handle副本上并发调用已声明线程安全的方法。
Runtime、Provider、Request/PreparationHandle方法线程安全；Conversation checkpoint/close由native串行状态保护；EventReader限制见表。
阻塞prepare/run/result/next/drain/exportCheckpoint不得阻塞Core IO或通知executor；需要等待时抛WOULD_DEADLOCK，0ms poll不等待可用。

## Async Read, Wait and Shutdown Rules

resultAsync是同native终态加局部timer，不启动第二请求；超时调用callback(DiError WAIT_TIMEOUT,nullopt)，不cancel业务/waiter。
Python None使用已有业务剩余deadline；读者在业务终态后仍可排空buffer，不重新延长业务期限。正数只限制局部等待，0为poll。
参数非法、空callback、资源上限在注册前同步失败；订阅成功后正好一次成功/异常回调，除非调用者退订。
同一request operation/preparation waiter全部handle副本共享最多64个completion/timed-wait槽位；request另外最多64个observer、一个active reader。
限额超出报SUBSCRIPTION_LIMIT；callback完成或退订且无执行中引用后才归还槽位，不允许复制handle绕过限额。
drainAsync每个owner最多64等待者；None的Python drain使用固定5秒默认，不从已经关闭的request借deadline。

nextAsync返回Subscription；退订只取消该次read等待，reader仍可用。尚未dispatch的事件不能被退订吞掉：
cursor推进与dispatch取得执行权同一线性化点，退订先赢则保留事件；dispatch先赢可完成本次callback。
reader.close令仍active的pending read以READER_CLOSED结束；已经退订者不回调。
原生stream成功且buffer读完才正常EOF；request失败/取消时，先读已验证buffer，再抛对应DiError，不能伪装EOF。
STREAM_GAP为该reader粘性失败，继续读仍报gap；close释放资源。stream final与会话commit仍分离，最后必须看request.result。

Runtime.close/Provider.stop不阻塞网络；drain成功意味着取消/协议收尾的本地owner、worker、timer及回调已收敛。
通知executor须在业务回调取消/完成后退出，不能关闭executor后才排不可交付的terminal callback。
drainAsync自身的最终通知不计入所等待的屏障，避免自等待；其他已经退订且正在执行的callback须退出后才能drain成功。
close/stop之后拒绝新onCompletion/resultAsync/observe/nextAsync注册（RUNTIME_CLOSED或PROVIDER_STOPPED），已注册回调按取消/终态规则收尾；
迟完成回放仅适用于所属owner仍open。已关闭handle仍可同步读取既有终态及buffer，不能再启动网络或异步订阅。
drainAsync在closing阶段仍允许注册；注册与清理结束以同一锁裁决。已经drained时在调用线程锁外立即callback(null exception,true)，
返回已完成的Subscription，unsubscribe为no-op；此明确快路径可能在方法返回前回调，异常隔离，Python适配须先建Future再注册。
这样不需要为已清理owner重启线程，也没有关闭通知executor后无人交付的回调。T002/T006/T013覆盖close+drain后的迟调用。
最后引用在IO/通知线程释放时由拥有依赖的原生join协调器完成清理；禁止销毁Face后遗留detached worker。
析构只保证启动安全清理；确定性退出使用close+drain，超时保留State供重试，不把超时当资源已释放。

## Python Binding and Convenience Inventory

所有C++核心对象经现有`pythonWrapper/src/ndnsf/di_bindings.cpp`导出；新Python包只能持有这些对象或有限的await适配状态。
不得同时维护Python版Runtime cache、catalog parser、planner、runner、session journal或recovery逻辑。
允许额外的协议便利方法：Runtime/Conversation/ProviderRegistration/EventReader/Subscription的`__enter__/__exit__`；
Runtime的`__aenter__/__aexit__`；EventReader迭代/异步迭代；user.prepare_async、handle.result_async、drain_async。
退出Runtime用close+drain/drainAsync；退出Conversation/registration/reader/subscription调用各自幂等终止接口。
Runtime.open仍是同步本地初始化，async context不会使open自动非阻塞；它不得做远端模型准备，耗时模型工作使用prepare_async。
await适配保留native Subscription到完成/取消；loop关闭先失效callback并退订；GIL下释放Python引用，不能等待持GIL的native join。
Python解释器正常退出测试必须覆盖活动订阅，不承诺进程被SIGKILL时执行任何析构。

## Advanced, Extension and Existing API Scope

本表完整枚举185新稳定application/provider面，不把所有8500条实现声明都承诺成稳定用户API。
高级`User::prepare(const PrepareRequest&,const PrepareOptions&)`保留C-01签名；PrepareRequest的ModelId/taskContract/inputLayoutDigest/catalogConfigurationJson/source/initializer仍由operator提供，普通头仅前置声明。
extension的registry/adapter/split/placement/runner及authority/admin旧接口全声明见现有索引；支持级别由T015逐符号api-exposure.json登记；合作式扩展精确改法已在[C-08](code-design.md#fn08-cooperative-extensions-and-existing-registry-delta)冻结，T016按契约实现并审查真实diff。
不得在该清单之外悄悄新增Python专属必需能力；旧兼容导出不承诺全部1:1绑定，但每一保留/不绑定/淘汰项都必须有原因与维护owner。
API完整交付条件：本表每行→实际头/符号→安装目标→C++例子/断言→Python绑定或显式CLI/advanced理由→Doxygen/docstring。
T015/T011/T012/T014共同核对，任何必需行没有实现/行为证据都保持PARTIAL，不能以目录存在代替完整性。
