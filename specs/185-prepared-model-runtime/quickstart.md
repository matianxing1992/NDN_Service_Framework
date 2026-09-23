# Quickstart

**Status**: DESIGN_EXAMPLE / NOT_COMPILED。目标C++17 API，完整consumer由T011交付。

## Standalone C++ Application

```cpp
#include <ndnsf-di/api.hpp>
namespace di = ndnsf::di;
di::RuntimeConfig config;
config.nativeConfigPath = "/operator/native-requester.json";
auto runtime = di::Runtime::open(config);
auto user = runtime->user();
auto model = user.prepare("default");
auto capabilities = model.capabilities();
auto result = user.run(model, di::Input::inlineBytes(encodedInput));
auto preparing = user.prepareAsync("default");
auto sameModel = preparing.result(std::chrono::seconds(10));
auto request = user.request(sameModel, di::Input::inlineBytes(nextEncodedInput));
auto completion = request.onCompletion([](std::exception_ptr error, std::optional<di::Result> value) {
  // Consume the native completion; do not block the IO owner.
});
runtime->close();
bool released = runtime->drain(std::chrono::seconds(5));
```

输入bytes由应用按capabilities中的任务schema提供；default配置精确绑定模型/任务/source，无须每次传catalog或model descriptor。
文本输入仅在native adapter支持时用Input::text。示例是签名组合，T011补完整main、输入读取、异常处理和安装prefix构建方式。
局部result(timeout)不取消请求；prepareAsync/completion均C++实现。

## Standalone C++ Provider

```cpp
#include <ndnsf-di/provider.hpp>
auto config = ndnsf::di::ProviderConfig::fromFile("/operator/native-provider.json");
auto runtime = ndnsf::di::Runtime::open(config);
auto provider = runtime->provider();
// Register supported services using C-03 Provider::serve.
```

不需要User模型注册或Python入口。配置schema见[C-06](contracts/cpp-first.md)。
可靠EventReader、会话恢复与Provider serve完整例子归T011，不能用observe充当可靠token流。

## Thin Python Boundary

完整方法、值类型和C++/Python对应见[C-07 API清单](contracts/api-catalog.md)。下面是目标普通用法（NOT_RUN）：

```python
from ndnsf_distributed_inference.api import Runtime, RuntimeConfig, Input

config = RuntimeConfig(native_config_path="/operator/native-requester.json")
with Runtime.open(config) as runtime:
    user = runtime.user()
    model = user.prepare()
    result = user.run(model, Input.inline_bytes(encoded_input))
```

encoded_input由应用按该模型schema提供。Provider使用provider_api中的ProviderConfig/ServiceDefinition；Runtime仍用同一C++绑定。
观察回调或C++异步读取返回Subscription，需要保留到完成；可靠流不能用observe代替。确定性C++退出用close+drain，Python with组合相同原生方法。

Python仅转换固定导出、timeout_s、异常、上下文管理和asyncio。
异步准备/结果/迭代必须桥接C++ completion/nextAsync，不增加独立规划、缓存、会话或恢复。

## Planned Validation Procedure

1. T015用安装prefix外部C++ consumer核对头、依赖及ABI。
2. T001–T011按依赖推进：逐任务静态门→批末组合审查→共享C++构建/测试。
3. T013完成独立authority/requester/provider、C-06全部模式/反例/no-Python闭包。
4. T012再验证包装，T014同步设计与交付。
所有新selector目前PLANNED/NOT_RUN，不提供会误跑旧build的命令。
