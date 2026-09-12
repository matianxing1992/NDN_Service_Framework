# Quickstart

**Status**: DESIGN_EXAMPLE / NOT_COMPILED。此页解释目标用法，不是当前可运行API。

```cpp
namespace di = ndnsf::di;
di::RuntimeConfig config;
config.nativeConfigPath = "/operator/native-requester.json";
auto runtime = di::Runtime::open(config);
auto user = runtime->user();

// Application supplies operator-pinned model/catalog once.
di::PrepareRequest preparation;
preparation.model = pinnedModelDescriptor;
preparation.taskName = "text-generation";
preparation.taskContract = pinnedTaskContract;
preparation.inputLayoutDigest = pinnedInputLayoutDigest;
preparation.catalogConfigurationJson = pinnedCatalogJson;
preparation.localSourcePath = "/models/canonical.onnx";
auto model = user.prepare(preparation); // default UseOrFetch; bounded wait

auto first = model.request(di::Input::inlineBytes(firstEncodedPrompt));
auto result = first.wait();
auto second = model.request(di::Input::inlineBytes(secondEncodedPrompt));
second.cancel();
runtime->close();
bool released = runtime->drain(std::chrono::seconds(5));
```

示例的pinnedModelDescriptor/pinnedCatalogJson及prompt bytes由现有operator配置和任务schema提供，
不是框架自动猜测的变量；T011交付使用真实配置loader的可编译example。
`RequireReady`在cold miss抛MODEL_NOT_READY；`wait(10ms)`只限制本次等待，之后可再次wait。
任务不支持会话时openConversation抛UNSUPPORTED_CAPABILITY，不静默退化成无状态调用。

## Planned Validation Procedure

1. T001核对Waf实际target、已验证build tree与native配置，记录output和source identity。
2. 每批先完成逐任务及组合静态门，然后构建受影响DI target，用该批注册的Spec185 C++ selector运行。
3. T011使用C++独立authority/requester/provider跑unary和stream；通过后才执行T012的Python薄封装检查。
4. T013运行C-04全部组合模式和反例，保留每阶段第一失败边界与cleanup，不以startup ready替代结果。

此时尚无Spec185 binary/selector，故不提供一个会误跑旧build的伪可执行命令。
