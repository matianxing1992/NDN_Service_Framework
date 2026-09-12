# C-05 API Surface, Usability and Extension Contracts

**Status**: PLANNED / 2026-09-12 revision。此契约把全API审查结论纳入185；C-01/C-03同步修订，
新接口尚未实现，不允许直接把所有Native头改名或删除旧Python包。

## Surface Levels and Export Policy

| Level | Target entry | Allowed responsibility | Compatibility |
| --- | --- | --- | --- |
| application | C++ `ndnsf-di/api.hpp`；Python `ndnsf_distributed_inference.api` | Runtime/User/PreparedModel/Input/RequestHandle/Result/Conversation/Errors及有限值类型 | Python现有api旧名字保留显式deprecated aliases；新例子只用canonical入口 |
| provider | C++ `ndnsf-di/provider.hpp`；Python `ndnsf_distributed_inference.provider_api` | Provider配置、serve、registration、stop/drain | 原NativeInferenceProvider与PythonInferenceProvider保留迁移映射 |
| extension | C++ `ndnsf-di/extensions.hpp`；Python原sdk明确支持清单 | 只读model/candidate/offer view、proposal、adapter/runner工厂和注册 | sealer/worker/key/commit不是插件自由操作；不再用star exports声称全稳定 |
| authority/admin | 独立operator头/模块与可执行程序 | 独立签名策略、发行、部署管理 | 不进入application/provider umbrella，不为清理API删除已有authority功能 |
| internal | 实现头、worker协议、journal内部、fault hooks | runtime内部协作 | 不承诺应用兼容；已有外部合法消费者先迁移，不一次全删 |
| compatibility | 既有root/client/app_sdk/facades路径 | 显式旧签名→同一owner | 签名/单位/失败默认保持；warning、使用记录、替代与退出条件明确 |

新installed umbrella必须只依赖安装闭包中的稳定声明，Native*是内部实现名而非用户必学术语。
T015建立 `contracts/api-exposure.json`：每个已安装头和Python导出记录path/symbol、层级、stability、
target、owner、replacement、transitive_dependencies、removal_condition；清单不是按文件名一刀切。
`api-inventory.json`是本次完整声明索引，不能把其自动family值当最终exposure决定。

## Simple Model Selection and Configuration

```cpp
struct ModelRegistration {
  std::string key;
  std::string nativeConfigPath;
};
// Added to RuntimeConfig:
// std::vector<ModelRegistration> models;
// Added to User:
PreparedModel prepare(const std::string& modelKey = "default",
                      const PrepareOptions& = {}) const;
```

`default`绑定RuntimeConfig.nativeConfigPath已有requester-v1的catalog+request；models额外登记
key→该同schema配置文件，每个key非空、无重复且不能重定义default。路径相对于主配置目录。
open解析并固定配置内容，所有配置的core principal/trust domain/grant authority必须与Runtime一致；
不一致在open失败，不能借换模型切换用户权限。source bytes昂贵工作仍在prepare发生。
prepare(key)从注册项解析ModelDescriptor、完整taskContract、inputLayout和source；不存在报MODEL_NOT_FOUND。
key只是本地别名，不进协议身份；key解析后的完整内容身份继续遵守C-02。
explicit PrepareRequest仍可用，但只在advanced头文档展示，普通quickstart不要求用户填写hash/JSON。
应用本身不生成authority key，不调用grant发行。配置校验不能通过缓存命中跳过。

## Ordinary Values and Capability Discovery

新application umbrella不把NativeInspectedModel/NativeRequestRuntime/NativeGenerationExecutionContract
定义传递给用户；内部Package保留它们。ModelManifest公开只读的模型/任务/双graph/配置身份，
与C-01一致；`PreparedModel::capabilities()`返回以下值：

```cpp
struct ModelCapabilities {
  std::string inputSchemaJson;
  std::string outputSchemaJson;
  std::vector<std::string> inputKinds;  // explicit: BYTES, UTF8_TEXT, REPOSITORY_REFERENCE
  std::vector<std::string> outputModes; // exact supported modes
  bool streaming = false;
  bool conversations = false;
};
ModelCapabilities capabilities() const;
Result run(Input, const RequestOptions& = {}) const;
```

capabilities由已验证adapter/task配置生成，不猜后端可用性，不把READY当远端capacity。
run精确等于request(...).result()，同一个deadline/错误/owner，不新建同步协议。
Input.text只接受有效UTF-8，在adapter声明UTF8_TEXT时由native encoder处理；不具备时显式
UNSUPPORTED_CAPABILITY，不能把字符串伪装成模型tensor或Python偷偷tokenize。
BYTES仍需符合已声明schema；不引入任意对象pickle。大输入引用保持完整protected metadata，见C-03。
新GenerationOptions只开放应用采样需求（本期maxNewTokens），tokenizer/布局/epoch/role等由operator契约固定。
现有高级generation设置不删除，留明确的advanced兼容路径；本期不保证所有模型都支持text/generation。

## Result, Error and Time Rules

C++统一std::chrono::milliseconds；Python新入口keyword-only `timeout_s: float | None`，单位秒，
拒绝bool、NaN、inf、负数，转换溢出拒绝，正数不足1ms向上取整。wait/result的0表示非阻塞poll，
prepare/request deadline必须>0；None只表示使用已有request/job deadline，绝不创建无限工作。
旧timeout_ms/timedelta接口仍按旧含义，仅compat层转换，不接受无单位的双义timeout。

RequestHandle::result() / result(timeout)是推荐的结果读取；wait两重载为完全等价便利alias，
不得像旧InferenceRequestHandle.wait那样返回状态。status统一为RequestStatus枚举，Python只读property。
Result始终表示成功结果，含payload/requestId/modelDigest/planDigest；失败抛DiError，
不再在新API返回status=false+error-string，也不丢native identity。
DiError保留native code/domain/boundary/requestId/attempt；类型/参数错误、局部等待超时、业务拒绝、
unsupported capability分别稳定映射；日志message不作机器oracle，不给所有错误一律retryable=true。

## Observation and Streaming

observe保持**best-effort诊断通知**，队列容量/丢弃计数可查询，不承诺完整token流或terminal必达。
`RequestDiagnostics { std::uint64_t observationDropped = 0; }`由RequestHandle::diagnostics()返回副本，
计数来自native owner，不接受caller写入；只计观测丢弃，不代表请求失败。
不得把当前publishEvent的满队列丢弃行为包装成无损Python生成器。
新可靠读取入口在C++定义 `EventReader RequestHandle::events()`，单handle最多一个active reader：

```cpp
struct StreamEvent { std::uint64_t sequence; Bytes payload; bool terminal = false; };
class EventReader {
public:
  std::optional<StreamEvent> next(std::chrono::milliseconds timeout);
  void close() noexcept;
};
```

reader创建从请求开始保留的有界native stream记录按序读取（序号来自验证后的native ingress），
不得从observe重建；stream关闭且读完返回nullopt，局部超时报WAIT_TIMEOUT，缺序/截断报STREAM_GAP。
容量1024事件、总16MiB，达到任一上限使落后reader显式gap；已提交request结果不因读者慢而降级。
新reader若最早事件已不可用立即gap，不静默从中间开始。close只停止读取，不取消请求。
普通非stream请求调用events报UNSUPPORTED_CAPABILITY；terminal frame不是会话durable commit。
原生结果/会话commit仍为权威；用户需要完成状态调用result。新Reader为move-only，持有必要owner lease。

## Python Mapping and Async

固定显式导出Runtime/User/PreparedModel/Input/RequestOptions/PrepareOptions/RequestHandle/Result/
Conversation/DiError及其值类型；不随optional模型依赖出现/消失。缺native运行依赖在open时明确报错，
不返回None或空占位实现。import不连接网络、启动线程或加载模型。

```python
with Runtime.open(config) as runtime:
    model = runtime.user().prepare("default")
    result = model.request(Input.inline_bytes(payload)).result(timeout_s=30)

async with Runtime.open(config) as runtime:
    model = await runtime.user().prepare_async("default", timeout_s=300)
    handle = model.request(Input.inline_bytes(payload), options=options)
    async for event in handle.events_async():
        consume(event.payload)
    result = await handle.result_async(timeout_s=30)
```

以上为DESIGN_EXAMPLE/NOT_RUN；config/payload/options由应用提供，stream示例要求options.stream启用。
prepare_async使用相同native准备job，Python协程取消只取消该waiter；result_async等待取消仅取消等待，
业务cancel必须显式handle.cancel。events_async协程取消关闭reader。不能用无界to_thread任务伪装异步。
按[C-06](cpp-first.md)，先实现C++prepareAsync/onCompletion/nextAsync，Python仅桥接Future和iterator；
不允许用Python线程或协程填补native异步能力，关闭和callback/GIL所有权也由同一native生命周期保证。
同步context exit调用close+drain，预算5秒；drain超时抛SHUTDOWN_TIMEOUT且保留安全owner供显式重试，
已有异常时不覆盖原异常而记录shutdown失败。async exit通过C-06原生drainAsync等待同一屏障，不阻塞event loop。
native操作释放GIL，Python callback获取GIL；callback不在native锁内执行，异常隔离且可观察。

## Extension Lifecycle and Budget

AdapterRegistry与RunnerFactory采用启动builder→freeze→read-only lookup；重复id/backend拒绝，
需要替换时仅允许freeze前显式replace。freeze后注册报REGISTRY_FROZEN；不得与执行并发写map。
shared_ptr只说明寿命；shared adapter/strategy需声明可重入，不能满足则由native owner串行调用，
不同mutable runner默认不同request独占，run/KV/promote/release/prefetch有排他及drain契约。

新支持的扩展入口增加合作控制：
`ExtensionControl { steady_clock::time_point deadline; std::function<bool()> cancelled; void requireActive() const; }`。
普通RequestOptions.placement使用opaque不可变PlacementStrategy注册句柄，由Runtime::placementStrategy(id)查询；
未知id报STRATEGY_NOT_FOUND，非合作注册在open/freeze阶段拒绝。它不是NativePlacementStrategy别名。
extension头定义CooperativePlacementStrategy，其proposeRoles保留现有全部只读参数并追加ExtensionControl；
内部adapter调用合作端口后交原validator。旧NativePlacementStrategy仅advanced兼容可用。
新受支持split/placement方法在现有只读参数后接 `const ExtensionControl&`；输出仍由独立validator处理。
旧接口保留trusted legacy adapter并标non-preemptive；其卡住时drain只能超时，不声称强制回收。
普通生产Runtime只接受支持control的已注册策略或内部有界built-in；新外部策略不得调用Core/authority。
合作控制不等于能抢占恶意无限循环；需要运行不合作插件时只允许独立进程与有界IPC/kill+reap，
该隔离宿主不在185新增范围。T016明确拒绝不支持control的普通动态注册，晚到输出不得提交。

## Installed Consumer and ABI Gate

T015/T011在**只有安装前缀**的外部目录逐头include与link，不带源码树-I、不复用内部DEFINES。
每个稳定/兼容安装头的transitive依赖完整；internal头不靠全装vendor/worker来掩盖公开面问题。
ONNX runner公开布局不能依赖未导出的macro：固定PImpl成员或由opaque factory隐藏具体class。
用相同installed工具链构造/析构、调用factory和读取结果，normal+ASan观察生命周期。
检查pkg-config Cflags/Libs与实际装载ELF；若旧布局改变必须重建必要ABI消费者并明确版本/迁移说明。
