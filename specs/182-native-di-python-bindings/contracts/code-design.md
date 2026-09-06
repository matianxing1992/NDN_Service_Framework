# Code Design Contract

**Revision**: 6 | **Status**: DRAFT
**Normative parent**: [spec.md](../spec.md)
**Baseline**: [merged-source-baseline-r3.json](../evidence/merged-source-baseline-r3.json)；revision 1/2 evidence 只保留历史意义。

## Baseline and Evidence

existing 表示合并工作区当前可见代码，不代表 qualification PASS。实际源码位于 `/home/tianxing/NDN/ndnsf-integration-182`，branch `integration/uav-182-20260906`；HEAD `d4a5e39ce5b4a023f6e55d2440c60aa998983f8f`，MERGE_HEAD `4391af81cd24ff5510aa52b48ab9cec0fdec1ebb`。文本冲突已解决，但合并尚未提交且有后续修复；不能仅用 HEAD 表示当前源内容。逐文件身份见 [merged source baseline](../evidence/merged-source-baseline-r3.json)，核对结果见 [revision 3 evidence](../evidence/skill-and-design-revision3.md)。

已核对 ServiceUser 的 BeginCollaboration / CommitCollaborationPlan、RequestConfidentiality / RevocationState / RuntimeStatusStore、现有 Provider runtime、Python requester/planner/DTO 及 ONNX/tokenizer helper。源码所在工作区无索引目录，因此按仓库规则跳过 CodeGraph，使用精确路径、头文件与 AST。主工作区 active Context Mode health 因 managed plan 仍指181失败；本轮以持久文档和源码为 authority，不使用自动记忆推断进度。

合并已带入请求级加密、ControllerVersion、撤销/权限刷新及持久运行状态。这些是 existing Core 机制，182 必须复用；原生 requester、无 Python 冷装配/分词仍 planned。集成记录中 unit R1 为747/751，integration R1 为60/92，均有失败；NAC-ABE 46/46 和 Context guard 45 PASS 只证明各自范围。未将既有失败重命名为182迁移失败，也未重跑或修改合并修复。

[Symbol design](symbol-design.md) 和 [value contracts](value-contracts.md) 是本 CD 的规范性补充：逐类/逐方法/逐字段解释职责、注释、用法与未决边界。所有新签名均为 DESIGN_EXAMPLE / NOT_COMPILED；T001 必须冻结剩余叶子 schema/ABI 后才允许实现对应单元。

## CD-001 Public API

### File and Symbol Manifest

| Operation | Exact paths | Symbols / consumers |
| --- | --- | --- |
| ADD | NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.cpp | NativeInferenceClient, NativeInferenceHandle, NativeRequestOptions, NativeRequestStatus, NativeInferenceResult, NativeDiError; C++ example / binding |
| REUSE | ndn-service-framework/ServiceUser.hpp; ndn-service-framework/ServiceUser.cpp | BeginCollaboration, CommitCollaborationPlan；Core 保留状态、ACK provenance 和网络 owner |
| REUSE | NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp | makeNativeProviderCollaborationRuntime；消费原生计划输出 |

以下为 planned 公开签名，before 均 absent；不宣称当前可编译。
NativeModelRef / NativeApplicationInput / NativeGenerationOptions 的字段来自当前
adapters/base.py、app_sdk/placement.py 和 adapters/qwen/generation.py 语义；
T001 必须逐字段冻结到同目录 NativePlanning.hpp 的类型表，O-004 关闭前禁止实现其序列化。

~~~cpp
namespace ndnsf::di {
class NativeInferenceClient {
public:
  NativeInferenceClient(std::shared_ptr<ndn_service_framework::ServiceUser> user,
                        std::shared_ptr<const NativeAdapterRegistry> adapters,
                        std::shared_ptr<NativeGrantClient> grants,
                        std::shared_ptr<NativeConversationCoordinator> conversations,
                        std::shared_ptr<NativeRequestPreparation> preparation,
                        std::shared_ptr<const NativeOfferAdmission> admission);
  NativeInferenceHandle request(
      const NativeModelRef& model,
      const NativeApplicationInput& input,
      std::shared_ptr<const NativeModelSplitStrategy> splitStrategy,
      std::shared_ptr<const NativePlacementStrategy> placementStrategy,
      const NativeRequestOptions& options);
  void close();
  ~NativeInferenceClient();
};
class NativeInferenceHandle {
public:
  std::string requestId() const;
  NativeRequestStatus status() const;
  NativeInferenceResult result(std::chrono::milliseconds waitTimeout) const;
  void cancel();
  void observe(std::function<void(const NativeInferenceEvent&)> observer);
};
}
~~~

| Parameter | Source / necessity | Bounds / ownership / validation |
| --- | --- | --- |
| user | 应用已配置并启动的 Core ServiceUser；不再重复传 group/controller identity | shared ownership；client 不私自启动第二个 Face；close 不关闭共享 user |
| adapters | 原生注册表提供模型差异，构造后只读 | 不允许模型名称分支散落于 client；未注册 adapter 在 Request 前拒绝 |
| grants | 已配置原生权威 port；保护请求必须有 | 不接受明文 key dict；grant secrets 禁止进入 handle/日志 |
| conversations | 可选的原生会话 owner | 无续接请求可为空；有 continuation 而未配置必须拒绝 |
| preparation / admission | CD-013 原生输入/工件 I/O 与 ACK policy owner；应用配置时构造 | 共享寿命，native ports；缺配置在 Request 前拒绝 |
| model | 调用者给出 immutable model/adapter 引用 | URI、revision、digest 按认证模型契约校验；不能只给可变名称 |
| input | application value、inline bytes 或认证 publication reference | 只允许声明 transport mode；禁止 caller 拼接伪造 REPO_REF |
| splitStrategy / placementStrategy | C++ 对象或由绑定生成的 C++ 对象 | 非空、immutable identity；无 Python trampoline；异步期间 shared lifetime |
| options | 调用者的 task、总预算、ACK 时间、生成参数、可选 continuation | 总预算毫秒 > ACK 毫秒 > 0；默认值由已验证 config 一处解析；不接受独立 planDigest/attempt 字段 |
| waitTimeout | 调用者本次等待时长 | 超过本次等待只报告 local wait timeout，不改变请求 deadline 或取消请求 |
| observer | C++ 或绑定层事件消费者 | 仅观察；执行于独立串行通知队列；异常隔离，不阻塞 Core I/O 或改变结果 |
| close | client 生命周期结束 | 阻止新请求，对存活请求发取消并清理自有资源；不可在 Core I/O 线程同步等待网络回调 |

NativeRequestOptions planned fields：task descriptor reference、timeoutMs、ackTimeoutMs、
optional generation options、optional conversation reference。请求 ID、attempt、deadline、ACK digest
均由 owner 生成/推导，禁止调用方提供互相矛盾的冗余值。测试可注入 clock/entropy 于非公开 test port，
生产 API 不暴露“跳过认证”“指定假身份”参数。

**Output/error**：handle 创建后的网络/规划/授权失败进入单一终态；参数类型/配置缺失在调用前抛
NativeDiError。其 code/domain/boundary/requestId/attempt 为结构化非秘密数据，
复用已注册 DI 原因码；原生异常文本不能被当作协议拒绝 oracle。
result 返回 payload 和真实执行/结果元数据，禁止把本地日志 marker 转为成功。
线程/取消见 [runtime boundary](runtime-boundaries.md#cancellation-and-observer-contract)；
现有 cancelStreamRequest 不是远端 abort。T001/O-004 固定非 stream 的本地 fencing 和真实 control 接线。

## CD-002 Strategies

| Operation | Exact paths | Symbols / before → after |
| --- | --- | --- |
| ADD | NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.cpp | NativeModelSplitStrategy, NativePlacementStrategy, NativePreSplitFirstPlacement, NativePlanningSnapshot, NativeModelAdapter, NativeAdapterRegistry |
| ADD | NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp; NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.cpp | NativeQwenLayerSplit::enumerate；对应 QwenThreeStageSplitter |
| ADD | NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.hpp; NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.cpp | NativeYoloComponentSplit::enumerate；对应 Yolo26Splitter |
| RETIRE default execution | NDNSF-DistributedInference/ndnsf_distributed_inference/planner/presplit_first.py; NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen/placement.py; NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/yolo/adapter.py | propose_v3 / enumerate_candidates；旧实现仅冻结对照，不由生产默认入口调用 |

~~~cpp
class NativeModelSplitStrategy {
public:
  virtual ~NativeModelSplitStrategy() = default;
  virtual NativeStrategyIdentity identity() const = 0;
  virtual std::vector<NativeSplitCandidate> enumerate(
      const NativeModelDescriptor& model,
      const NativeGraphSnapshot& graph,
      const NativeCandidateBudget& budget) const = 0;
};
class NativePlacementStrategy {
public:
  virtual ~NativePlacementStrategy() = default;
  virtual NativeStrategyIdentity identity() const = 0;
  virtual NativePlacementProposal propose(
      const NativePlanningSnapshot& snapshot,
      const NativeSplitCandidate& candidate) const = 0;
};
~~~

两个策略职责不同：split 指定图如何形成 roles；placement 指定 roles 由谁执行。
Python LayerSplit 等名字只是 native 类型绑定；不把 Python callable 包装后称为 native。

**Inputs**：model/graph 来自认证目录与 canonical 工件；snapshot 仅包含一次 ACK_CLOSED 的
认证 offers、其 digest、绝对 deadline 和不可变模型/图引用。
budget 对应当前 core/ports.py::CandidateBudget：maxCandidates、maxPolicyMs（默认100毫秒）、maxReentries（默认1）。
请求 deadline 属于 request control/snapshot，不是 CandidateBudget 字段；图节点/角色上限属于图与装配契约。
T001 冻结整数范围及上限来源；不能把字段改名当成预算语义等价。见 value-contracts V04。
strategy identity 是版本/参数规范摘要，不含可变全局缓存。

**Behavior**：
1. 检查模型与图身份；按 adapter 的已支持 cover 规则产生候选，禁止“只读离线切分结果”替代动态策略。
2. 过滤 role/backend/device/resource 不兼容项；residency 只影响兼容项排序，has_model 不等于精确驻留。
3. 保留当前已支持 role/rank cover 和确定性 tie-break，基于同一个时刻判断 lease。
4. 策略输出交给 CD-003 独立校验，策略不得授权执行、发布工件、创建 grant 或直接调用 Core commit。
5. 原生 registry 按 adapter ID 注入 Qwen/YOLO 实现；通用 client 不比较模型名称。

**Purity**：策略只读取输入，无网络/磁盘副作用。模型描述/图构建需要 I/O 时由 adapter preparation
port 在 ACK_CLOSED 后、planning snapshot 形成前完成，不能在 comparator 中加载模型。未经支持的 tensor parallel 配置
按现有能力清单显式拒绝，不在本次迁移中顺便增加算法。

## CD-003 Plan Semantics

| Operation | Exact paths | Symbols |
| --- | --- | --- |
| ADD | NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.cpp | NativePlanSealer::sealCore, grantView, finalizeSecurity, project, encode |
| MODIFY | NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.cpp | NativeSelectionProjectionV3；nativeSelectionProjectionV3FromJson；validateNativeSelectionProjectionSetV3；共用规范字段定义 |
| RETIRE default execution | NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/placement.py; NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py | PlanSealerV3；_request_v3 中依赖/投影构造 |

planned methods（全部 static；before absent）：
- sealCore(const NativePlanningSnapshot&, const NativePlacementProposal&) → NativePlacementPlanCore。
- grantView(const NativePlacementPlanCore&, const NativeProviderPlanningView&, const NativeSecurityPolicySnapshot&) → NativeProviderGrantView。
- finalizeSecurity(const NativePlacementPlanCore&, const std::vector<NativeGrantBinding>&, const NativeSecurityPolicySnapshot&) → NativeSealedPlan。
- project(const NativeSealedPlan&, const std::string& provider) → NativeSelectionProjectionV3。
- encode(const NativeSelectionProjectionV3&) → std::vector<uint8_t>。

参数都是经校验的 native immutable 值，provider 必须在 plan 中；grants 必须覆盖保护角色且无冲突。
deadline/requestId/attempt/modelDigest 从 core 派生，不允许 project 再接受覆盖参数。
NativeSealedPlan 持有已校验 dataflow/device/generation/authorization 投影输入；
具体字段和规范 JSON 字节表由 T001 从现有 PlanSealerV3 冻结，O-004 前不能自由添加字段。

canonical JSON、整数/字符串维度、UTF-8、摘要表示、role#rank、named tensors、scope、
grantName 和 signed payload 保持现有 wire 语义。共享源码不取消 Provider 独立验证。
每个 mayPublish/mustFetch endpoint 必须对应 adapter 声明的真实张量；
终端 owner 唯一，ingress 与普通依赖分开，token feedback 保持现有单 epoch DAG 规则。

**No wire redesign**：若 C++ 无法复现现有规范字节，先记录差异与版本化需求，重开 CD-003；
不能同时换 schema、重新封印旧 oracle 后声称迁移等价。

## CD-004 Grant

| Operation | Exact paths | Symbols |
| --- | --- | --- |
| ADD | NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantClient.hpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantClient.cpp | NativeGrantClient::acquire；原生 publication port |
| ADD | NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactPolicyAuthority.hpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactPolicyAuthority.cpp | NativeArtifactPolicyAuthority::issue |
| REUSE | NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantVerifier.cpp; NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.cpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedProvider.cpp | 现有验证、recipient credentials、secret lease 和 AEAD 消费 |
| RETIRE default execution | NDNSF-DistributedInference/ndnsf_distributed_inference/security/grant_provider.py; NDNSF-DistributedInference/ndnsf_distributed_inference/security/artifact_policy_authority.py; NDNSF-DistributedInference/ndnsf_distributed_inference/security/requester_grant_pipeline.py | AuthorityBackedGrantProvider.__call__；ArtifactPolicyAuthority.issue；Python requester pipeline |

planned acquire(const NativeProviderGrantView&, std::chrono::system_clock::time_point deadline)
→ NativeGrantBinding（工作 executor 等待，publication post 至 Face；deadline/cancel 终止等待）；issue(const NativeGrantRequest&, std::chrono::system_clock::time_point now)
→ NativeKeyGrant。认证证书、私钥 handle 和 issuer policy 在构造时注入；禁止把私钥字节作为 request 参数。

按已有 seal core → 签名请求 → policy 检查 → recipient encryption → 规范名 signed Data 发布 →
grant binding → finalize 的顺序。NativeGrantClient 使用现有 ServiceUser::publishSignedAppData，
不得引入新的网络 authority 服务作为迁移捷径。
进程内 authority 仍为独立职责；requester 不可绕过 policy 签发，Provider 不可把“字段相等”当验证成功。
key 只能经已有安全原语消费和零化；不自写替代密码算法。算法/字段复用 Spec170/181，
合并 Core 的请求级撤销、ControllerVersion 校验与权限刷新必须保持；DI 工件 grant 的独立撤销扩展和独立网络 authority 部署不由本迁移自动增加，不能混写为全部撤销尚未实现。

## CD-005 Assembly

| Operation | Exact paths | Symbols |
| --- | --- | --- |
| ADD | NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp; NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.cpp | assembleNativeCertifiedOnnxModel |
| MODIFY | NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.cpp | prepareNativeCanonicalOnnxRole 两个重载；NativeCanonicalOnnxAssemblerOptions 移除 pythonExecutable/pythonModule/helperTimeoutMs；保留请求 deadline/cancel |
| DELETE from production path | 同上 NativeCanonicalOnnxAssembler.cpp | OwnedAssemblyHelper、runPythonHelper、helper request/result IPC；路径安全/资源检查由原生 adapter 保留 |
| KEEP reference only | NDNSF-DistributedInference/ndnsf_distributed_inference/native_assembly_helper.py; NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/onnx/executor.py | 原独立对照及离线工具；不安装到 native runtime |

planned assembleNativeCertifiedOnnxModel(const NativeCanonicalSource&, const NativeCertifiedRecipe&,
const NativeAssemblyControl&) → NativeCertifiedAssembly。
source 拥有认证 graph 和可选 external initializer bytes；recipe 来自 sealed role；
control 只传当前请求的授权/取消检查和资源预算，不传可绕过保护的 bool。
结果含装配字节、external entries 和身份；C++ Provider 继续拥有缓存、密钥消费与激活。

保留 node extraction、输入输出修整、external-data 单位置/相对路径限制、initializer 命名归一化、
确定性 protobuf 序列化、source/assembled/node 上限及 model checker 语义。
装配依旧在 Selection/真实 grant 后执行，不以全部离线预装配替代。
O-002 必须证明原生 ONNX/protobuf 依赖与既有装配固定向量逐字节一致；
在其 API/版本/算法差异关闭前，此单元 BLOCK。

## CD-006 Tokenizer

| Operation | Exact paths | Symbols |
| --- | --- | --- |
| ADD | NDNSF-DistributedInference/cpp/adapters/qwen/NativeTokenizer.hpp; NDNSF-DistributedInference/cpp/adapters/qwen/NativeTokenizer.cpp | NativeTokenizer::encode, decode；资源和取消边界 |
| MODIFY | NDNSF-DistributedInference/cpp/ndnsf-di/NativeStandaloneTokenizer.hpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativeStandaloneTokenizer.cpp | NativeStandaloneTokenizerOptions；makeNativeStandaloneTokenizerDecoder |
| MODIFY | examples/DI_NativeProviderExecutable.cpp | generationTextDecoderFactory；注入原生 decoder，不 fork Python |
| KEEP reference only | NDNSF-DistributedInference/ndnsf_distributed_inference/native_token_decode_helper.py | 冻结 tokenizer oracle；不属 runtime dependency |

planned NativeTokenizer(path, expectedDigest)；encode(const std::string& text) →
std::vector<int64_t>；decode(const std::vector<int64_t>& ids) → std::string。
tokenizer.json 在创建时验证 digest 并加载；const 方法不加载第二份 tokenizer 或每 token 重建进程。
特殊 token、normalization、BPE、byte fallback、UTF-8 和 skip-special 行为按实际 standalone
tokenizer 及冻结向量定义。返回 UTF-8；非法 ID、digest 或配置在 adapter 边界失败。
O-003 决定原生库/可能的 Rust C ABI、锁文件、许可、安装路径和线程安全；禁止虚构已可用的库。
若选择 C ABI，必须在设计清单新增源/头/构建和内存释放函数后才能实现。

## CD-007 Lifecycle

| Operation | Exact paths | Symbols |
| --- | --- | --- |
| ADD | NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.cpp | beginTurn, abortTurn, prepareCheckpoint, commitTurn, restore |
| REUSE | NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.hpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.cpp; NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp | runNativeEpochCoordinator；ConversationStateStore 在 NativeProviderRuntime.hpp 内声明，现有 Provider 状态 owner |
| RETIRE default execution | NDNSF-DistributedInference/ndnsf_distributed_inference/conversation.py; NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py; NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/runtime_journal.py | ConversationCoordinator；AutomaticStreamingHandle 的业务推进；Python journal authority |

planned beginTurn(const NativeConversationContinuation&, const NativeApplicationInput&) →
NativeConversationTurn；abortTurn(const NativeConversationTurn&, const NativeDiError&) → void；
prepareCheckpoint(const NativeConversationTurn&, const NativeCompletedAttempt&) → NativeConversationCheckpoint；
commitTurn(const NativeConversationTurn&, const NativeConversationCheckpoint&) → NativeConversationRecord；
restore(const std::filesystem::path& journalRoot) → void。
所有 token lineage、parent digest、attempt 和 checkpoint format 必须从现有契约映射，
未冻结的持久化字段/旧 journal 兼容列入 O-004，不能直接逐对象 dump C++ 内存。

### State Authority

| Owner / field | Creation / writer / readers | Transition / lifetime | Wire / persistence |
| --- | --- | --- | --- |
| Core pending call | BeginCollaboration 生成 request ID、ACK snapshot、网络 deadline | Core serial I/O owner；DI 只调用公开操作 | 既有 wire 不变 |
| DI request phase | NEW → PREPARING_INPUT → REQUESTING → PLANNING → COMMITTED → TERMINAL | client 自有串行 executor；Core callback post 后更新；晚到结果不得复活终态 | 内部 phase；不能覆盖 Core 网络状态 |
| attempt | 首次 1；仅已定义恢复策略可递增 | 旧 attempt 的结果不得覆盖新 attempt；取消禁止创建替代 attempt | 保持当前 attempt fencing |
| plan / grants | snapshot→sealed core→grants→final plan | 封印后 immutable；secret 不入 observer/journal | 当前 canonical wire |
| handle status | 从 DI operation + Core result 派生 | binding 只读；result/observer 不改变状态 | 非第二套权威 |
| Provider role/state | 既有 NativeProviderRuntime/ProtectedRuntime/ConversationStateStore | 原位保留授权、deadline、排队取消和零化 | 现有状态引用 |
| conversation journal | NativeConversationCoordinator 单写者 | 验证完整 checkpoint 后原子提交；崩溃恢复拒绝不完整后继 | 旧格式迁移由 O-004 定义 |
| notified event queue | 只存有界非秘密事件 | observer 不能阻塞 Core；通知溢出只记 DELIVERY_OVERFLOW，业务 stream 缺失另按既有语义失败 | 不擅自丢 token 后声称完整 |

策略/装配等耗时工作不阻塞 ndn::Face event loop。继续复用原生 epoch 调度，
不引入一个“统一状态机”取代请求方和 Provider 必要的独立权限边界。

## CD-008 Bindings

| Operation | Exact paths | Symbols |
| --- | --- | --- |
| ADD | pythonWrapper/src/ndnsf/di_bindings.cpp | bindDistributedInference；绑定 CD-001/002 的原生对象和错误 |
| MODIFY | pythonWrapper/src/ndnsf/_ndnsf.cpp; pythonWrapper/setup.py | 模块注册/可选 DI 链接；禁止再编译一份同名 DI 核心实现 |
| MODIFY | NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/application.py; NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py; NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/provider.py | InferenceApplication.request/request_preplanned；InferenceClient.request_model/request_task 与 APPClient.request/request_task；InferenceProvider/APPProvider 的兼容转发 |
| MODIFY | NDNSF-DistributedInference/ndnsf_distributed_inference/__init__.py; NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/__init__.py | 导出原生-backed API，去掉默认旧运行时 import |

planned bindDistributedInference(pybind11::module_& module)；
Python request/infer/result/cancel 及 Provider serve/run/stop 按 T001 capability manifest 转发；
Provider native counterpart 为 CD-014；受支持语义不得通过“不可映射”删减。
不支持的签名必须列出替代调用；不能悄悄转到旧 APPClient 或旧 Provider。
阻塞 native wait 释放 GIL；回调进入 Python 才获取 GIL，投递只在 observation queue，
对象销毁注销回调并保留在途 native shared lifetime。
不提供 Python override 的策略 trampoline。native 库与 C++ consumer 不链接 libpython；
Python extension 本身依赖 Python 是允许的，不混淆检查范围。

## CD-009 Build

| Operation | Exact paths | Contract |
| --- | --- | --- |
| MODIFY | wscript | 增加可独立链接的 ndnsf-distributed-inference 库、native headers 和依赖 closure；原有 core/adapter objects 按明确依赖复用 |
| ADD | NDNSF-DistributedInference/ndnsf-distributed-inference.pc.in | pkg-config 导出 include/link 信息；不导出 Python 依赖 |
| MODIFY | examples/wscript; tests/wscript | requester/provider 与测试链接同一 DI 库，退出复制源文件列表 |
| MODIFY | pythonWrapper/setup.py | 绑定链接已构建库，runtime source/ABI identity 一致 |
| ADD | specs/182-native-di-python-bindings/contracts/native-dependencies.json | T001/O-002/O-003 关闭后冻结 ONNX/protobuf/tokenizer/ORT 版本与校验和，当前尚不存在，不编造锁 |

Waf、测试驱动或离线导出可以使用 Python；“runtime 无 Python”不等于“构建工具无 Python”。
O-002/O-003 冻结后必须记录原生工具链、include/lib、RPATH、license、build/runtime hash；
禁止从 host venv 搬 .so 充当容器 ABI 闭合。SIF 构建不在本轮执行范围。

## CD-010 Migration

| Operation | Exact paths | Migration |
| --- | --- | --- |
| ADD | examples/DI_NativeRequester.cpp | CLI main 调 CD-001，模型/输入/config/strategy 参数，不要求用户提供内部 plan/projection |
| MODIFY | examples/DI_NativeProviderExecutable.cpp | 链接同一 DI 库，CD-005/006 原生 prepare/decoder 注册 |
| MODIFY | Experiments/NDNSF_DI_YoloAckDriven_Minindn.py; Experiments/NDNSF_DI_QwenAckDriven_Minindn.py; Experiments/NDNSF_DI_StreamedGeneration_Minindn.py | harness 可用 Python，被测 requester/providers 默认原生 executable；原错误/数值/清理 oracle 保留 |
| MODIFY | examples/python/NDNSF-DistributedInference/yolo_2x2/user.py; examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py; examples/python/NDNSF-DistributedInference/llm_pipeline/user.py; examples/python/NDNSF-DistributedInference/llm_pipeline/provider.py | 仅参数/输入/输出与绑定调用；退出模型运行和业务控制重复实现 |
| RETIRE default execution | NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py; NDNSF-DistributedInference/ndnsf_distributed_inference/runtime_v1.py; NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/facades.py; NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py | 旧执行不再默认可达，调用方先迁移；不要根据“无直接 caller”删除 callback |
| ADD | specs/182-native-di-python-bindings/contracts/compatibility-manifest.json | T001 产出每个公开签名、调用方、配置、旧数据和退出证明清单；O-004 当前 OPEN |

迁移顺序：原生等价实现 + 冻结向量 → 独立 C++ 路径闭合 → binding 转发 →
maintained callers 切换 → 阻断旧路径验证 → T013 删除无生产消费者的运行实现和包注册。
兼容字段、callback 形式变更、mixed-version 与持久化回退以
[runtime migration](runtime-boundaries.md#migration-and-rollback-contract) 为规范。
不存在长期 dual-default 或自动 fallback。测试 oracle/离线工具允许保留 Python，
必须显式分类，不能将生产算法改名为“工具”规避依赖检查。

## Call Flows

| FLOW / step | Caller → callee | Data / effect | Failure owner |
| --- | --- | --- | --- |
| FLOW-001 / 1 | C++ main 或 binding → NativeInferenceClient::request | 认证模型引用、输入、native strategies、已验证 config | client 参数错误，尚无网络调用 |
| FLOW-001 / 2 | client → CD-013 prepareInput → Core BeginCollaboration | native encodeInput；Core 产生 request/ACK authority | native 输入错误或 Core timeout → DI terminal error |
| FLOW-001 / 3 | ACK_CLOSED → CD-013 verify/inspectModel → snapshot → enumerate/propose | Core provenance + DI offer policy；认证图、预算；原生 executor | admission/strategy error → 不提交 Selection |
| FLOW-001 / 4 | client → CD-013 ensureArtifacts → sealer → grants → finalize/project | 请求/计划/权限绑定；同一权威生成各投影 | grant/seal failure → 清理 requester secrets |
| FLOW-001 / 5 | client → Core CommitCollaborationPlan | 原 ACK digest、roles/dependencies/assignment payload | Core 独立拒绝过期/越界/二次冲突 |
| FLOW-001 / 6 | CD-014 native host → Native Provider → ProtectedRuntime → native assembler/runner | Selection 后冷装配；device/model 由 adapter 表达 | Provider 拒绝、资源清理和退出证据 |
| FLOW-001 / 7 | Core Response → native adapter decodeResult → handle → observer | 单一结果/终态，Python 不重新判定成功 | late callbacks 忽略业务状态更新 |
| FLOW-002 | handle.cancel → local fence；stream 调 cancelStreamRequest；远端按已有 control/deadline → executionGuard | 本地取消不等于远端已停；分开记录终态/cleanup | 见 runtime cancellation 契约，禁止新 attempt/Selection |
| FLOW-003 | beginTurn → request → native epochs → prepareCheckpoint/commitTurn | lineage 绑定、原子状态提交、原生文本 decode | 旧 attempt/state 不能晋升，abort 保留诚实错误 |
| FLOW-004 | compatibility entry → native request | 无旧 coordinator、无 callback strategy | 不支持 API 显式错误，不能 fallback |

## Additional Normative Contracts

CD-013/014、取消/通知队列及旧路径回退完整定义于
[runtime boundaries](runtime-boundaries.md)。其字段、任务和证明与本文共同构成规范。

## Open Questions

| Open ID | Unknown / impact | Bounded investigation and acceptable result | Owner / blocked units |
| --- | --- | --- | --- |
| O-001 | 合并修复最终源身份和承接表尚未封存 | 读取合并 closure/handoff、实际 merge commit 与逐文件哈希，核对当前 unit/integration 首边界；确认181遗留能力/验证迁入182，不要求先跑完181旧完整资格 | T001；全部实现 |
| O-002 | 原生 ONNX extraction/checker/protobuf 是否复现既有精确字节 | 在固定 inline/external-data 与两种 role recipe 上比较；列出 native 调用、版本、许可、依赖和差异。精确相等或经明确版本化设计修订后才能关闭；最多两个候选方案 | T001；T002/T006 |
| O-003 | 可复用 native tokenizer 库/ABI/线程安全尚未验证 | 固定 tokenizer.json 的 ASCII、Unicode、special/byte fallback 向量，比较完整 ids/text；证明无 Python。冻结一种 ABI/依赖及内存所有权；最多两个候选方案 | T001；T002/T007 |
| O-004 | 所有旧公开 API/策略/会话持久化与调用方尚未穷举 | 有索引时 CodeGraph，否则精确源码 + AST/import/config inventory；按 runtime-boundaries 的 NATIVE_REQUIRED/BINDING_ONLY/OFFLINE_REFERENCE/UNSUPPORTED_EXTENSION 分类，补完整 types、状态、Core cancel/observer 接线和错误映射；不允许遗漏调用方或以未验证分支做基线 | T001；T002--T013 |
| O-005 | native runtime 隔离设计可行性 | 核对 Linux mount/process observation 能阻断解释器、libpython、旁路服务，同时允许 harness 在外部；T001 冻结工具、权限和白名单设计后关闭此 OPEN；T014 实现并用故意 helper 验证有效性 | T001 设计；T014 实现；T016 资格 |

每项 OPEN 是具体设计边界，不能宣称 READY 后留给实现 improvisation。
T001 的交付是关闭表、叶子签名、lock/compatibility manifest 和修订后的原子任务，
不是启动大重写。若两候选均不满足，记录证据、收窄受阻范围；不得减弱 FR。

## Change Control

当前签名和 manifest 是设计 revision 2；OPEN 影响范围为 BLOCK。
新增原生依赖/公有字段/状态 owner/wire format/调用方必须先修订 CD、T、PO。
Private helper 只可实现已描述职责，不能用 helper 名义增加 subsystem。
完整单元边界见 [work-units](work-units.md)，证明见 [proof-design](proof-design.md)。

## Pre-Test Review Obligation

源码对照设计的审查、任务内unit和全部实现后的integration/MiniNDN统一见 [validation workflow](pre-test-static-review.md)。本附件只定义技术契约，静态或文档检查不代替运行证明。
