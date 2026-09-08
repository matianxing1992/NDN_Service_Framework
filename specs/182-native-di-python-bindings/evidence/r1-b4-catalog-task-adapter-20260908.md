# R1-B4 Catalog Task Adapter

## Design

基线 49a142af。C09 NativeModelAdapter 目前只有测试派生类；本批增加共享
NativeCatalogModelAdapter，作为明确注册的生产实现，不根据模型名称猜测模型家族。
构造输入为可信 bootstrap 提供的一组完整 NativeModelDescriptor、显式输入格式与
正数最大字节数。目录按 modelName/contentDigest 精确查找；同一 adapter ID/version
可包含多个模型修订，缺失、身份冲突与重复拒绝。构造后数据不可变，读取无 I/O，
由 shared_ptr<const NativeModelAdapter> 在既有 registry/preparation 中共享。

OpaqueBytes 用于已经编码的 Qwen pipeline 数据，保持所有二进制字节；JsonBytes
用于已编码 JSON 任务，在原生库校验 UTF-8/JSON/重复键等格式后返回原字节，不再次
把它编码成 JSON 字符串，也不改变调用方已绑定的 wire 字节。两种格式在 encodeInput/
decodeResult 都执行构造时冻结的大小限制。复用现有 nativeParseJson，不引入第二套
JSON 实现。Python facade 的对象/字节转换仍属薄绑定，不能承担 DI 决策。

inspect 返回完整匹配描述符；目录描述符不替代模型源认证或 ONNX 检查。
当前 NativeModelAdapter 接口不携带 task options，options 的传递/校验继续归原生
requester/preparation 接线，不能宣称本批关闭完整任务契约。

## Verification

**CA-1/CA-2 DONE (batch only)**：单次增量 -j4
[build](../../../.codex-tmp/spec182-r1-b4/build.log) 15.489s PASS，仅新 adapter 与对应
测试 Compiling、最终 Link；[C++ tests](../../../.codex-tmp/spec182-r1-b4/focused.log)
45 cases/944 assertions PASS；[design](../../../.codex-tmp/spec182-r1-b4/design.json) PASS。
未改变既有 class layout、Core/UAV 或依赖，不执行 fresh rebuild。
本批提供可实际注册的生产 adapter；未声称默认 requester、真实图源、role port 或
task options 已闭合。下一步实现实际 source/catalog→角色生产。

期间其他会话提交 fc45ea81，保留其 D-DESIGN-DIAGRAMS DONE 行与证据；
两份未提交 native design 不纳入本批。

CA-1/CA-2 IN_PROGRESS；新增 C++ case 直接注册生产 adapter，通过 prepareInput/
inspectModel 验证真实消费；覆盖两种格式、UTF-8、精确修订、目录重复与异家族、
错误 JSON、大小边界和输入/结果字节不变。真实图源由现有 inspect port 提供；测试
port 返回明确合成图，不声明完成实际 catalog/Repo 认证。统一构建前完成官方静态门。

CA-1/CA-2 STATIC_PASS / TESTS_DEFERRED：已加载官方 review-agent，当前执行者只读
审查新 header/source、完整 test diff、registry/preparation 消费及 Waf library/header/
unit target 注册。No findings. 值拷贝切断目录和返回对象的可写别名，assignment 禁止
修改已构造 adapter。既有类型 ABI 无变化；新源/头由 Waf 原有 glob 纳入。
组合审查 READY_FOR_BATCH_TESTS；只更新新 adapter 和本套测试，Core/UAV 对象复用。
Context Mode active hash 过期，使用 canonical repository/CodeGraph fallback。
