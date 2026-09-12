# C-01 Public API and Ownership

**Status**: PLANNED。命名空间 `ndnsf::di`；新公开头位于 `cpp/ndnsf-di/Runtime.hpp`、
`PreparedModel.hpp`、`Conversation.hpp`、`Provider.hpp`，实现同目录 `.cpp`；以下省略 namespace 外壳。
所有声明是设计契约，非已编译示例。已有值类型通过 `Native*` 精确复用，不创建另一套 wire schema。

## Types and Signatures

```cpp
using Bytes = std::vector<std::uint8_t>;
using Milliseconds = std::chrono::milliseconds;
using RequestId = std::string;
using ModelId = NativeModelDescriptor;
using PlacementStrategy = NativePlacementStrategy;
using Result = NativeInferenceResult;
using RequestStatus = NativeRequestStatus;
using Event = NativeInferenceEvent;
using EventObserver = std::function<void(const Event&)>;
using DiError = NativeDiError;

struct RuntimeConfig {
  std::string nativeConfigPath;           // required, operator-pinned native config
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
  std::function<bool()> cancelled;        // empty = never cancelled
};
struct PreparationReceipt {
  enum class Origin { CacheHit, JoinedInFlight, Fetched, Refreshed };
  Origin origin;
  std::string preparationKeyDigest;
  std::string manifestDigest;
  Milliseconds elapsed{0};
};
struct ModelManifest {
  NativeInspectedModel inspected;
  std::string catalogConfigurationDigest;
  NativeStrategyIdentity splitterIdentity;
  NativeRequestContract taskContract;
  std::string taskContractDigest;
  std::string preparationKeyDigest;
};
struct DataRef {
  ndn::Name name;
  std::string digest;                    // sha256:<64 lowercase hex>
  std::uint64_t bytes;                   // exact size; 0 means empty, never unknown
};
class Input {
public:
  static Input inlineBytes(Bytes payload);
  static Input repository(DataRef reference);
};
struct RequestOptions {
  Milliseconds timeout{30000};
  Milliseconds ackTimeout{5000};
  std::shared_ptr<const PlacementStrategy> placement; // null = prepared default
  std::string applicationRequestId;       // correlation only, not idempotency
  std::string outputMode = "FULL";
  std::optional<NativeGenerationExecutionContractV1> generation;
  std::optional<ndn_service_framework::StreamRequestOptions> stream;
};
class Runtime {
public:
  static std::shared_ptr<Runtime> open(RuntimeConfig);
  User user(UserConfig = {});
  Provider provider(const ProviderConfig&);
  void close() noexcept;
  bool drain(Milliseconds timeout) const;
  ~Runtime() noexcept;
};
class User {
public:
  PreparedModel prepare(const PrepareRequest&, const PrepareOptions& = {}) const;
};
class PreparedModel {
public:
  const ModelManifest& manifest() const noexcept;
  const PreparationReceipt& receipt() const noexcept;
  RequestHandle request(Input, const RequestOptions& = {}) const;
  Conversation openConversation(const ConversationOptions& = {}) const;
};
class RequestHandle {
public:
  RequestId id() const;
  RequestStatus status() const;
  Result wait() const;
  Result wait(Milliseconds timeout) const;
  void cancel();
  void observe(EventObserver);
};
```

generation/stream 精确复用 `NativeInferenceClient.hpp:62-63` 的类型；字段含义与验证
继续由原生执行契约拥有，不引入字段相同的平行 DTO。相关类型映射见执行契约。

## Symbol Responsibilities and Members

| Symbol / Task | Before → After / private owner | Lifetime and concurrency / failure |
| --- | --- | --- |
| Runtime / T001,T002 | 外部手动拼接 ServiceUser/adapters/preparation → shared State 拥有 Face、IO thread、worker pool、凭据配置、cache、用户/Provider registry | 配置复制；open 完整成功才返回；失败逆序释放；close 原子拒绝新任务并发出取消，drain 是清理屏障 |
| User / T002 | caller 直接构造 NativeInferenceClient → profile principal + shared State | 不可改变 principal；可复制、跨线程 prepare；不暴露 Core/grant setter |
| PreparedModel / T003,T005 | 每次显式 model/splitter → shared immutable Package、shared State、per-call receipt | 非默认构造，由 prepare 返回；可复制；manifest 引用仅在对象/副本存活时有效；request 并发安全 |
| RequestHandle / T006 | NativeInferenceHandle → 持有该 handle、Package lease、State | 不建立第二个 operation 状态机；析构不隐式 cancel；释放包装对象不提前驱逐在途请求依赖 |
| Input / T005 | NativeApplicationInput → owning variant Bytes/DataRef | 工厂复制/移动数据；拒绝空 name/非法 digest/超限；不保留 caller span；没有任意可执行 decoder |
| ModelManifest / T003 | NativeInspectedModel + config + splitter → read-only public view | 构造前校验关联摘要；不含 grant、epoch、plan、ACK offers、runner、request/attempt 或可变候选 |
| PreparationReceipt / T004 | absent → 每次 prepare 的来源、key、耗时 | 等待者独立 receipt；共享 Package 不覆盖别人的 origin；耗时使用 steady_clock |

上述 private State/Package 是唯一所有者结构；mutex/condition_variable/容器选择是 LOCAL_DETAIL，
其必须保持 C-02 原子发布和 C-03 生命周期不变量。禁止 Runtime↔子对象 registry 强引用环；
registry 使用 weak owner，异步 closure 在终态显式断环。

## Defaults, Errors and Thread Contract

所有 timeout 必须 >0、转换不溢出，ackTimeout <= timeout；缓存两个上限及 job timeout 必须正数。
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
prepare、wait、drain 从 Runtime IO/通知回调线程调用返回 `WOULD_DEADLOCK`，不偷偷阻塞。
close 可在回调中调用且不等待自己；由非 owner 线程 drain；最后一个外部 Runtime/子对象释放时
State 通过已有安全 shutdown/join 机制完成清理，禁止 detach 后访问已析构 Face。
Runtime 析构行为和 self-thread 释放须由 T002 C++ fixture 证明；不能用“shared_ptr 会处理”替代。

非法参数、已关闭在入口同步抛 DiError；非default profile同步抛UNSUPPORTED_CAPABILITY；提交后的业务失败由 wait 抛同一
native error，保留 code/domain/boundary/requestId/attempt，不改写为成功空结果。
现有 NativeDiError 已提供结构化字段；本期不强制大范围改 enum，不新增可误导的 retryable 布尔值。
新增本地码：`MODEL_NOT_READY`、`PREPARATION_NOT_IN_FLIGHT`、`PREPARATION_TIMEOUT`、
`PREPARATION_CANCELLED`、`CACHE_BUDGET_EXCEEDED`、`SOURCE_IDENTITY_MISMATCH`、
`RUNTIME_CLOSED`、`WOULD_DEADLOCK`、`UNSUPPORTED_CAPABILITY`；错误边界分别标 preparation/cache/lifecycle/input。

## Handle Compatibility

wait() 等到 native request 终态，受 request deadline 限制；wait(timeout) 只限制本次等待，
超时不 cancel，仍可再次 wait。RequestStatus 保留 Pending/Succeeded/Failed/Cancelled，
业务 deadline 以 Failed + native timeout code 表达；不凭空新增 EXPIRED 或 ASSEMBLING 公共状态。
observe 委托现有有序通知/历史回放契约，不另开无界队列；迟注册、observer 异常和慢消费者
行为以现有 native handle 为基线，在 T006 对照冻结。新接口不能悄悄改变旧回放结果。
applicationRequestId 仅日志关联字段，不能取代由 native owner 分配的 ID、attempt 或重试凭据。

## Documentation Obligations

新增公开声明使用英文 Doxygen 明确参数来源/单位、返回是提交还是完成、异常、ownership、
thread safety 和 callback 禁止阻塞规则；Python docstring 写明 GIL 释放和 native 生命周期。
不得用 generated signature inventory 代替以上行为契约。
