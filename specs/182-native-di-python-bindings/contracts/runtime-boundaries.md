# Runtime Boundary Completion

**Revision**: 8 | **Status**: DRAFT / BLOCK for implementation
**Normative parent**: [spec](../spec.md), [code design](code-design.md)
本附件补齐 revision 1 的中间调用缺口；所有 Native* 新接口均 planned。
O-003 tokenizer依赖/ABI设计与O-005隔离设计已关闭；O-002完整ONNX算法已由
[native ONNX assembly design](native-onnx-assembly-design.md)设计关闭；O-004 静态
映射已于 2026-09-07 收口（UNREVIEWED 0，344 项 manifest，逐项带 nativeOwner/
ownerTask），字段/方法/错误 parity 按 owner 任务（T012 等）继续。设计关闭仍
不宣称可直接编译：T002-A 起按 T001-C 冻结的工具链与 selector 执行。

## CD-013 Preparation and Offer Admission

**Requirements**: FR-001,FR-002,FR-004,FR-009,FR-016。
**Owner**: T008；NativeRequestPreparation 拥有 I/O，NativeOfferAdmission 拥有 DI offer policy，
NativeModelAdapter 拥有模型输入/输出语义；Core 继续拥有网络包认证。

| Operation | Exact paths | Symbols / current counterpart |
| --- | --- | --- |
| ADD | NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.cpp | NativeRequestPreparation::prepareInput, inspectModel, ensureArtifacts；当前 _request_v3、CanonicalCatalogEnsurer 的运行时部分 |
| ADD | NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.cpp | NativeOfferAdmission::verify；当前 ProviderOfferTrustVerifier::verify_ack 和 provider view 构造 |
| MODIFY planned | NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp | NativeModelAdapter::inspect, encodeInput, decodeResult；显式补齐原 GraphAdapter/TaskAdapter 端口 |
| MODIFY planned | NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp; NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.cpp; NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.hpp; NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.cpp | 注册各自 native task/graph 实现；T001 必须先细化超预算批次 |
| RETIRE runtime use | NDNSF-DistributedInference/ndnsf_distributed_inference/artifact_deployment.py; NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/provider.py; NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/base.py | CanonicalCatalogEnsurer、ProviderOfferTrustVerifier、GraphAdapter/TaskAdapter 的默认运行时调用；离线声明按 inventory 分类 |

~~~cpp
// planned; all returned values own their bytes and identity references
NativePreparedInput prepareInput(const NativeModelRef& model,
                                const NativeApplicationInput& value,
                                const NativeRequestOptions& options);
NativeInspectedModel inspectModel(const NativePreparedInput& input);
std::vector<NativeSelectionRoleV3> prepareRoles(const NativeInspectedModel& model,
    const NativeSplitCandidate& candidate, const NativeRequestControl& control);
NativeArtifactBinding ensureArtifacts(const NativeInspectedModel& model,
                                      const NativeSplitCandidate& candidate,
                                      const NativeRolePlacementProposalV3& proposal,
                                      const NativeRequestControl& control);
NativeAdmittedOfferV3 verify(const ndn_service_framework::AckSelectionCandidate& ack,
                            const NativeOfferBindingContext& context,
                            std::uint64_t nowMs) const;
~~~

- preparation 构造时注入已认证 catalog/Repo 访问、publication 和 adapter registry 端口；
  不接受 Python callback，也不要求 caller 先用 Python 生成 graph/plan。
- prepareInput 验证 task/options/schema，调用 native encodeInput；只做 Request 必需输入准备。
  文本 encode 在 Qwen adapter 调用 CD-006，普通 bytes 不再编码；禁止重复分词。
- BeginCollaboration 后的 ACK_CLOSED 冻结才 inspectModel，保留当前 post-ACK
  graph/candidate 决策顺序。读取同一认证模型 revision，目录提示不能覆盖 digest。
- NativePreparedInput 拥有 task descriptor、完整 expectedModel、编码 bytes/已验证引用和单调 deadline；
  NativeInspectedModel 拥有 descriptor、graph、实际解析的 canonical source name/digest 和
  modelManifestDigest。InspectPort 返回完整 inspected model，不允许 preparation 合成来源名。
  canonicalGraphDigest 单独绑定 ONNX 装配身份；graph.graphDigest 是 adapter 的 planning
  graph 身份，不能强制相等。角色 recipe 检查前者，request/offer/candidate 检查后者。
  当前发布前仍必须存在实际 source name，本地 source inspection 与 request-scoped
  publisher owner 仍待接线；发布后 manifest 的验证与重新认证规则见 ensureArtifacts。
- prepareRoles 从 native catalog/recipe owner 获取候选完整角色契约，绑定 model/manifest、
  role/rank/artifact、adapter、节点与最低内存预算，再交给 proposeRoles；缺 port 明确失败。
  网络来源认证由既有 Core/catalog owner 完成，端口 DTO 本身不构成认证证明。
- NativePlacementStrategy::proposeRoles 是原生策略的必需虚接口，完整 V3 数据直接传入；
  requester 通过持有的 const strategy 基类调用，不 dynamic_cast 或硬编码默认类。
  实际 requester 调用尚待接线；独立 sealer 仍负责检查自定义策略的输出。
- V3 sealCore 直接使用 inspected model/candidate、完整 proposal/execution plan、admitted
  offers 和 Core owner 的原始 ACK_CLOSED digest；共用 validateRoles/feasibleChoices，
  不经旧简化 Provider view。grantView 对 admitted offer 保留 exact-reuse 可用性。
  无 caller 提供的 trusted=true；网络或本地可信配置的验证证据由对应 owner 创建。
- ensureArtifacts 在候选选定后、sealCore/grant 前执行；复用已存在 canonical 工件，
  或由原生 owner 完成所需 publication/encryption。
  NativeCanonicalArtifactPublisher 是可直接注入 ArtifactPort 的原生 Core owner。
  SourcePort 返回请求拥有的 const canonical ONNX/initializer 字节；publisher 先核对
  inspection object digest/大小和既有 canonicalOnnxSourceIdentity，再在 Core I/O
  context 内用同一 PreparedServiceRequest 发布 source、可选 initializer 和 root。
  NativeRequestControl.cancelled 是线程安全的 request-owner predicate，捕获状态须覆盖
  排队及执行中的回调寿命；preparation 与 Core I/O 可能并发读取它。
  publisher 的同步调用禁止来自 Core I/O 线程；等待使用原单调 deadline，并定期检查
  取消。排队工作在取消后释放源字节；已进入 Core 的一次加密调用由 Core 收尾，之后
  不发布下一对象，也不把迟到结果交给 requester。它不创建第二个 Face/密钥系统。
  返回内容须为 encrypted/success，字节数、明文摘要、scope、epoch、transport manifest
  和实际名字完整。稳定名字由配置 root/candidate/logical role/rank 形成有效 NDN name。
  可选 package manifest 和 layer manifest 元数据由原生模型 owner 配置，不按模型名猜测。
  发布后业务 manifest 变更须附 canonicalManifestJson 原始字节，并绑定 inspection 的
  source object digest/字节数、可选 initializer object digest/字节数、模型和 profile。
  artifactNameByRole 保存稳定工件身份，sourceByRole 保存实际 root fetch name；两者
  不能互相替代。1 MiB 以内的 root 须为 ACTIVE canonical-model-manifest-v1；其原始
  字节 SHA-256 必须等于 manifestDigest，不把 Core transport manifest 当成业务摘要。
  bindPublishedRoles 只更新 modelManifestDigest/recipeDigest，复用既有 canonical ONNX
  recipe 编码。sealer 先验证原 proposal，再对更新后角色重新检查 admitted offers 的
  device/residency 可行性；旧 recipe 的 exact-reuse 证明不能授权新 recipe。
  缺 root 的既有不可变目录路径仍要求 manifest 完全相同。此接口不替代本地 source
  inspection、Core 加密发布或 requester owner 的实际接线。
  发布 immutable 引用前，requester 必须用原始角色/已验证 offers 独立校验 V3 placement。
  ArtifactPort 只接收 inspected model、原 candidate、selected V3 roles 与 control；
  ensureArtifacts 检查 request/attempt/model/graph、完整 role/rank/assignment/offer identity，
  以及返回的每个 artifact digest 与选定角色一致；不构造旧 proposal。端口不是执行授权。
  只返回认证 manifest/recipe/input
  引用与清理 lease，不把 Provider 的角色 ONNX 装配提前到 Requester。
  维护入口的请求级加密 publication 复用 ServiceUser::prepareServiceRequest 和
  publishEncryptedLargeData；Core 返回 contentDigest 是明文字节摘要，manifestDigest
  是传输描述摘要，均不能按名字误认成业务 root 摘要。小型 requester app 签名记录仍
  复用 publishSignedAppData；canonical catalog 名字空间使用既有 Repo 端口，不能
  绕过 Core app 发布的名字空间限制。实际返回名、内容摘要、签名者与 model/recipe
  的绑定各自验证。
- NativeModelAdapter::decodeResult 在 native 终态结果构造前执行；YOLO task decode、
  Qwen text/token 结果映射均由 adapter 实现，Python 不重新运行后处理或判定业务成功。
  模型预处理/后处理仅迁移现有已支持行为；不暗中增加图像格式或新模型能力。
- verify 的 ack 必须来自 Core callback 的真实 ACK/provenance；验证 Trust Schema 结果、
  signer identity/key locator/wire digest、policy 的 Provider/service/key/candidate 绑定、
  offer 签名、request/model/graph/有效期和字段限额，然后才产生 admitted observation。
  Core 包认证与 DI candidate policy 不合并为“字段相等”；缺证据失败关闭。
- admission 构造时绑定既有 candidate policy 与 Ed25519 公钥注册表；public-key raw bytes
  的 SHA256 必须等于 signerKeyId。返回仅 owner 可构造的 NativeAdmittedOfferV3，完整保留
  ACK payload 观测；policy 不含 freeBytes/backends/residency 等观测值，signature 必须验证。
- wall-clock 用于现有签名/有效期字段；本地总预算使用单调 deadline，不因时钟回拨延长。
  O-004 固定字段、序列化和上限；production clock 不允许测试覆盖。
- 所有耗时 I/O/图解析在工作 executor；Core Face 调用投递回 Core I/O owner。
  prepare/inspect/ensure 失败或取消释放本请求临时数据和 secret lease；
  已对外发布的不可变记录不声称被撤回，只按既有 TTL/授权过期，不进入有效 Selection。

**Proof**: PO-013。原生 API 输入 raw application value，输出 oracle 一致结果；
篡改 ACK provenance/candidate policy、catalog identity、实际 publication name 均在指定边界拒绝。
记录 ordered events 证明 ACK_CLOSED 先于 graph/candidate，ensure 后于 placement、
Provider assembly 后于 Selection。不得让测试 harness 代做这些生产步骤。

## R5-B4 Native Runtime Configuration Boundary

The maintained requester may supply operator configuration through a thin
binding, but the configuration is parsed and validated by the native owner.
The schema is `ndnsf-di-native-request-runtime-v1` and contains only immutable
runtime policy and identity values:

```json
{
  "schema": "ndnsf-di-native-request-runtime-v1",
  "contract": {
    "service_name": "/service",
    "task_name": "task",
    "adapter_name": "adapter",
    "adapter_descriptor_digest": "sha256:...",
    "adapter_composition_digest": "sha256:...",
    "task_descriptor_digest": "sha256:...",
    "generation_mode": "TOKEN_DIAGNOSTIC",
    "tokenizer_digest": ""
  },
  "requester_identity": "/user",
  "protection_epoch": "epoch-1",
  "input_layout_digest": "sha256:...",
  "security": {"policy_digest": "sha256:...", "require_protected_artifacts": true},
  "budget": {"max_candidates": 1, "max_policy_ms": 100, "max_reentries": 1},
  "state_mapping": {"inputs": {}, "outputs": {}},
  "no_progress_ms": 5000,
  "max_segments": 4096
}
```

`catalog` and `grants` are native objects supplied by the composition root;
they are not serialized into this JSON. The native parser rejects an unknown
schema, missing or extra contract fields, invalid digest/identity/epoch values,
plaintext protection, zero or out-of-range budgets, and a runtime whose
catalog/grant owners do not match the contract and requester identity. It does
not read model bytes, private keys, or public-key files. Catalog source checks
remain in `NativeRequestCatalog::load`; grant key loading remains in
`NativeServiceUser::nativeGrantClientFromConfig`.

The Python facade may decode bytes and pass the JSON string, catalog and grant
objects through. It must not create a plan, verify ACKs, or provide a Python
strategy callback. Request execution still uses the catalog's bound C++ split
strategy and `NativePreSplitFirstPlacement` (or another C++ strategy), and a
missing runtime component fails closed.

## CD-014 Provider Host and Binding

**Requirements**: FR-001,FR-009,FR-010,FR-012。
**Owner**: T009；从 executable 提取 DI 接线，复用原 Provider runtime，不重写推理执行。

| Operation | Exact paths | Symbols / consumers |
| --- | --- | --- |
| ADD | NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.hpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.cpp | NativeInferenceProvider::serve, stop；NativeServiceRegistration::close；C++ consumer、native executable、Python bindings |
| MODIFY | examples/DI_NativeProviderExecutable.cpp | main 中 DI configuration/runtime/handler 接线转交 native host；Face/证书配置仍由应用创建 |
| REUSE | NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp | makeNativeProviderCollaborationRuntime、现有执行/准备/状态/cleanup |
| MODIFY planned | pythonWrapper/src/ndnsf/di_bindings.cpp | InferenceProvider/from_config/serve/run/stop 兼容转发，绑定 native runner 类型 |

~~~cpp
class NativeInferenceProvider {
public:
  NativeInferenceProvider(
      std::shared_ptr<ndn_service_framework::ServiceProvider> provider,
      std::shared_ptr<const NativeAdapterRegistry> adapters);
  NativeServiceRegistration serve(const NativeServiceDefinition& service,
                                  const NativeProviderHandlerConfig& config);
  void stop();
};
~~~

NativeServiceDefinition 的 service/roles/backend/能力来自已验证 native config；
配置引用 adapter/runner 是 C++ 实例，Python callable runner 不作为兼容默认。
NativeServiceRegistration 持有该 registration 与在途 handler 的共享寿命，close 幂等；
停止新 admission 后让已接受角色按既有取消/deadline/cleanup 完成，不销毁正在运行的 handler。
宿主不关闭共享 ServiceProvider/Face，不在 I/O 线程 join；异常不能越过 Core callback。
O-004 映射（2026-09-07 收口）证实现有 API 无公开逐服务注销/registration token；
exact config 类型按 O-004 记录。因此 NativeServiceRegistration::close 保持 planned
Core scoped registration 扩展（见 [lifecycle design](native-provider-lifecycle-design.md)
Registration Generation Decision），实现卡不得临场发明 Core 方法或包装全局 stop。

### Current Registration Boundary

多服务提取的共享lease所有权、精确字段/API及关闭期间清理见[provider lifecycle design](native-provider-lifecycle-design.md)。已识别固定lease入口覆盖和每服务独立表的冲突缺口；registration generation与真正关闭入口方案随 O-004 收口（2026-09-07）冻结为 planned Core scoped registration，见下文与 lifecycle Registration Generation Decision。

在审计基线中，`ndn-service-framework/ServiceProvider.hpp`提供`addService(serviceName, ackHandler, requestHandler, ServiceInvocationMode)`和`addCollaborationHandler(serviceName, allowedRoles, ackHandler, handler)`及重载，没有公开逐服务remove/unregister方法。`examples/DI_NativeProviderExecutable.cpp::main`分别注册execution lease服务与推理collaboration handler；其ACK路径调用`issueNativeProviderOfferV3`，准备路径注入`runnerPreparationFactory`及`generationTextDecoderFactory`，就绪后安装`makeNativeProviderCollaborationRuntime(...).handler`。这些接线必须整体提取复用，不能仅移动最终handler而丢失ACK、lease、readiness和权限检查。

因此`NativeServiceRegistration::close`目前是planned设计缺口，而不是现有Core方法包装。
T001-B 已在 [lifecycle design](native-provider-lifecycle-design.md) 冻结 registration
记录 owner（move-only ServiceRegistration + 共享 RegistrationState）、closed/generation
fence（Provider 单调 generation，ACK 发布/Selection 派发/CollaborationWorkFence 三处
检查）、晚到 ACK/Selection 处理、重复注册语义与共享 lease 服务寿命；所需新 Core API
以精确文件/签名/字段/调用链/PO 列入 T009 卡，不临场扩写。PO-014 必须证明关闭一个
registration 后不再接收新工作且共享服务继续可用，并核对已接收工作的清理；T009 在
其 Core 改动落地并运行 PO-014 前保持 BLOCK。

InferenceProvider 是 least-authority serving facade。ProviderAdminPort 的 stage/activate/
drain/delete、带凭证的修订生命周期与普通 serve 分开；T001 清单必须判断真实支持的管理
调用是否需原生替代并追加设计，不能悄悄合并权限或删除既有能力。
从 executable 抽出的 native host 必须由 CLI 和绑定共同使用，不保留第三套初始化路径。

**Proof**: PO-014。独立 native host 与绑定分别注册同一服务，真实 ACK/Selection/Response
达到现有 Provider；重复注册、unsupported Python runner、stop 与晚到 callback、
另一个共享服务仍可用均有区分性断言。移除 native host 接线或接回旧 APPProvider 必须失败。

## Cancellation and Observer Contract

CD-001/FLOW-002 的 cancel 不代表已向远端发送取消或已完成清理。
现有 ServiceUser::cancelStreamRequest(requestId) 只改变 requester stream 消费/终态，
其实现没有通用 remote role-abort。必须在原生串行 owner 上调用；不使用 ForTest 入口。
另有existing `ServiceUser::publishCollaborationData(targetProvider, requestId, keyScope, topic, payload) -> bool`，用于请求密钥保护的协作commit/rollback记录；它不是通用remote-abort。O-004应按现有调用方与Provider控制解码器映射具体控制，不把该bool当远端执行/清理完成的证明。

| Trigger | Required behavior | Explicit limit / proof |
| --- | --- | --- |
| cancel before Selection | fence native operation，禁止后续 grant/Selection/新 attempt，释放自有资源 | Core ACK/timeout 可晚到；只消费/忽略，不复活 |
| cancel after Selection | 本地只接受一个取消终态；stream 模式另调用 cancelStreamRequest | 已提交远端工作由已支持 control 或原 deadline/executionGuard 收束；不能声称立即停止 GPU/epoch |
| deadline / close | 以相同 gate 禁止新工作；清理 worker/observer；共享 Core 保持有效 | requester 终态与 Provider cleanup 是两个证据字段；deadline+cleanupBudget 内证明远端收束 |
| result(waitTimeout) | 超时仅结束此次等待，仍可随后取结果或显式取消 | 不把 local wait timeout 写成 request failure |
| observer throws / slow consumer | observer 异常隔离；业务结果不由 observer 决定 | 通知溢出记录独立 DELIVERY_OVERFLOW；不得宣称观察者收到完整 stream |
| business stream gap / overflow | 按现有可靠流语义失败，禁止结果宣称完整生成 | 区别于非权威 observation queue；PO-007 注入两类 overflow |

若现有受支持能力要求立即远端取消，O-004 必须先列出已经存在的 authenticated control
及 Provider handler，再接线和注册负例；本 Spec 不凭一个本地方法虚构新控制协议。
请求 handle 可早于 ACK 完成返回；requestId 只能来自 Core 分配或唯一 native request owner，
T001 固定创建时序。维护调用方如果已有 `wire_request_id`，只能通过
`NativeRequestOptions.applicationRequestId` 作为非权威 correlation；handle 同时暴露
`requestId()`（native owner/Core identity）和 `applicationRequestId()`，两者必须在
调用边界显式配对，不能把 caller 字符串写入或冒充 Core requestId。close 与 callback
的 join/unsubscribe 实现必须具备明确线程所有权。

## Migration and Rollback Contract

T001 compatibility manifest 每项必须包含：source symbol/caller/config、是否当前受支持、
原生替代 symbol、API/数据/wire version、状态读写边界、owner task、
旧 default 禁用方式、oracle、删除条件、rollback compatibility。
分类：NATIVE_REQUIRED、BINDING_ONLY、OFFLINE_REFERENCE、UNSUPPORTED_EXTENSION。
UNSUPPORTED_EXTENSION 只能用于被本次用户目标明确排除的 Python callback 扩展形式，
不可把未移植的已支持模型/会话/安全行为改标 unsupported 过门。

- Python callable strategy/runner 改为 native strategy/runner 对象是明确接口迁移，
  不是逐签名无损兼容。相同行为必须有原生实现；所有 maintained caller 迁移后才允许退役。
- T013 拥有旧路径生命周期：每项标记 defaultReachable=false、nativeOwner、
  disposition 和删除证据。日志/静态 import/运行 trace 联合证明 legacy invocations=0，
  在同单元删除无生产消费者的运行实现和构建/import/export 注册。
  只因离线 oracle 保留的文件必须不进入安装运行包；不得无限期保留可选旧生产 backend。
- callback/config/dynamic import、GUI、CLI、顶层包、构建清单都进入 inventory。
  保留 oracle 的旧行为测试与纯原生运行隔离，测试存在不等于默认路径仍可达。
- 切换是完整发布单元，不做逐 token/per-request 的隐式 fallback。
  回退选择最后已验证 commit+依赖+配置+可兼容 checkpoint；保留失败证据，
  不重置共享工作区或覆盖较新会话数据。
- 新旧 requester/Provider 可互通仅在冻结 wire/vector 与版本矩阵证明后声明。
  不支持的版本在 admission/read 边界显式拒绝，不在解码失败后自动降级。
- journal 使用原格式时验证新写旧读/旧写新读；如必须迁移，先复制备份、
  原子转换与写入新版本目录，单写者锁定，保留 crash/partial-write/restart 负例。
  没有降级读取证明时旧版本不得读取新 journal，回退使用先前快照并说明丢失的未提交 turn。

## Merged Security Ownership

合并Core已具备请求级机密性、ControllerVersion、服务撤销/权限刷新和RuntimeStatusStore。Core/Provider继续持有当前权限与generation fence；DI admission只读取认证快照，不能建立第二份可写撤销表或以相同字段绕过Core独立校验。会话journal与Core持久运行状态分别归属，路径/锁/恢复规则不混用。DI artifact grant与Core请求/响应密钥分别验证，不因为两者都叫grant/key就共享寿命或撤销范围。

新增/修改接口的完整职责、字段含义及注释要求见 [symbol design](symbol-design.md) 和 [value contracts](value-contracts.md)。O-004需核对合并后的真实注册、撤销、取消和callback generation接线，不能引用旧行号证明当前实现。

## Pre-Test Review Scope

源码对照设计的审查、任务内unit和全部实现后的integration/MiniNDN统一见 [validation workflow](pre-test-static-review.md)。本附件只定义技术契约，静态或文档检查不代替运行证明。
