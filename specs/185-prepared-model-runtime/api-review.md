# NDNSF-DI API Review

**Date**: 2026-09-12 | **Source**: d484a047bb38a1a03978a11208d335af9842b36a
**Verdict**: REVISE — 机制覆盖广，但普通应用入口尚不集中，安装与扩展契约存在具体缺口。
**Scope**: 全部项目自有 C++ 头/Python 包声明普查，公开导出与关键API家族的源码语义审查。
这不是所有内部方法的逐行正确性证明，也没有运行用户研究、编译、推理或性能实验。

## Coverage and Interpretation

本轮从当前源码重新提取 **76个C++头、139个Python文件、8500项声明**；其中2564项被解析为函数，
其余包含类、字段、枚举、别名，不能称为8500个独立用户API。另核对DI binding TU的225个导出候选行，
以及共享Core binding TU的876个候选行；后者不是DI导出数量。
逐文件来源、声明签名/行号、hash、exports/imports在 [inventory](evidence/api-inventory.json)，
便于阅读的范围索引见 [API surface index](api-surface-index.md)。
第三方vendor、构建副本、测试/实验脚本不算产品公开API；测试注入出现在安装头时仍作为问题登记。
不执行Python导入以免启动外部设施；动态__all__、宏、继承和binding链式声明须人工复核，未声称ABI全量验证。

| API family | Files / declarations | Judgment and disposition |
| --- | --- | --- |
| application-sdk | 19 / 1283 | 应压缩成普通应用入口；旧部署/协调接口留compatibility或operator |
| request-client | 3 / 143 | 保留异步handle架构，统一request/result/status/timeout语义 |
| planning-preparation | 26 / 473 | 拆分、放置是合理扩展点，普通request不传内部plan/grant |
| extension-model | 43 / 1316 | 按adapter/runner/策略分层，冻结注册与并发规则，不全塞进用户namespace |
| provider-execution | 16 / 651 | 保留受保护post-Selection执行；配置/工厂与普通serve分层 |
| conversation-persistence | 8 / 482 | 用户只管理Conversation；事务/journal/receipt构造保持内部 |
| security-authority | 16 / 214 | 独立authority/admin SDK，不能从application umbrella导出裸key操作 |
| runtime-contracts | 67 / 3614 | 多数为内部数据流、tensor、消息/状态值；维持内部契约，不逐一改名 |
| operations-diagnostics | 10 / 236 | 操作员/诊断面单列，fault/test hooks不进入稳定应用导出 |
| compatibility-experimental | 7 / 88 | 明确版本/替代入口/删除条件；不因“旧”就一次性删除 |

家族归类是导航，不代表同一文件里每个symbol属于同一个稳定层；T015建立逐导出symbol的allowlist。

## Findings

路径缩写C++为 `NDNSF-DistributedInference/cpp/`，Python为
`NDNSF-DistributedInference/ndnsf_distributed_inference/`；行号绑定本轮hash。

| ID / Severity | Evidence and user consequence | Revision / Task |
| --- | --- | --- |
| U01 HIGH | app_sdk/client.py:1096动态request、:2562预部署request，facades.py:243又有APPClient；同名对象/方法认知不一致 | application唯一Runtime→User→PreparedModel，旧入口显式compat。T001,T011,T012 |
| U02 HIGH | api/__init__.py同时导出DeploymentDefinition/Activation/Ref/RequestableDeployment；普通推理学习负担过大 | 按application/provider/extension/authority/internal/compat分级，稳定入口显式__all__。T015,T012 |
| U03 HIGH | C++ NativeInferenceClient.hpp:158–165允许缺preparation/admission构造，RequestPreparation.cpp:208才报未配置 | open/prepare完整验证；低层不完整构造不列普通用法。T001,T003 |
| U04 HIGH | 初版185 prepare仍要手填catalog JSON、task digest、inputLayout、NativeModelDescriptor | 新增model key普通入口，由operator注册配置；显式PrepareRequest仅advanced。C-05/T003 |
| U05 MEDIUM | client.py:98为毫秒result，:2333 wait返回状态且收timedelta，native result/185 wait返回结果；status有property/method/string/enum | 新API统一结果/状态/单位；保留旧形态adapter不静默改单位。C-05/T006,T012 |
| U06 HIGH | client.py:100–105把native结果转成仅bool/payload/request_id，丢model/plan摘要；InferenceResult同时bool/error字符串与异常 | 新Result保留identity；typed DiError跨绑定保留code/domain/boundary，不双重成功/错误通道。T006,T012 |
| U07 HIGH | NativeInferenceClient.cpp:613–615观察队列满直接丢事件；observe是非权威通知，不能承诺无损token iterator | observe标best-effort；可靠stream reader需native序号/显式gap，不能靠Python list模拟。C-05/T006 |
| U08 MEDIUM | Python现有result_async用to_thread(:2346)，新185只有阻塞prepare/wait，async应用仍需自己处理GIL/取消/close | 提供明确prepare_async/result_async/events_async及context manager，结果等待取消不暗中取消业务。T012 |
| U09 HIGH | NativeProviderHandler.hpp:51–139混合plan/assignment/factory/legacy/KV/raw lease；普通serve难正确组合 | operator配置→验证后Provider；内部factory扩展明确，业务不拼安全flag。T009 |
| U10 HIGH | wscript:623不安装CanonicalJson，但CheckpointExport.hpp:3/ConversationWire.hpp:3依赖它；CanonicalOnnxAssembler.hpp:7依赖未安装worker头 | 安装allowlist与transitive include闭包；installed-only逐头消费。T015 |
| U11 HIGH | OnnxRuntimeModelRunner.hpp:185–194的m_impl布局受宏控制，root wscript:504设宏而.pc.in:11不导出 | 固定opaque PImpl布局或隐藏具体runner；外部构造/析构ABI用例。T015 |
| U12 HIGH | wscript glob安装FaultInjection/security/coordinator内部头；ProviderHandler.hpp:70公开test mutation字段 | 应用umbrella不传递内部头；authority/test独立；保留合法旧依赖迁移窗口。T015 |
| U13 MEDIUM | NativePlanning.cpp:687–709 registry无锁freeze/map；NativeModelRunner.cpp:159–168重复backend覆盖 | 启动builder→冻结registry；重复项拒绝/显式replace，注册与执行不可并发。T016 |
| U14 HIGH | NativePlanning.hpp:265–284策略无cancel参数，Planner.cpp:328–354返回后才计时 | 策略预算不是任意代码抢占；合作取消与隔离策略明确，晚提交拒绝。T016 |
| U15 HIGH | NativeModelRunner.hpp:83–151同对象run/stream/KV/promote/release/prefetch；shared_ptr不表示线程安全 | per-request mutable state排他，immutable artifact与live runner分开；不安全backend禁template复用。T010,T016 |
| U16 HIGH | ConversationCoordinator.hpp:121–147与CompletedAttempt暴露requestId/attempt/commit回调 | 普通Conversation不接受内部事务控制，独立advanced/internal头。T007,T015 |
| U17 MEDIUM | app_sdk/__init__.py大量star imports、dynamic __all__且按缺依赖选择导出；compatibility/exports.py:40–58可返回占位对象/None | 新稳定包导出固定；缺可选能力显式异常；不重写历史root导出语义。T012,T015 |
| U18 HIGH | 初版185把Input.repository简化为name/digest/size并建议requester先fetch；当前request_native_reference:777–818保留完整protected reference，Provider负责解析/解密 | 引用对象保留完整协议metadata及信任/加密绑定，复用现有native接收owner；不能以易用为名丢字段。C-05/T005 |
| U19 MEDIUM | sdk/__init__.py同时导出placement策略、worker envelope、sealer、Provider grant/projection | 支持的扩展只暴露只读view/proposal，worker/sealer不作为策略使用说明。T016,T015 |
| U20 MEDIUM | header多数关键方法仅签名，语义藏在.cpp；缺输入schema/能力查询导致试错 | 加manifest/capabilities描述，完整默认/错误/线程/释放契约及可编译场景。T003,T006,T011 |

这些是API设计和静态风险；U10/U11需要实际installed-consumer测试才可声称故障复现或修复。
公开安全头本身不等于绕过权限；问题在最小权限边界、误用风险与承诺的稳定性。

## Comparison with Official Framework APIs

截至本轮在线核对，使用官方文档说明模式，不作性能、安全性或实现等价比较。

| Framework | Verified API pattern | Adopt / retain NDNSF distinction |
| --- | --- | --- |
| ONNX Runtime | InferenceSession(model)随后run(outputs,inputs)，有get_inputs/get_outputs等元数据入口 | 模型准备与每次输入分离、schema可发现；不把本地execution provider当NDNSF网络Provider。见[官方Python API](https://onnxruntime.ai/docs/api/python/api_summary.html) |
| Ray Serve | DeploymentHandle.remote返回可等待结果；同步result与async await分开，stream返回生成器 | model-bound handle、异步明确、options不修改共享对象；不复制Ray部署假设或增加另一套调度器。见[DeploymentResponse](https://docs.ray.io/en/latest/serve/api/doc/ray.serve.handle.DeploymentResponse.html)、[stream composition](https://docs.ray.io/en/latest/serve/model_composition.html) |
| Triton | 客户端infer/async_infer按model name调用，metadata/readiness/model-load属于独立管理方法 | 普通推理与管理分离、能力查询；其每次model_name也合理，因此不能声称所有框架都必须PreparedModel。见[官方HTTP client](https://docs.nvidia.com/deeplearning/triton-inference-server/user-guide/docs/_reference/tritonclient/tritonclient.http.html) |
| vLLM | LLM绑定模型，generate(prompts,SamplingParams)；面向生成任务的输入/选项 | 专用typed输入可作为adapter便利层，不能要求YOLO也使用chat接口。见[官方offline inference](https://docs.vllm.ai/en/v0.27.0/examples/basic/offline_inference/) |

因此修订方向有可参照惯例，但“用户习惯”在本轮是工程判断，不是经过用户测量的结论。
不追求方法名逐字相同：NDNSF的授权发现、分布式role、独立authority和会话commit仍有自身语义。

## Revised User Journey and Acceptance

普通路径：配置一次 → prepare(model key) → 检查capabilities（可选） → request(input) → result/events。
普通用户不提供plan、grant、epoch、role map、catalog JSON或digest；operator配置仍显式固定这些身份。
一次性调用可通过PreparedModel.run作为request(...).result()便利组合；不得另有执行器。
输入转换只支持adapter声明的schema，不承诺任意Python对象、自动图像预处理或开放式chat。

新增任务T015负责API曝光/安装/ABI，T016负责扩展生命周期；原14任务保留ID并补易用性出口。
T011用只有安装包的外部消费者验证冷/热prepare、普通请求、取消、错误、Provider注册；
T012验证固定import、Python async/context manager和兼容单位，不能只检查类是否存在。
T013在T012之前完成全部独立C++模式、安装消费和no-Python闭包；Python不得补齐缺失native功能。
原生prepareAsync/completion/EventReader/drainAsync及可退订token由C++拥有，详见[C-06](contracts/cpp-first.md)。
每行迁移保留原method signature和替代入口，未知维护caller时不删除。详细设计见[C-05](contracts/api-usability.md)。
