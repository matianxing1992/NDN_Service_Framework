# C-08 Implementation Design and Task Bindings

**Baseline**: 6a29dbe8 / Experimental / 2026-09-12；只有文档设计，所有新符号PLANNED。
本契约补齐“类怎么改、关键函数改成什么、谁调用、字段归谁、失败怎么收尾”；公开签名引用C-07，避免再复制64行。
`DI/`精确表示`NDNSF-DistributedInference/cpp/ndnsf-di/`；路径中的`.hpp/.cpp`表示同basename声明与实现两个文件。
`ADAPTERS/`精确表示`NDNSF-DistributedInference/cpp/adapters/`，不在ndnsf-di子目录内。
existing来源以canonical磁盘文件为准；CodeGraph宽同名搜索可能命中staging副本，必须用完整路径复核。
设计状态见末尾；实现前核对source/diff，不把文档就绪或新头文件存在当成产品完成。

## Class and File Change Manifest

| CD | Operation / exact path | Class / before → after | Task / public contract |
| --- | --- | --- | --- |
| CD01 | ADD `DI/Runtime.hpp/.cpp`、`DI/NativeRuntimeBootstrap.hpp/.cpp`、`DI/RuntimeState.hpp/.cpp` | 无稳定Runtime；CLI手动组合 → Runtime外壳、单State和内部bootstrap；不持authority私钥 | T001,T002 / C-07 A01–A10 |
| CD02 | ADD `DI/PreparedModel.hpp/.cpp`、`DI/ModelPreparationCache.hpp/.cpp`、`DI/PreparedModelPackage.hpp` | 每次传catalog/model → immutable Package、独立waiter和有界缓存 | T003,T004 / A11–A24 |
| CD03 | MODIFY `DI/NativeInferenceClient.hpp/.cpp`；ADD `DI/NativeRequestAccess.hpp` | 旧5参数request和result/observe保留 → additive cooperative request与内部操作访问端口；Access实现留client同TU以看见完整Operation | T005,T006 / A22–A43 |
| CD04 | ADD `DI/RequestHandle.hpp/.cpp`、`DI/EventReader.hpp/.cpp`、`DI/Subscription.hpp/.cpp` | absent → 用户包装、native订阅slot和可靠单游标；native Operation仍权威 | T002,T004,T006 / A13–A18,A25–A43 |
| CD05 | ADD `DI/Conversation.hpp/.cpp`；REUSE `DI/NativeConversationCoordinator.hpp/.cpp`、`DI/NativeCheckpointExport.hpp` | 调用者手填continuation → 模型绑定会话包装；原begin/replace/commit/export不另造 | T007,T008 / A24,A44–A50 |
| CD06 | ADD `DI/Provider.hpp/.cpp`、`DI/NativeProviderBootstrap.hpp/.cpp`；MODIFY `examples/DI_NativeProviderExecutable.cpp` | CLI内组合NativeProviderHandlerConfig → 共用C++ loader及Provider owner；旧host serve签名保留 | T009 / A02,A05,A06,A56–A63 |
| CD07 | ADD `DI/ProviderArtifactCache.hpp/.cpp`；MODIFY `DI/NativeRunnerPreparation.cpp`及CD06生产factory接线 | 现有protected store/lease → 有界不可变artifact/template复用；不共享mutable runner/KV | T010 / C-03 Provider cache |
| CD08 | MODIFY `DI/NativePlanning.hpp/.cpp`、`DI/NativeModelRunner.hpp/.cpp`、`DI/NativeRequestCatalog.hpp/.cpp`；ADD `DI/CooperativeStrategies.hpp` | adapter已有freeze，runner backend静默覆盖 → explicit replace/freeze；新合作式扩展与旧abstract vtable并存，catalog明确返回合作splitter | T016 / C-05 extension |
| CD09 | MODIFY `DI/NativeRequestPlanner.hpp/.cpp`、`DI/NativeV3Placement.cpp`、`ADAPTERS/yolo/NativeYoloPlanner.hpp/.cpp`、`ADAPTERS/qwen/NativeQwenPlanner.hpp/.cpp` | 原策略只返回后计时 → 共享算法接合作控制；保留原validator/publication/authority顺序 | T016,T005 / C-05,C-03 |
| CD10 | MODIFY `wscript`、`NDNSF-DistributedInference/ndnsf-distributed-inference.pc.in`、`ADAPTERS/onnx/OnnxRuntimeModelRunner.hpp/.cpp`；ADD `DI/api.hpp`、`DI/provider.hpp`、`DI/extensions.hpp` | 广泛安装内部头、宏改变布局 → explicit exposure/install闭包、稳定PImpl | T015 / C-05 installed gate |
| CD11 | MODIFY `examples/DI_NativeRequester.cpp`、`examples/wscript`、`tests/wscript`；ADD `examples/DI_PreparedModel.cpp`、`tests/installed-api/` | main自己组装 → 生产CLI/外部consumer调用公开SDK；原flags/exit含义保留 | T011,T013 / C-06,C-07 |
| CD12 | MODIFY `pythonWrapper/src/ndnsf/di_bindings.cpp`、`NDNSF-DistributedInference/ndnsf_distributed_inference/api/__init__.py`、`app_sdk/client.py`、`app_sdk/facades.py`（后二者同包前缀）；ADD 同包`api/_async.py`、`provider_api.py` | 旧façade保留显式兼容 → native对象直绑，有限async/context适配 | T012 / C-07 |

CD04 public值类型/错误可独立拆到`DI/Types.hpp`与`DI/Errors.hpp/.cpp`，由api.hpp安装；这是已接受的文件分组，不能改字段含义。
tests的完整路径以tasks卡为准；每个CD同时改其fixture/断言及必要注册。T014是文档交付，不伪造生产类delta。

## Private Types and Important Fields

下列字段名是本期约定；成员访问控制private/internal，不能从application umbrella暴露。局部容器实现可等价调整，但owner/原子性不可改变。

| FIELD | Owner / types | Source / writers / lifetime and invariant |
| --- | --- | --- |
| F01 | `RuntimeState::phase` enum `Open,Closing,Drained`；mutex；`inFlight` size_t | open后唯一State写，close单向转换；计数初0，每个工作ticket一次增减，包含native清理但不计drain最终通知自身 |
| F02 | `RuntimeState::coreUser` shared_ptr<ServiceUser>；`providers` map<string,shared_ptr<ProviderState>>；native IO/executor/key owners | bootstrap创建，State销毁前join；子包装持State，registry指子包装用weak引用，不能Runtime↔子对象环 |
| F03 | `RuntimeState::models` map<string,FrozenModelRegistration>；`clients` map<string,shared_ptr<NativeInferenceClient>> | open冻结key/config；客户端按verified package key与State身份复用，close后拒建；无Python parser或全局单例 |
| F04 | `FrozenModelRegistration`：`key` string、`baseDirectory` filesystem::path、`configurationJson` string、`configurationDigest` string | open复制规范化operator配置，引用密钥路径由State加载；不把私钥字节或grant内容存配置快照；默认/同域约束C-05 |
| F05 | `PreparedModelPackage`：`catalog` NativeRequestCatalog、`registration` shared_ptr<const FrozenModelRegistration>、`cooperativeSplitter` shared_ptr<const CooperativeModelSplitStrategy>、`manifest` ModelManifest、`capabilities` ModelCapabilities、`keyDigest` string、`retainedBytes` size_t | 全验证后private构造，发布后const；registration保留完整request contract/layout配置及来源，不按model名反查；不存requestId/plan/grant或当前授权快照；catalog内既有recipe epoch仍纳入配置身份，不能当当前授权 |
| F06 | `ModelPreparationCache::entries` map<string,Entry>；`nextGeneration` uint64_t；`chargedBytes` size_t | mutex保护，C-02四policy和generation CAS；字节在fetch前预留，失败回收，计数不能因共享指针重复扣减 |
| F07 | `Entry`：`ready` shared_ptr<const PreparedModelPackage>、`normalJob/refreshJob` shared_ptr<PreparationJob>、`generation` uint64_t、LRU访问序号 | key派生规范见C-02；每key至多normal+refresh，旧job不得覆盖新generation，active leases不可驱逐 |
| F08 | `PreparationJob`：`key` string、`generation` uint64_t、`deadline` steady_clock::time_point、`cancelled` atomic<bool>、`waiters` map<uint64_t,shared_ptr<PreparationWaiter>> | job owner独立期限/预算；最后有效waiter退出取消；worker捕获job直到完成，不借用调用栈 |
| F09 | `PreparationWaiter`：`status` PreparationStatus、`deadline` time_point、`package` shared_ptr<const Package>、`receipt` PreparationReceipt、`failure` exception_ptr、mutex/CV、subscription slots | 初Pending，owner终态一次；handle外部引用计数独立于job/slot引用，最后用户handle释放取消该waiter；READY receipt按waiter生成 |
| F10 | `Subscription::State`：`phase` enum Queued,Executing,Cancelled,Finished；callback、native cancel registration、weak owner | owner锁裁决dispatch/退订，执行中不能提前还slot；slot结束释放callback，token不取消业务；Python引用释放遵守GIL |
| F11 | native `Operation::completionSlots/observerSlots`、`streamBuffer` deque<StreamEvent>、`streamBytes` size_t、`nextSequence` uint64_t、`reader` weak_ptr<ReaderState> | 同Operation所有副本共享64/64槽；1024事件/16MiB；仅验证后的native ingress写stream，不从observe队列反推 |
| F12 | `ReaderState`：`cursor` uint64_t、`closed/gap` bool、`pendingRead` shared_ptr<Subscription::State>、strong operation lease | 同operation mutex保护；一次active reader/在途read；cursor推进与callback获得执行权原子，取消先赢不吞数据 |
| F13 | `Conversation::State`：Package lease、RuntimeState、coordinator shared_ptr、conversationId、closed、active request weak handle | coordinator/journal唯一durable事实；包装只作串行入口和已认证checkpoint视图，不存另一套成功状态 |
| F14 | `ProviderState`：Core Provider/NativeInferenceProvider owner、validated config、registrations map、artifact cache、phase/tickets | registration token析构仅停该能力；Provider handle析构不stop；Runtime.close/Provider.stop控制owner |
| F15 | `ProviderArtifactCache::Entry`：exactKey string、immutable artifact/template handle、chargedBytes、activeLeases、generation | exactKey使用C-03完整身份；只有认证请求获得lease，mutable runner新建；不落盘明文或跨权限复用授权凭据 |
| F16 | `PlacementStrategy::State`：cooperative implementation shared_ptr、NativeStrategyIdentity、RuntimeState弱身份、securityDomainDigest | 注册后不可替换；request校验同State/安全域，不能只用字符串重查成不同策略 |

`Package`仅为表内PreparedModelPackage缩写；Native类型定义沿用实际头。ServiceUser/ServiceProvider由`ndn_service_framework`提供。
业务时间均steady_clock，协议wire时间沿用native转换。持久化仍由现有journal/store负责，F01–F16本身不新建wire schema。

## FN01 Runtime Bootstrap and Lifecycle

Before：`examples/DI_NativeRequester.cpp`中的`runtimeConfiguration(const NativeJson&,const NativeRequestCatalog&,const string&,const string&) -> NativeJson`及main负责配置/keys/Core/catalog/client组合。
After：公开A01–A10不变；抽出internal `NativeRuntimeBootstrap`，禁止库反向调用example。

```cpp
// ADD, internal, NativeRuntimeBootstrap.hpp/.cpp
static std::shared_ptr<RuntimeState> NativeRuntimeBootstrap::open(RuntimeConfig config);
static std::shared_ptr<RuntimeState> NativeRuntimeBootstrap::open(const ProviderConfig& config);
static FrozenModelRegistration NativeRuntimeBootstrap::loadModelRegistration(
  const ModelRegistration& registration, const std::filesystem::path& baseDirectory);
static NativeRequestRuntime NativeRuntimeBootstrap::bindRequestRuntime(
  const FrozenModelRegistration& registration, const NativeRequestCatalog& catalog,
  std::shared_ptr<const NativeAuthenticatedGrantClient> grants,
  const std::string& requesterIdentity, const std::string& protectionEpoch);
// ADD, internal RuntimeState.hpp/.cpp
void RuntimeState::requireOpen() const;
std::shared_ptr<NativeInferenceClient> RuntimeState::clientFor(
  const std::shared_ptr<const PreparedModelPackage>& package);
void RuntimeState::close() noexcept;
bool RuntimeState::drain(std::chrono::milliseconds timeout) const;
Subscription RuntimeState::drainAsync(std::chrono::milliseconds timeout, DrainDone callback) const;
```

DrainDone是C-07定义的callback签名，实现在头中展开或具名using。bootstrap顺序固定：解析schema/limits→冻结model key与同域配置→取得用户/Provider所需key/trust（不读authority私钥）→创建Core/IO依赖→构建State owner→发布Runtime。source bytes只在prepare取；部分初始化失败逆序清理，不能发布半个Runtime。
bindRequestRuntime复用`nativeRequestRuntimeFromJson(string,const NativeRequestCatalog&,shared_ptr<const NativeAuthenticatedGrantClient>)`；移动现有runtimeConfiguration逻辑，完整contract/stateMapping/budget不手抄成第二份parser。
protectionEpoch来源沿用已验证native运行配置/owner，不从Package查；现有请求status/grant验证继续逐request。缓存不使旧epoch自动有效。
clientFor只在State内缓存client，绑定catalog.makePreparation(user,service)、native grants/admission/conversation owner；模型包只缓存不可变模型事实。
close先上fence，再取消准备waiter、close所有native client、stop Provider，ticket收尾后停止Core/IO；drain等待F01屏障，C-07 callback快路径及join规则逐条适用。
requireOpen与工作ticket登记须在同State锁提交，不能检查后解锁再入队绕过close；提交失败归还ticket。State/cache/operation锁不嵌套等待，取必要owning引用后解锁，再调下一owner；提交时重验phase/generation，callback永远锁外。
PO01：错配置无owner残留；Runtime先析构但handle活着无UAF；close/drain及callback线程最后释放；Spec185Runtime。

## FN02 Package Preparation and Cache

Before：`NativeRequestCatalog::load(const string&,NativeCanonicalSource,const NativeAssemblyControl&)`每次由调用方取得source后构造；该签名REUSE。
After：A11–A21由cache统一组合；不改load的验证，不接受caller跳过inspection。

```cpp
// ADD, internal ModelPreparationCache.hpp/.cpp
PreparationHandle ModelPreparationCache::prepare(
  const FrozenModelRegistration& registration, const PrepareOptions& options);
std::shared_ptr<const PreparedModelPackage> ModelPreparationCache::buildPackage(
  const FrozenModelRegistration& registration, const NativeAssemblyControl& control);
void ModelPreparationCache::publish(
  const std::shared_ptr<PreparationJob>& job,
  std::shared_ptr<const PreparedModelPackage> package);
void ModelPreparationCache::fail(const std::shared_ptr<PreparationJob>& job,
                                std::exception_ptr failure);
void ModelPreparationCache::cancelWaiter(const std::string& key, std::uint64_t waiterId);
```

prepare：校验State/key/options→mutex下应用C-02四policy→创建独立waiter/deadline或返回对应错误→锁外执行job；命中创建本waiter receipt，不覆盖共享对象。
buildPackage：有界预留bytes→取得owned source/initializer→NativeRequestCatalog::load→检查双graph/task/config身份→从verified adapter构造capabilities→private Package；任何失败释放预留，不发布READY。
registration按值冻结后以shared const存入Package，clientFor直接使用它调用bindRequestRuntime；别名/不同task/refresh不能查错配置。
advanced PrepareRequest在State验证同域后规范化为临时FrozenModelRegistration，含完整taskContract/inputLayout/catalog与source/initializer；相对路径按主Runtime配置目录解释，身份进入C-02 key，不借用caller对象。
cooperativeSplitter从FN08修改后的catalog.cooperativeSplitter直接取得，QWEN/YOLO两个已验证构造分支明确赋值；缺失拒UNSUPPORTED_CAPABILITY，禁止dynamic_cast任意旧插件或伪装control支持。
publish：锁内比较generation/job身份→READY与取消竞争裁决→只发布完整包→移出待通知callbacks→锁外通知；refresh失败保持旧ready/lease。
resultAsync/onCompletion只订阅F09；局部wait超时不更改F09业务deadline；最后外部handle cancelWaiter，不因callback自持有无限保活。
PO02：8waiter单次fetch/inspect、不同waiter取消、refresh逆序/失败、全pin预算、wrong digest不READY；Spec185Preparation。

## FN03 Prepared Request and Native Strategy Wiring

Before保留：`NativeInferenceClient::request(const NativeModelRef&,const NativeApplicationInput&,shared_ptr<const NativeModelSplitStrategy>,shared_ptr<const NativePlacementStrategy>,const NativeRequestOptions&) -> NativeInferenceHandle`。
After新增明确名称`requestCooperative`，仅两策略参数换为`shared_ptr<const CooperativeModelSplitStrategy>`与`shared_ptr<const CooperativePlacementStrategy>`；旧入口explicit legacy，不在普通façade中自动fallback。
旧request与新requestCooperative都进入同一个private `requestImpl(model,input,NativeStrategyPorts,options)`；NativeStrategyPorts字段为FN08的EnumeratePort/PlacementPort及必要owner捕获，定义于internal NativeRequestAccess.hpp。Operation只存一个ports实例，不复制请求状态机。

```cpp
// ADD private, NativeInferenceClient.hpp/.cpp
NativeInferenceHandle NativeInferenceClient::requestImpl(
  const NativeModelRef& model, const NativeApplicationInput& input,
  NativeStrategyPorts strategies, const NativeRequestOptions& options);
// ADD private, PreparedModel.hpp/.cpp
NativeApplicationInput PreparedModel::encodeInput(const Input& input) const;
NativeRequestOptions PreparedModel::projectOptions(const RequestOptions& options) const;
RequestHandle PreparedModel::requestInternal(Input input, const RequestOptions& options,
  std::optional<NativeConversationContinuation> continuation) const;
```

A22：requireOpen→复制owning Input/options→校验capabilities/size/time/placement同Runtime identity→encodeInput→从Package得到modelRef/splitter及State.clientFor→调用requestCooperative→包装同native handle与Package lease。
新增入口完整形状：
```cpp
NativeInferenceHandle NativeInferenceClient::requestCooperative(
  const NativeModelRef& model, const NativeApplicationInput& input,
  std::shared_ptr<const CooperativeModelSplitStrategy> splitter,
  std::shared_ptr<const CooperativePlacementStrategy> placement,
  const NativeRequestOptions& options);
```
不同名字避免具体builtin同时可转换为两种基类时产生overload二义性；旧调用方直接传具体shared_ptr仍选择旧request。
公开request调用requestInternal(...,nullopt)；Conversation是friend，仅传自己由native父状态构造并验证的continuation，写入projectOptions结果的NativeRequestOptions.conversation后仍调用同一requestCooperative。
encodeInput的BYTES/UTF8_TEXT/repository分支沿verified adapter schema；repository完整metadata原样进入native接收路径，Provider取得/解密，不能先在requester拼明文tensor。
projectOptions只映射C-01字段；model/task/tokenizer/contract从verified配置，拒绝用户冲突；grant/ACK/plan/candidate不从Package缓存取。
requestImpl复用现有Operation分配、request envelope、ACK_CLOSED、planner和终态。输入校验失败不发布Selection；动态身份最终仍由原owner验证。
PO03：两次request复用包但request/grant独立；hot-cache revoke、错引用、foreign strategy、unsupported text拒绝；Spec185PreparedRequest。

## FN04 Native Completion, Read and Subscription Ports

Before：NativeInferenceHandle只有result(ms)/cancel/void observe；publishEvent有bounded best-effort丢弃。旧签名继续兼容。
After：新增internal friend `NativeRequestAccess`，直接访问同Operation的F11/F12；公开RequestHandle不成为新状态owner。

```cpp
// ADD internal NativeRequestAccess.hpp; definitions in NativeInferenceClient.cpp
// NativeInferenceHandle grants friendship; Operation stays private to that TU.
static Subscription NativeRequestAccess::subscribeCompletion(
  const NativeInferenceHandle& handle, std::optional<std::chrono::milliseconds> timeout,
  std::function<void(std::exception_ptr,std::optional<Result>)> callback);
static Subscription NativeRequestAccess::subscribeObserver(
  const NativeInferenceHandle& handle, EventObserver observer);
static EventReader NativeRequestAccess::openReader(const NativeInferenceHandle& handle);
static RequestDiagnostics NativeRequestAccess::diagnostics(const NativeInferenceHandle& handle);
static std::optional<StreamEvent> NativeRequestAccess::read(
  const std::shared_ptr<ReaderState>& reader, std::chrono::milliseconds timeout);
static Subscription NativeRequestAccess::readAsync(const std::shared_ptr<ReaderState>& reader,
  std::chrono::milliseconds timeout,
  std::function<void(std::exception_ptr,std::optional<StreamEvent>)> callback);
static void NativeRequestAccess::closeReader(const std::shared_ptr<ReaderState>& reader) noexcept;
// ADD private EventReader.hpp/.cpp
Subscription EventReader::readAsync(std::chrono::milliseconds timeout,
  std::function<void(std::exception_ptr,std::optional<StreamEvent>)> callback);
```

subscribeCompletion锁内校验owner/open/64共享槽→已有终态构造快照或挂slot/timer→锁外native通知。resultAsync传局部timeout，onCompletion传nullopt；timer只完成该slot为WAIT_TIMEOUT，不终止operation。
Operation完整定义继续在NativeInferenceClient.cpp，friend不等于跨TU可见。ReaderState在internal NativeRequestAccess.hpp定义F12，持shared_ptr<Operation>而不在该头解引用；EventReader.cpp只委托Access.read/readAsync/closeReader，所有Operation锁/字段访问留同TU。
订阅与native终态转换在同Operation mutex同步；所有success/fail/cancel路径汇聚既有终态发布点再取slot，不只覆盖unary success。
可靠stream在原验证/排序后的ingress、best-effort observe排队之前写F11；观察队列溢出不影响可靠buffer。C-07 overflow/gap/EOF规则不得从旧notifications列表推导。
readAsync：校验single reader/read→若有event保留candidate→锁内取得dispatch权并推进cursor→锁外callback；若退订先赢保持cursor。超时/close/failure与dispatch同一提交点，active pendingRead归零。
next用同reader状态/CV等待，不为每个read创建线程；nextAsync返回Subscription；observe返回多次slot token，旧void observe继续原约定。
Subscription::unsubscribe锁内markCancelled并撤timer/queued callback，执行中只失效未来调用；退出callback后还额度，析构不得自join。
PO04：全部终态通知、64槽共享、close后注册拒绝、迟drain快路径、read取消不吞事件、failure非EOF、slow-reader gap；Spec185PreparedRequest与Runtime。

## FN05 Conversation and Checkpoint

Before REUSE：NativeConversationCoordinator::beginTurn/replaceAttempt/bindInitialPlanRoleMap/bindAttemptPlanRoleMap/prepareCheckpoint/commitTurn/find/restore全部保持现有签名；native客户端仍拥有receipt/control/durableCommitGate。
After公开A24/A44–A50；新增包装私有转换，不开放原coordinator给普通用户。

```cpp
// ADD private Conversation.hpp/.cpp
NativeConversationContinuation Conversation::makeContinuation(const Input& input) const;
void Conversation::requireIdle() const;
// REUSE existing nativeConversationCoordinatorFromConfig(string,path,string)
```

openConversation验证模型能力/tokenizer/security及checkpoint后取得State同一个coordinator；checkpoint无身份认证不得直接写journal。
request锁内requireIdle/closed检查并保留active ticket→从verified parent/transcript构造continuation→走FN03同requestImpl；requestId/attempt在native分配后才进入beginTurn，包装不能提前假造。
实际接线为Conversation→PreparedModel::requestInternal(input,options,continuation)；projectOptions不得丢continuation。同调用同步抛错时作用域ticket立即解busy，异步提交成功才移交operation完成/失败收尾；不能只靠可能尚未注册的完成订阅释放入口。
完成订阅只解除包装active入口/更新只读视图，不能执行commit；checkpoint从native已提交结果/coordinator.find取得；stream final仍不够。
恢复使用现有opaque wire/transcript和restore，replacement继续native policy/replaceAttempt。exportCheckpoint委托NativeCheckpointExport.hpp原子private helper，失败保留旧文件。
PO05：两轮、同时turn拒绝、commit前/后取消、重开恢复、错模型/tokenizer、替换attempt隔离及export失败；Spec185Conversation。

## FN06 Provider Bootstrap and Registration

Before：`NativeInferenceProvider::serve(const NativeServiceDefinition&,const NativeProviderHandlerConfig&) -> NativeServiceRegistration`仅在Face线程或loop启动前调用；stop可跨线程，host不拥有Face join。
After保留该签名；Provider包装管理线程切换/配置和生命周期，不把host直接改成另一套Core Provider。

```cpp
// ADD internal NativeProviderBootstrap.hpp/.cpp
static ProviderConfig NativeProviderBootstrap::fromFile(const std::filesystem::path& path);
static ProviderConfig NativeProviderBootstrap::fromCommandLine(int argc,const char* const* argv);
static std::shared_ptr<ProviderState> NativeProviderBootstrap::open(const ProviderConfig& config);
static NativeProviderHandlerConfig NativeProviderBootstrap::handlerConfig(
  const ProviderConfig& config, const ServiceDefinition& service);
```

将生产CLI parser与factory组合移动到bootstrap，CLI仅参数/信号/退出编排；C-06 launch schema解析在同C++ owner，无shell或Python补parser。
A58校验Runtime/Provider open及service允许集→注册重复fence→post到Face调用native serve→取得native registration后返回public token；该公开同步方法在非IO线程等待本地注册完成，IO线程可直接执行，不持锁跨等待。
slot创建失败撤fence；token.close委托native draining fence并停新接收，已接受work保留依赖；stop负责全部注册/未提交取消，drain属于ProviderState ticket屏障。
handlerConfig保留authenticated Selection之后的runnerPreparationFactory和所有guard；不能在serve中提前下载模型或组装。
PO06：独立Provider无User目录启动；无Selection fetch=0；wrong identity、重复service、close/stop时已接受工作清理；Spec185ProviderAssembly。

## FN07 Provider Artifact Cache

新增内部类型均在ProviderArtifactCache.hpp定义，不公开ABI：
`ProviderArtifactKey`的string字段为sourceDigest/initializerDigest/canonicalGraphDigest/role/candidateDigest/recipeDigest/backendAbi/device/precision/quantization/layoutDigest/artifactProfile/securityDomain/protectionEpoch/protectionIdentity；都从已验证projection、backend配置或本owner取得，按固定字段名canonical编码生成key，不能用任意metadata map代替。
`PreparedProviderArtifact`含string encryptedObjectName/ciphertextDigest/formatVersion/canonicalMetadataJson及uint64_t ciphertextBytes；只有非秘密绑定metadata和受保护store引用，不含request-bound授权凭据或明文path。
canonicalMetadataJson仅保留既有schema中的模型/source/recipe/content绑定，不缓存requestId/attempt/plan/grant/receipt；这些由每次已认证请求重新绑定。
`ProviderArtifactLease`为move-only，持shared_ptr<const PreparedProviderArtifact>与一次release回调；析构归还cache entry activeLeases，per-request plaintext staging仍由ProtectedRuntime原lease负责。
当前后端先固定artifact-only：没有已证明可共享的immutable runner template就不新增通用template基类或缓存live session；T010的支持矩阵明确该限制，不能声称warm runner命中。

```cpp
ProviderArtifactLease ProviderArtifactCache::acquire(
  const ProviderArtifactKey& key, const NativeSelectionProjectionV3& projection,
  const NativeRequestControl& control,
  std::function<std::shared_ptr<const PreparedProviderArtifact>(const NativeRequestControl&)> build);
void ProviderArtifactCache::stop() noexcept;
```

调用者仅是认证后runner factory；acquire不替代ProtectedRuntime鉴权。先验证本waiter授权/期限→预留独立lease/预算→single-flight→锁外build→锁内generation发布→每请求新建mutable runner；命中也重复guard。
build收到job-owned control：deadline取首个有效授权与assemblyJobTimeout较早者，cancel predicate由job而非首waiter持有；单waiter离开不能通过捕获的caller取消对象终止其他waiter。
job仍绑定创建时授权上限且不得被后来waiter延长；授权撤销/到期取消该job并拒所有不能重新验证的使用。共享结果只能是合法受保护不可变artifact，不共享plaintext/KV；每个使用者重新取得自己的授权staging lease，Creator新建mutable context。
本期不改`bindNativeRunnerPreparationContext(NativeModelRunnerSpec&,const NativeSelectionProjectionV3&,const NativeRunnerPreparationContext&)`签名；保留其可信Provider观测绑定，再使用当前factory路径。
失败释放预留/临时对象/本waiterlease；其他waiter仍有效时不能销毁共享job。最后waiter退出取消。PO07：cold/hit身份计数、两请求真实KV隔离、换epoch/ABI/role、全pin、发布前失败；Spec185ProviderAssembly。

## FN08 Cooperative Extensions and Existing Registry Delta

现有NativeAdapterRegistry已经register拒重复且有freeze/frozen/find；不把这些记为185新增。
ADD `void replaceAdapter(shared_ptr<const NativeModelAdapter>)`：只允许未frozen且已有相同ID；constructor/builder单线程，freeze后只读发布，不声称并发mutation安全。
现有`RegistryNativeModelRunnerFactory::registerBackend(string,Creator)`静默覆盖；MODIFY为拒重复，ADD `replaceBackend(string,Creator)`、`freeze()`、`bool frozen() const noexcept`及private `bool m_frozen=false`。
Creator现有类型`function<shared_ptr<NativeModelRunner>(const NativeModelRunnerSpec&)>`及create签名不变；所有backend注册后freeze再发布workers，执行Creator不持registry锁。

```cpp
// ADD declarations in NativePlanning.hpp before builtin strategy declarations.
// CooperativeStrategies.hpp/installed extensions.hpp re-export; no include cycle.
struct ExtensionControl {
  std::chrono::steady_clock::time_point deadline;
  std::function<bool()> cancelled;
  void requireActive() const;
};
class CooperativeModelSplitStrategy {
public:
  virtual ~CooperativeModelSplitStrategy() = default;
  virtual NativeStrategyIdentity identity() const = 0;
  virtual std::vector<NativeSplitCandidate> enumerate(const NativeModelDescriptor&,
    const NativeGraphSnapshot&,const NativeCandidateBudget&,const ExtensionControl&) const = 0;
};
class CooperativePlacementStrategy {
public:
  virtual ~CooperativePlacementStrategy() = default;
  virtual NativeStrategyIdentity identity() const = 0;
  virtual NativeRolePlacementProposalV3 proposeRoles(const NativeOfferBindingContext&,
    const std::string& ackClosedDigest,const std::vector<NativeSelectionRoleV3>&,
    const std::vector<NativeAdmittedOfferV3>&,std::uint64_t nowMs,const ExtensionControl&) const = 0;
};
```

旧abstract策略签名不换vtable。具体builtin NativeQwenLayerSplit/NativeYoloComponentSplit/NativePreSplitFirstPlacement增加cooperative接口基类和对应重载；identity实现共用，构造参数不变；具体类/NativeRequestCatalog布局变化需重建依赖消费者，不声称旧binary兼容。
builtin算法提取共用helper，旧入口传legacy control，新cooperative入口传真实control。
NativeRequestCatalog增加`shared_ptr<const CooperativeModelSplitStrategy> cooperativeSplitter`；load的QWEN/YOLO分支在具体typed shared_ptr尚未擦除类型前同时赋值splitter与cooperativeSplitter，不新增二次source解析、不修改原构造参数或依赖dynamic_cast。
因此T016前移到B1之后、T003之前，cooperative类型/catalog接线先完成再构建Package；批次仍保留ID B2E，但顺序按plan表，不按数字猜测。
新内部`EnumeratePort`/`PlacementPort`即上述两虚函数的同参数同返回`std::function`，定义NativeStrategyPorts的两个字段；另含NativeStrategyIdentity splitterIdentity/placementIdentity，由具体策略identity()在构造ports时复制，用于原planner身份绑定。callable捕获shared策略owner到operation结束，不能借用临时引用。
新增planNativeRequestCooperative，除splitter/placement引用类型替换外其他参数、默认conversationTurn=nullptr均与现有头一致；旧planNativeRequest与新入口调用唯一private planNativeRequestImpl（这两个策略引用参数改NativeStrategyPorts）。
新planner签名（声明NativeRequestPlanner.hpp，定义同.cpp）：
```cpp
NativePlannedRequest planNativeRequestCooperative(
  const NativeRequestRuntime& runtime, const NativeRequestOptions& options,
  const NativeInspectedModel& model, const NativeEncodedRequest& encoded,
  const CooperativeModelSplitStrategy& splitter,
  const CooperativePlacementStrategy& placement,
  const NativeRequestPreparation& preparation, const NativeOfferAdmission& admission,
  const ndn_service_framework::CollaborationAckClosure& closure,
  const NativeRequestControl& control, std::uint64_t wireDeadlineMs,
  std::shared_ptr<const std::atomic<bool>> cancelled,
  const NativeConversationTurn* conversationTurn = nullptr);
```
旧具体builtin引用调用planNativeRequest仍不二义；新增编译反例必须直接传具体类而非仅基类擦除指针。
控制deadline=min(request deadline,now+剩余policy预算)；保留仅累计策略时间的既有语义，不将grant/artifact时间混算。candidate/role/provider/graph遍历循环内检查requireActive，不能只调用前后检查。
返回后requireActive→原独立validator→原artifact/grant/seal；超时/取消不能落入NoFeasiblePlacement继续候选分支；所有入参含已验证ACK closure/admitted offers，不降级裸OfferView。
新普通PlacementStrategy只持F16注册句柄，拒legacy非合作插件及foreign Runtime handle；策略扩展不能获得Core/authority自由入口。
PO08：重复/replace/freeze、8线程冻结lookup、真实mutable session隔离、循环内取消、返回后超时、late proposal不publication/Selection；Spec185ExtensionRegistry。

## FN09 Installation and ABI

`contracts/api-exposure.json`是唯一安装/支持清单；每个已安装头及Python导出必须分类。wscript按显式清单安装并核验transitive include，公共头可单独包含。
现有NativeCheckpointExport/NativeConversationWire依赖未安装CanonicalJson，NativeCanonicalOnnxAssembler依赖未安装worker；这些内部helper迁移到internal不再安装，若存在合法外部consumer则先改其公开依赖并保留明确兼容入口，不能全安装worker/vendor遮盖问题。
具体ONNX修正固定为：`class Impl;`及`unique_ptr<Impl> m_impl`不再受NDNSF_DI_ENABLE_ONNXRUNTIME_CPP宏影响；`m_spec/m_evidence`保持；enabled实现保留现有Impl，disabled cpp定义空Impl后定义析构，disabled构造仍明确报backend unavailable。
T015只验本批存在的安装头/ABI及新umbrella骨架，不要求未实现的全部facade提前链接；T011对最终新增全SDK重新外部消费。必要ABI消费者重建，无旧二进制兼容虚假保证。
PO09：normal/ASan与enabled/disabled各自同配置外部consumer include/link、构造析构或预期unavailable；Spec185InstalledApi。

## FN10 CLI, Binding and Qualification

T011把DI_NativeRequester main的配置/catalog/client组合改为Runtime.open→user.prepare→request/conversation；现有input/输出/退出码保持对照，信号仅触发公开cancel/close；Provider main同FN06。
新DI_PreparedModel.cpp使用安装umbrella，按C-07含同步/异步、流、会话及Provider例子，不读测试私有头。examples/wscript、tests/wscript注册同生产library，不复制DI源子集。
T012直接在di_bindings.cpp绑定A01–A64核心对象和所有值类型；C-07每行记录def/property或CLI-only不绑定原因。_async.py只持native handle/Subscription、Future及弱loop失效状态；先建Future再注册，call_soon_threadsafe，取消退订，native资源释放时GIL/join顺序按C-07。
api/__init__.py明确导出，client/facades旧签名只适配相同native owner；旧单位/错误不能静默改。无Python tokenization/planning/session/cache implementation。
PO10：T011每C-07行有外部consumer/对应领域oracle；T013完整C++流程先通过；T012再验映射、GC/loop关闭、异常与单位；T014文档比对。测试body/driver/oracle约束仍C-04。

## FLOW and Failure Owners

| FLOW | Ordered production calls | Commit / failure cleanup | Proof |
| --- | --- | --- | --- |
| FLOW01 | CLI/app→Runtime.open→bootstrap.loadModelRegistration→State发布 | 初始化全部成功才发布；bootstrap逆序释放 | PO01 |
| FLOW02 | User.prepareAsync→cache.prepare→buildPackage→NativeRequestCatalog.load→publish→waiter通知 | generation+waiter终态锁内提交；job失败无READY、旧refresh保留 | PO02 |
| FLOW03 | model.request→encode/project→clientFor→requestCooperative→requestImpl→ACK_CLOSED→planNativeRequestImpl→validator→原publication/seal→Selection | 原operation二次deadline/cancel fence；无策略失败Selection | PO03,PO08 |
| FLOW04 | 已验证native stream ingress→可靠buffer→read dispatch；native terminal→completion slots | cursor/dispatch同锁；失败不EOF；slot callback锁外 | PO04 |
| FLOW05 | model.openConversation→同coordinator→conversation.request→FLOW03→receipt/COMMIT ACK/durable gate→checkpoint | journal唯一成功事实；失败原rollback，不创建第二终态 | PO05 |
| FLOW06 | Provider.serve→Face native host→authenticated Selection→guards→cache.acquire→factory→run/Response | 每request权限/lease独立；失败job和mutable context清理 | PO06,PO07 |
| FLOW07 | close/stop→fence→cancel/close native owners→tickets/callback退出→drain→join | 不等待自身通知；晚订阅拒绝，已drained快路径 | PO01,PO04,PO06 |

## Design Binding and Readiness

| Task | Class / fields / functions / flow / proof |
| --- | --- |
| T001 | CD01/F01–F04/FN01/FLOW01/PO01 |
| T002 | CD01,CD04/F01,F02,F10/FN01,FN04/FLOW07/PO01,PO04 |
| T003 | CD02/F04–F09/FN02/FLOW02/PO02 |
| T004 | CD02,CD04/F06–F10/FN02,FN04/FLOW02/PO02,PO04 |
| T005 | CD03/F03,F05,F16/FN03,FN08/FLOW03/PO03,PO08 |
| T006 | CD03,CD04/F10–F12/FN04/FLOW04,FLOW07/PO04 |
| T007 | CD05/F13/FN05/FLOW05/PO05 |
| T008 | CD05/F13/FN05/FLOW05/PO05 |
| T009 | CD06/F02,F14/FN06/FLOW06,FLOW07/PO06 |
| T010 | CD07/F15/FN07/FLOW06/PO07 |
| T011 | CD10,CD11/FN09,FN10/FLOW01–FLOW07/PO09,PO10 |
| T012 | CD12/F10/FN10/C-07 all mappings/PO10 |
| T013 | CD11/FN10/FLOW01–FLOW07/PO01–PO10；资格fixture代码按既有任务范围 |
| T014 | N/A生产代码；Design/API当前与目标、实际exports、PDF与证据交付 |
| T015 | CD10/FN09/PO09；existing安装/ABI前置，不冒充最终SDK已存在 |
| T016 | CD08,CD09/F16/FN08/FLOW03/PO08 |

**Design status**: READY_FOR_IMPLEMENTATION（本基线CD01–CD12与T001–T016设计范围；T014为文档）。已完成组合只读审查与具体接线缺口修订；没有把设计状态记为实现/编译/行为PASS。开工时源码漂移仍需重新核对受影响范围。
**LOCAL_DETAIL**: 等价map实现、局部变量、日志文案、内部小helper拆分；不可改变key字节、接口、提交点、owner/锁顺序、序列化或安全校验。
新增/改变上述契约先修本文件与受影响tasks；源码审查比较真实diff候选与CD双向覆盖，不以本表存在判PASS。
英文Doxygen每公开方法写参数/返回/异常/owner/thread规则；内部F01计数、F06 generation、F10 slot、F12 cursor和durable边界注释说明提交点与原因。
