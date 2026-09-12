# C-06 Standalone C++ API and Thin Python Binding

**Authority**: 用户在全API审计中明确要求：C++有自己完整独立入口与API，Python只是外包装。
本契约是185前置架构/验收约束；不允许以Python便利实现补齐C++缺失功能。

## Ownership and Entry Points

C++库独立提供Runtime配置解析、模型注册/准备、输入schema处理、异步提交、结果/流、
会话/恢复、Provider注册/runner准备、取消/关闭/诊断与错误。CLI仅组合库API；Python同样仅组合库API。
没有“C++返回半成品，由Python完成tokenize/plan/commit/recovery”的正常路径。

| Capability | C++ owner / public entry | Python allowed mapping |
| --- | --- | --- |
| 配置/模型注册 | Runtime::open, User::prepare/prepareAsync | 参数与路径转换，调用同一native loader |
| 请求/结果 | PreparedModel::request/run, RequestHandle::result/onCompletion | snake_case、Future/asyncio Future适配 |
| 可靠流 | native EventReader::next/nextAsync/close | iterator/async iterator协议外壳，不缓存第二份权威序列 |
| 输入/生成 | Input工厂、native adapter encode、task capabilities | bytes/str转换；不执行Python模型规划或tokenizer |
| 会话 | Conversation/NativeConversationCoordinator/journal | opaque checkpoint与异常映射 |
| Provider | Runtime::open(ProviderConfig), Provider::serve/stop/drain | 同一C++服务owner，禁止Python runner fallback计为native |
| 扩展 | 已注册C++adapter/strategy/runner | 可配置扩展identity，不以Pythoncallback实现必需native算法 |

## C++ Async Contract

异步能力首先在C++声明和实现，不能只在Python补 `asyncio.to_thread`。

```cpp
enum class PreparationStatus { Pending, Ready, Failed, Cancelled };
class PreparationHandle {
public:
  PreparationStatus status() const;
  PreparedModel result() const;
  PreparedModel result(std::chrono::milliseconds timeout) const;
  void cancel();
  CompletionSubscription onCompletion(std::function<void(std::exception_ptr,
                                      std::optional<PreparedModel>)>);
};
// User; advanced overload remains under advanced header.
PreparationHandle prepareAsync(const std::string& modelKey = "default",
                               const PrepareOptions& = {}) const;
// RequestHandle
CompletionSubscription onCompletion(std::function<void(std::exception_ptr, std::optional<Result>)>);
// EventReader; at most one outstanding next/nextAsync on the same reader.
void nextAsync(std::chrono::milliseconds timeout,
               std::function<void(std::exception_ptr, std::optional<StreamEvent>)>);
```

PreparationHandle代表一个waiter，不是整个single-flight job；cancel只移除自己，最后waiter才按C-02取消job。
同步prepare精确调用prepareAsync(...).result()；不会维护第二条准备链。
completion在native通知executor锁外执行，成功时exception_ptr为空且value非空，失败时相反；
未退订的每次注册调用一次，迟注册也有终态回放；异常隔离，注册后关闭也必须返回明确取消/关闭结果。
完成通知不是best-effort observe，不能被观测队列满丢弃。每handle最多64个完成订阅，超限同步拒绝。
EventReader.nextAsync的nullopt表示正常EOF，异常代表timeout/gap/关闭；与同步next共享同一cursor。
async提交不阻塞Core IO。结果与清理屏障仍不同，onCompletion不是drain完成证明。

Python prepare_async/result_async只注册这些native完成回调，再用loop.call_soon_threadsafe更新Future。
协程取消/loop关闭要解除Python回调引用，不阻塞native executor；准备取消只cancel对应waiter，
请求等待取消不cancel业务。GIL/loop引用与native owner在退出时明确释放，不开无限线程池。
Python events_async映射nextAsync，不从observe或Python list重建业务流。

## Native Subscription and Cleanup

```cpp
class CompletionSubscription {
public:
  CompletionSubscription(CompletionSubscription&&) noexcept;
  CompletionSubscription& operator=(CompletionSubscription&&) noexcept;
  ~CompletionSubscription(); // unsubscribe; never block
  void unsubscribe() noexcept;
};
// Both Runtime and Provider:
CompletionSubscription drainAsync(std::chrono::milliseconds timeout,
  std::function<void(std::exception_ptr, bool)> callback) const;
```

onCompletion与drainAsync返回move-only token，应用须保留到完成；复制禁用。
unsubscribe幂等且不取消业务/准备waiter；与dispatch以同一native锁决定是否已开始。
尚未开始的callback撤下并释放引用；已开始者可完成一次，token析构不等待它。
完成后释放callback及64订阅额度；Python trampoline持有可失效状态，失效后不访问已关闭loop，
Python对象释放仍须遵守GIL。C++ fixture验证退订/排队/执行/关闭竞争。
Python await取消先退订；prepare取消还cancel该waiter；request等待取消不cancel业务。

drainAsync等待同一清理屏障，不隐式close/stop；清理完成callback为(null exception,true)，
期限到为(null exception,false)，参数/内部错误通过exception_ptr返回；采用native timer/barrier，
不阻塞IO线程。退订只停止该次等待。async exit先close/stop再等待，false映射SHUTDOWN_TIMEOUT。
T002/T009实现，T013验原生，T012仅映射；request completion不是cleanup证明。

## Provider-Only C++ Entry

Provider不能为了获得Runtime而配置requester私钥、requester catalog或Python应用。
增加 `Runtime::open(const ProviderConfig&)`，只创建Provider所需的Core/IO/凭据/缓存owner；
该Runtime的 `provider()` 返回已配置Provider，`user()` 抛 ROLE_UNAVAILABLE。
现有 `Runtime::open(RuntimeConfig)` 是User配置路径；其 `provider(config)` 为显式组合入口，
不得因此要求独立Provider进程加载User专有配置。

ProviderConfig在provider.hpp为opaque可复制validated配置，提供
`static ProviderConfig fromFile(const std::filesystem::path&)` 和
`static ProviderConfig fromCommandLine(int argc, const char* const* argv)`。
后者仅给C++CLI组合器；普通Provider应用使用fromFile，不传raw argument list给serve。
解析与语义验证均在C++，同一内部parser输出同一validated配置。
本期文件格式固定 `ndnsf-di-native-provider-launch-v1`：
`{"schema":"ndnsf-di-native-provider-launch-v1","arguments":["...existing provider option tokens..."],"cache":{"max_artifact_bytes":1073741824,"max_artifact_entries":8,"assembly_job_timeout_ms":300000}}`。
arguments使用现有Provider CLI语法，路径相对配置文件目录；无shell展开；CLI路径相对启动cwd。
缓存项未给采用上述默认，未知顶层字段/负数/零上限/错误身份及生产禁用test/legacy选项拒绝。
这是operator/CLI兼容文件格式，不是普通应用request API，也不宣称已存在该schema。
校验后字段私有且不可变，C++库不依赖Python产生文件，纯C++fromCommandLine也能构造相同配置。

## Independent Installation and Examples

必须交付可复制构建的C++应用例：prepare+unary、async准备/结果、stream、两轮会话/恢复、Provider注册。
它们只包含安装的 `ndnsf-di/api.hpp` / `provider.hpp`，通过pkg-config链接
`ndnsf-distributed-inference`，不使用源码目录、PythonWrapper、测试私有头或_test factory。
DI_NativeRequester与di-native-provider迁移为这些公开库入口的真实消费者；不只加一个未被实际使用的demo。
外部测试fixture/oracle均C++。准备模型fixture与配置可由仓库提供或C++生成，不调用Python准备算法。
部署运行时的library/executable/transitive ELF/subprocess闭包不能包含Python解释器、libpython、
Python模型helper、Pythonplanner或Python恢复脚本；外部MiniNDN编排不属于native数据面。

## Acceptance Order

T015独立C++安装/ABI前置门 → T001–T011生产C++及定向行为 → T013完整C++进程资格
→ T012 Python绑定检查 → T014文档交付。T016在T005之前完成扩展注册/取消契约。
T012不能先于T013的C++能力闭合；T013不再依赖Python迁移，否则形成逻辑循环。
T013逐能力列 C++ public entry / installed example / C++ oracle / result / dependency closure，
任一必需C++行缺失即PARTIAL；Python类存在或测试数量不能关闭该行。

Python差异只限命名、语法、对象转换、GIL、异常和asyncio协议；行为差异必须作为bug或显式不支持，
不能作为“Python版功能”绕过本契约。其他扩展语言可在未来采用同一C++边界，本Spec不实现新语言绑定。
