# Runtime Boundary Completion

**Revision**: 4 | **Status**: DRAFT / BLOCK for implementation
**Normative parent**: [spec](../spec.md), [code design](code-design.md)
本附件补齐 revision 1 的中间调用缺口；所有 Native* 新接口均 planned。
O-002/O-003 依赖锁和 O-004 完整字段/兼容清单未关闭，不宣称可直接编译。

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
NativeArtifactBinding ensureArtifacts(const NativeInspectedModel& model,
                                      const NativePlacementProposal& proposal,
                                      const NativeRequestControl& control);
NativeProviderPlanningView verify(const NativeAckEvidence& ack,
                                 const NativeOfferPolicySnapshot& policy,
                                 const NativeOfferBindingContext& context);
~~~

- preparation 构造时注入已认证 catalog/Repo 访问、publication 和 adapter registry 端口；
  不接受 Python callback，也不要求 caller 先用 Python 生成 graph/plan。
- prepareInput 验证 task/options/schema，调用 native encodeInput；只做 Request 必需输入准备。
  文本 encode 在 Qwen adapter 调用 CD-006，普通 bytes 不再编码；禁止重复分词。
- BeginCollaboration 后的 ACK_CLOSED 冻结才 inspectModel，保留当前 post-ACK
  graph/candidate 决策顺序。读取同一认证模型 revision，目录提示不能覆盖 digest。
- NativePreparedInput 拥有 task descriptor、编码 bytes/已验证引用和单调 deadline；
  NativeInspectedModel 拥有 descriptor、graph、认证 canonical source identity。
  无 caller 提供的 trusted=true；网络或本地可信配置的验证证据由对应 owner 创建。
- ensureArtifacts 在候选选定后、sealCore/grant 前执行；复用已存在 canonical 工件或
  原生完成当前运行时所需 publication/encryption。只返回认证 manifest/recipe/input
  引用与清理 lease，不把 Provider 的角色 ONNX 装配提前到 Requester。
  publishing 复用 ServiceUser::publishSignedAppData 或已有 Repo 端口；
  发布名、实际返回名、ciphertext digest、签名者与 model/recipe 绑定分别校验。
- NativeModelAdapter::decodeResult 在 native 终态结果构造前执行；YOLO task decode、
  Qwen text/token 结果映射均由 adapter 实现，Python 不重新运行后处理或判定业务成功。
  模型预处理/后处理仅迁移现有已支持行为；不暗中增加图像格式或新模型能力。
- verify 的 ack 必须来自 Core callback 的真实 ACK/provenance；验证 Trust Schema 结果、
  signer identity/key locator/wire digest、policy 的 Provider/service/key/candidate 绑定、
  offer 签名、request/model/graph/有效期和字段限额，然后才产生 planning view。
  Core 包认证与 DI candidate policy 不合并为“字段相等”；缺证据失败关闭。
  禁止 caller 或策略自行构造 NativeAckEvidence 的可信实例。
- wall-clock 用于现有签名/有效期字段；本地总预算使用单调 deadline，不因时钟回拨延长。
  O-004 固定字段、序列化和上限；production clock 不允许测试覆盖。
- 所有耗时 I/O/图解析在工作 executor；Core Face 调用投递回 Core I/O owner。
  prepare/inspect/ensure 失败或取消释放本请求临时数据和 secret lease；
  已对外发布的不可变记录不声称被撤回，只按既有 TTL/授权过期，不进入有效 Selection。

**Proof**: PO-013。原生 API 输入 raw application value，输出 oracle 一致结果；
篡改 ACK provenance/candidate policy、catalog identity、实际 publication name 均在指定边界拒绝。
记录 ordered events 证明 ACK_CLOSED 先于 graph/candidate，ensure 后于 placement、
Provider assembly 后于 Selection。不得让测试 harness 代做这些生产步骤。

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
Core 是否支持逐服务注销及 exact config 类型由 O-004 映射已有 API；缺少能力时保持 BLOCK，
不能实现为全局 stop 他人服务或发明 Core 方法。

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
T001 固定创建时序。close 与 callback 的 join/unsubscribe 实现必须具备明确线程所有权。

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

CD-013/014、取消/观察、迁移回退与合并安全owner必须在相应运行检查前按 [S0](pre-test-static-review.md)逐真实代码路径审查。晚到callback、共享服务停止、publication失败和旧默认入口不可仅靠后续测试发现。运行时行为仍由原PO独立证明，静态PASS不是权限或数值结果证据。
