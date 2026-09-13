# C-01 Public API and Ownership

**Status**: PLANNED。命名空间 `ndnsf::di`；新公开头位于 `cpp/ndnsf-di/Runtime.hpp`、
`PreparedModel.hpp`、`Conversation.hpp`、`Provider.hpp`，实现同目录 `.cpp`；以下省略 namespace 外壳。
所有声明是设计契约，非已编译示例。内部值通过Native类型复用，不创建另一套wire schema；
以下operator/advanced定义与application声明分头，稳定安装入口和Python映射由[C-05](api-usability.md)限定。

## Types and Signatures

```cpp
using Bytes = std::vector<std::uint8_t>;
using Milliseconds = std::chrono::milliseconds;
using RequestId = std::string;
using ModelId = NativeModelDescriptor;
class PlacementStrategy; // opaque registered cooperative strategy; C-05
class Subscription; // move-only cancellation token; C-06
struct Result { Bytes payload; RequestId requestId; std::string modelDigest; std::string planDigest; };
enum class RequestStatus { Pending, Succeeded, Failed, Cancelled };
struct Event { RequestId requestId; Bytes payload; bool terminal = false; std::uint64_t sequence = 0; };
struct RequestDiagnostics { std::uint64_t observationDropped = 0; };
using EventObserver = std::function<void(const Event&)>;
// Defined in public errors.hpp, without the native requester implementation.
class DiError : public std::runtime_error {
public:
  const std::string& code() const noexcept;
  const std::string& domain() const noexcept;
  const std::string& boundary() const noexcept;
  const std::string& requestId() const noexcept;
  std::uint64_t attempt() const noexcept;
};

struct RuntimeConfig {
  std::string nativeConfigPath;           // required, operator-pinned native config
  std::vector<ModelRegistration> models;  // C-05; empty = only default model
  std::size_t maxPreparedBytes = 536870912;
  std::size_t maxPreparedEntries = 8;
  Milliseconds preparationJobTimeout{300000};
};
struct UserConfig { std::string profileName; }; // v1 accepts only empty/default
struct PrepareRequest {
  ModelId model;
  std::string taskName;
  NativeRequestContract taskContract;
  std::string inputLayoutDigest;
  std::string catalogConfigurationJson;
  std::optional<std::string> localSourcePath;
  std::optional<std::string> localInitializerPath;
};
enum class CachePolicy { RequireReady, UseOrWait, UseOrFetch, Refresh };
struct PrepareOptions {
  CachePolicy cache = CachePolicy::UseOrFetch;
  Milliseconds timeout{300000};
};
struct PreparationReceipt {
  enum class Origin { CacheHit, JoinedInFlight, Fetched, Refreshed };
  Origin origin;
  std::string preparationKeyDigest;
  std::string manifestDigest;
  Milliseconds elapsed{0};
};
struct ModelManifest {
  std::string modelName;
  std::string modelRevision;
  std::string modelDigest;
  std::string taskName;
  std::string canonicalGraphDigest;
  std::string planningGraphDigest;
  std::string catalogConfigurationDigest;
  std::string taskContractDigest;
  std::string preparationKeyDigest;
};
class DataRef {
public:
  static DataRef fromPublishedMetadata(std::string canonicalReferenceJson);
  std::string canonicalMetadata() const;
};
class Input {
public:
  static Input inlineBytes(Bytes payload, Bytes applicationOptions = {});
  static Input text(std::string utf8);
  static Input repository(DataRef reference);
};
struct GenerationOptions { std::uint64_t maxNewTokens = 32; };
struct StreamOptions { bool enabled = true; };
struct RequestOptions {
  Milliseconds timeout{30000};
  Milliseconds ackTimeout{5000};
  std::shared_ptr<const PlacementStrategy> placement; // null = prepared default
  std::string applicationRequestId;       // correlation only, not idempotency
  std::string outputMode = "FULL";
  std::optional<GenerationOptions> generation;
  std::optional<StreamOptions> stream;
};
class Runtime {
public:
  static std::shared_ptr<Runtime> open(RuntimeConfig);
  static std::shared_ptr<Runtime> open(const ProviderConfig&);
  User user(UserConfig = {});
  std::shared_ptr<const PlacementStrategy> placementStrategy(const std::string& id) const;
  Provider provider(const ProviderConfig&);
  Provider provider(); // configured Provider-only Runtime; C-06
  void close() noexcept;
  bool drain(Milliseconds timeout) const;
  Subscription drainAsync(Milliseconds timeout,
    std::function<void(std::exception_ptr, bool)> callback) const;
  ~Runtime() noexcept;
};
class User {
public:
  PreparedModel prepare(const std::string& modelKey = "default", const PrepareOptions& = {}) const;
  PreparationHandle prepareAsync(const std::string& modelKey = "default", const PrepareOptions& = {}) const;
  // Advanced/operator overload; ordinary apps use the registered model key.
  PreparedModel prepare(const PrepareRequest&, const PrepareOptions& = {}) const;
};
class PreparedModel {
public:
  const ModelManifest& manifest() const noexcept;
  const PreparationReceipt& receipt() const noexcept;
  ModelCapabilities capabilities() const;
  RequestHandle request(Input, const RequestOptions& = {}) const;
  Result run(Input, const RequestOptions& = {}) const;
  Conversation openConversation(const ConversationOptions& = {}) const;
};
class RequestHandle {
public:
  RequestId id() const;
  RequestStatus status() const;
  Result result() const;
  Result result(Milliseconds timeout) const;
  Result wait() const;
  Result wait(Milliseconds timeout) const;
  EventReader events();
  Subscription onCompletion(std::function<void(std::exception_ptr, std::optional<Result>)>);
  Subscription resultAsync(Milliseconds timeout,
    std::function<void(std::exception_ptr, std::optional<Result>)>);
  RequestDiagnostics diagnostics() const;
  void cancel();
  Subscription observe(EventObserver);
};
```

普通generation/stream选项由runtime映射到 `NativeInferenceClient.hpp:62-63` 的完整内部类型，
tokenizer/安全/布局字段不向普通用户开放。GenerationOptions.maxNewTokens必须>0且不超过任务限制，
其他采样项沿用operator配置；旧完整配置留advanced。StreamOptions.enabled=false等于不请求stream；
没指定stream时按已验证任务默认。ModelRegistration、ModelCapabilities、EventReader完整定义见C-05。

## Symbol Responsibilities and Members

| Symbol / Task | Before → After / private owner | Lifetime and concurrency / failure |
| --- | --- | --- |
| Runtime / T001,T002 | 外部手动拼接 ServiceUser/adapters/preparation → shared State 拥有 Face、IO thread、worker pool、凭据配置、cache、用户/Provider registry | 配置复制；open 完整成功才返回；失败逆序释放；close 原子拒绝新任务并发出取消，drain 是清理屏障 |
| User / T002 | caller 直接构造 NativeInferenceClient → profile principal + shared State | 不可改变 principal；可复制、跨线程 prepare；不暴露 Core/grant setter |
| PreparedModel / T003,T005 | 每次显式 model/splitter → shared immutable Package、shared State、per-call receipt | 非默认构造，由 prepare 返回；可复制；manifest 引用仅在对象/副本存活时有效；request 并发安全 |
| RequestHandle / T006 | NativeInferenceHandle → 持有该 handle、Package lease、State | 不建立第二个 operation 状态机；析构不隐式 cancel；释放包装对象不提前驱逐在途请求依赖 |
| Input / T005 | NativeApplicationInput → owning variant Bytes/DataRef | 工厂复制/移动数据；拒绝空 name/非法 digest/超限；不保留 caller span；没有任意可执行 decoder |
| ModelManifest / T003 | NativeInspectedModel + config + splitter → flattened read-only identity view | 完整task/splitter/inspected仍归Package；不含grant、epoch、plan、ACK、runner或可变候选 |
| PreparationReceipt / T004 | absent → 每次 prepare 的来源、key、耗时 | 等待者独立 receipt；共享 Package 不覆盖别人的 origin；耗时使用 steady_clock |

上述 private State/Package 是唯一所有者结构；mutex/condition_variable/容器选择是 LOCAL_DETAIL，
其必须保持 C-02 原子发布和 C-03 生命周期不变量。禁止 Runtime↔子对象 registry 强引用环；
registry 使用 weak owner，异步 closure 在终态显式断环。

## Defaults, Errors and Thread Contract

prepare/request timeout必须>0、转换不溢出，ackTimeout <= timeout；result/wait允许0作poll，负数拒绝；缓存两个上限及job timeout必须正数。
RuntimeConfig.nativeConfigPath复用 `ndnsf-di-native-requester-v1` 的 core/grant/offer_admission/limits/
request/conversation 分块和原有相对路径规则（相对于配置文件目录）。open只加载运行资源和信任配置，
catalog/source的昂贵读取延后至prepare。现有schema没有profiles；v1 UserConfig只接受空或default，
非空其他值报UNSUPPORTED_CAPABILITY，不接受caller principal；多主体用不同Runtime隔离。
PrepareRequest 的 catalog JSON 是 operator-pinned 当前 catalog schema；model与catalog一致，
taskName必须与独立taskContract.taskName一致。taskContract所有字段复用NativeRequestContract，
检查service/task/adapter descriptor/composition/task descriptor/tokenizer/generation mode，不能从模型hash推导。
taskContract来自操作员request分块（DI_NativeRequester.cpp:61–96），应用仅在prepare传一次。
inputLayoutDigest来自同一request配置，绑定Package与key。默认placement为现有NativePreSplitFirstPlacement；
generation/stream缺省由taskContract及现有native runtime builder推导，显式override整项替换后重新验证，
不能覆盖tokenizer/task/model或安全策略；不支持模式明确拒绝，不做字段级隐式merge。
source 路径只在 prepare 使用；请求阶段从 Package 获取已验证 metadata/source lease。
source 只放 PrepareRequest；去掉原草案 PrepareOptions.source 的重复定义。

Runtime 的 Core 调用统一 post 到拥有 Face 的 IO；解析/下载等待不阻塞该线程。
prepare/run/result/next/drain/exportCheckpoint需要阻塞时，从Runtime IO/通知线程调用返回WOULD_DEADLOCK；0ms poll可用，不偷偷阻塞。
close 可在回调中调用且不等待自己；最后Runtime外壳析构即close，子对象持State不保活外壳；由非owner线程drain。
最后一个外部State/子对象引用释放时
State 通过已有安全 shutdown/join 机制完成清理，禁止 detach 后访问已析构 Face。
Runtime 析构行为和 self-thread 释放须由 T002 C++ fixture 证明；不能用“shared_ptr 会处理”替代。

非法参数、已关闭在入口同步抛 DiError；非default profile同步抛UNSUPPORTED_CAPABILITY；提交后的业务失败由 wait 抛同一
native error，保留 code/domain/boundary/requestId/attempt，不改写为成功空结果。
现有NativeDiError已提供结构化字段；新公开DiError逐字段保留，不强制内部大范围改enum，
不新增可误导的retryable布尔值。公开类由native错误转换构造，用户不手工拼接内部状态。
新增本地码：`MODEL_NOT_READY`、`PREPARATION_NOT_IN_FLIGHT`、`PREPARATION_TIMEOUT`、
`PREPARATION_CANCELLED`、`CACHE_BUDGET_EXCEEDED`、`SOURCE_IDENTITY_MISMATCH`、
`RUNTIME_CLOSED`、`WOULD_DEADLOCK`、`UNSUPPORTED_CAPABILITY`；错误边界分别标 preparation/cache/lifecycle/input。

## Handle Compatibility

result()等到native request终态，受request deadline限制；wait()是其等价alias。result(timeout)/wait(timeout)只限制本次等待，
超时不 cancel，仍可再次 wait。RequestStatus 保留 Pending/Succeeded/Failed/Cancelled，
业务 deadline 以 Failed + native timeout code 表达；不凭空新增 EXPIRED 或 ASSEMBLING 公共状态。
observe委托现有有序通知/历史回放契约，是best-effort诊断，不是可靠stream。
满队列丢弃不能隐藏为完整结果；可靠events reader与gap语义见C-05。新接口不悄悄改变旧回放结果。
applicationRequestId 仅日志关联字段，不能取代由 native owner 分配的 ID、attempt 或重试凭据。

## Documentation Obligations

新增公开声明使用英文 Doxygen 明确参数来源/单位、返回是提交还是完成、异常、ownership、
thread safety 和 callback 禁止阻塞规则；Python docstring 写明 GIL 释放和 native 生命周期。
不得用 generated signature inventory 代替以上行为契约。
