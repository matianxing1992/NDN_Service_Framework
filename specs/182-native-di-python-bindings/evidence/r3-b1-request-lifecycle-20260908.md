# R3-B1 Default Request Lifecycle

## Scope and Batch

基线 b0b2f4cc。Owner：当前执行者；T010-A/B 的默认 model/input 请求调用链。
保留 R2 已验证组件，不以 preplanned 请求替代公开入口。

| Member | Implementation boundary | Status |
| --- | --- | --- |
| RL-1 | operation 拥有提交参数和策略，worker 使用同一快照执行 preparation，取消后不推进 | DONE (local batch) |
| RL-2 | 显式 service/runtime 配置与兼容请求 wire；Core BeginCollaboration/ACK 接入已验证的 prepare/place/seal/grant/group owner | DONE (initial request local batch) |
| RL-3 | Core commit/response、取消与 deadline 清理，配置化 CLI 和本地请求用例 | DONE (local batch; network validation deferred) |

同批实现依赖 RL-1→RL-2→RL-3；T010 原有验收前置不豁免。
每成员加载官方 review-agent 静态门，批末统一审查并构建 unit-tests，执行
Spec182NativeInferenceClient、Spec182ClientState、Spec182V3Placement（实际请求组合用例）及受影响
preparation/placement/grant suites。集成用例随编码编写，T016 运行。
新增公开配置/布局时记录 ABI 变化并重建受影响消费者；不无故重建未变 Core/UAV。

## Source Findings

## Final Local Result

**DONE (initial-request local batch only)**。131个 DI cases 的最终通过证据由
[r5 shared suite](../../../.codex-tmp/spec182-r3-b1-r5/focused.log)（130/131、2963/2975）和
[r6 failed-case retry](../../../.codex-tmp/spec182-r3-b1-r6/projection-retry.log)（1/1、282/282）
共同组成：重生成 SDK 派生 fixture 后只重跑失败单例，未重复其余130个通过用例。
131个用例的全部2975项断言均已有通过结果，不能把r5原日志改写为一次全绿运行。
另 [Core collaboration](../../../.codex-tmp/spec182-r3-b1-r6/core.log) 13/13 cases、183/183 assertions PASS。

独立 [oracle record](../../../.codex-tmp/spec182-r3-b1-r6/oracle-record.json) 全部 exit0：
request envelope 2条严格 SDK roundtrip/摘要；grant 4条签名与独立解封；projection 7条
dataflow/11个端点。源 wire 来自 r5 C++ 测试。CLI [入口记录](../../../.codex-tmp/spec182-r3-b1-r6/cli-record.json)
验证 help=0、usage=2、错误schema=1且不创建output；ldd无缺库并加载本次构建的DI库。
CLI成功真实模型请求尚未运行，不能以help通过替代该义务。

构建：fresh configure5.934s；r1失败506.101s、r2失败232.870s；r3首次完整构建
PASS126.721s，r4身份修复增量PASS46.092s，r5测试修复增量PASS30.971s。原始命令/
退出码均在各自build-record.json；此次新ABI对象及CLI已链接，Python binding仍属T012。
首次SIGSEGV保留；修正身份消费者与测试异步所有权后，隔离和共享重跑未再出现。
该结果不能单独证明首轮崩溃的唯一根因。

后续：R4 stream/session/feedback；T010整体、T012调用方绑定、T013旧路径退出、
T014/T015/T016完整集成与无Python验收仍未完成。无runtime配置的旧constructor继续
显式拒绝，不能将配置化入口的本地PASS推广给尚未迁移的调用方。

## Validation History

### Batch Validation Entry

r5 build PASS /30.971s；共享131 cases 中130通过，2963/2975 assertions通过，
无 SIGSEGV。剩余 `AdmittedPlacementSealsSdkCoreAndRejectsTampering` 12断言失败来自
未随 placement-v3 更新的 dependent projection-oracle.json。核对既有 SDK author
独立读取新 core_digest 计算 plan/dataflow，重生成此派生 fixture；无 C++ 源码修改，
不重编，r6 仅重跑失败单例，再执行 Core selector、CLI 与三个输出 oracle。

r4 build PASS /46.092s。隔离成功链
`Spec182V3Placement/PublicClientCommitsSignedOfferAndIgnoresLateTerminalCallbacks`
PASS：1 case/38 assertions（见 [success diagnostic](../../../.codex-tmp/spec182-r3-b1-r4/success-diagnostic.log)）。
空 ACK/取消例则 exit201：adapter fixture 只声明 `task`，测试使用 `inference`，
requestWire 正确拒绝。改为已声明 task，不修改生产验证。原始 client-diagnostic.log
保留；r5 仅增量编译受影响测试并重跑共享 selectors。此次成功只证明本地 fixture 链，
不是网络/跨进程、CLI真实模型或 Spec182 完成证明。

r3 build PASS /126.721s；focused 首轮131选中用例在5.548s 后 SIGSEGV，未验收。
已执行部分报告112/118通过、6失败，不能用部分计数关闭任务。保留
[focused record](../../../.codex-tmp/spec182-r3-b1-r3/focused-record.json) 与同目录 focused.log。
首边界复核：projection builder 仍将 candidate.contentDigest 与 V3 core.intent 比较；
修复同时验证 intent 和 sourceContent。NativeMerge offer author 更新为 SDK ModelRef
intent，重新签名的两条 fixture 由现有 author 脚本验证。client 失败断言增加原始
NativeDiError 暴露，成功链 fixture 的异步 preparation 捕获改为 shared ownership，
避免 fatal assert 后仍运行的 worker 引用栈上 Input。后续先隔离生命周期测试诊断，
再重跑受影响 suites；不将 SIGSEGV 直接归因为某个协议缺陷。r4 增量 build/test 日志
使用 `.codex-tmp/spec182-r3-b1-r4/`；原结果不覆盖。

r2 build exit 1 / 232.870s：[原始 record](../../../.codex-tmp/spec182-r3-b1-r2/build-record.json)。
首边界为 client 新测试漏 `<openssl/evp.h>`；补显式 include。测试前静态检查另发现
旧 ClientTestAdapter.inspect 返回空 descriptor，空 ACK/取消例不能进入 Core；改用真实
NativeCatalogModelAdapter，仅此例替换 registry，既有 fixture 行为保持。复审调用参数、
直接依赖头与 inspect→model identity；增量 r3 日志独立保存，仍未运行测试。

首轮 configure PASS 5.934s；build exit 1 / 506.101s。首个错误为新增 V3 lifecycle
test 少传 request 的第五个 options 参数；检查并修复 client 空 ACK/取消同类调用。
原始 [build record](../../../.codex-tmp/spec182-r3-b1/build-record.json) 与 build.log
保留；未完成链接/测试。先修复、复审这两处调用，再于原 ABI build tree 增量续建，
新日志写 `.codex-tmp/spec182-r3-b1-r2/`，不重新 configure 或重编已完成的未变对象。
首轮最后60个 vmstat 样本中51个存在 swap-in（峰值132 KiB/s），swap-out 为0；
按主机规则，下一次 r2 改为 `-j2`，不把少量换页宣称为 OOM 或测试失败。

新增 `Spec182V3Placement/PublicClientCommitsSignedOfferAndIgnoresLateTerminalCallbacks`：
公开 client→真实 worker→Core Begin→重新签名 offer/admission→原生 prepare/place/seal/
grant/project→Core commit→Response decode。覆盖成功、错误 Provider transport binding、
commit 后取消及已持有 response/timeout callback 的晚到重放。模型/source publication、
grant publication 和 Core 认证传输边界为 local fixtures，签名/授权/规划及 client 状态机
为实际实现；网络/进程间验收仍由 T016 承担，不把此用例记作 NFD 完整请求资格。

整批静态复核关联 RL-1 参数/终态、RL-2 request identity/规划/授权、RL-3 Core
pending/commit/response/清理以及 catalog/CLI、构建注册和新增正负例。之前记录的
intent/content、Buffer 类型与 publication allowlist 缺陷已在源码修复；剩余运行假设
交给本批测试，不豁免 binding migration、REPO_REF、stream/session 或 T016。
**READY_FOR_BATCH_TESTS (initial request local scope)**；所有任务仍 PARTIAL。

ABI 布局已改变，使用新 `.codex-tmp/spec182-r3-b1/build`，复用已验收依赖安装与 Rust
archive，仅选择 `unit-tests,DI_NativeRequester` 及其构建依赖，`-j4`；不构建 UAV
应用或重编第三方依赖。fresh 对象覆盖 DI、新 Core header consumers 和实际 CLI。
配置/构建命令及原始日志写入 `.codex-tmp/spec182-r3-b1/`。先构建，再运行 ClientState、
NativeInferenceClient、NativePlanning、V3Placement、Sealer/Preparation/CanonicalPublisher/
GrantIssuer/GrantVerifier/Group 和 Core collaboration selectors；独立 oracle 读取实际
C++ 输出。首次失败先保留原始边界、修复复审后在独立 retry 目录记录。

### Configured Client Callback Coverage

新增 `Spec182ClientState/ConfiguredClientClosesEmptyAckAndCancelsActualCorePendingCall`：
使用公开 runtime constructor、实际 worker 与 Core BeginCollaboration，等待真实 pending
记录后分别触发 Core 空 ACK 关闭、handle.cancel。检查 ACK_CLOSED 失败边界或 Cancelled、
Core pending 删除、重复 close/cancel 不复活请求。仅 source inspection 与 Core local-mock
transport 为 fixture，未伪造成功 Response；不声称网络认证或完整成功链已覆盖。
官方 review-agent 只读复核 constructor→preparation→Begin→ACK callback→worker failure/
cancel→markTerminal→CancelCollaboration，确认新断言落在这条路径；无新增控制性发现。
本次新增用例未编译或执行，上一轮 catalog syntax 不覆盖本次改动。留批末共享构建；
下一步仍需成功 commit/Response 与晚到回调覆盖，再关闭整批静态门。

### Catalog Configuration Coverage

已补 [CLI 配置契约](../contracts/native-requester-configuration.md) 与 `--help`。
新增 `Spec182NativePlanning/RequestCatalogLoadsPinnedSourceAndRejectsConfigurationDrift`
：使用既有真实 ONNX semantic oracle，经过公开
NativeRequestCatalog::load、原生 splitter 与 adapter，比较完整候选、检查输入格式与
长度拒绝；覆盖7类配置篡改、源字节损坏与取消。未运行，不授予行为 PASS。
本轮官方 review-agent 只读检查 loader、canonical catalog/role owner、现有 oracle
及新测试；完整回调覆盖仍缺，整批不授予 READY_FOR_BATCH_TESTS。
限定语法诊断扩展至新 catalog、CLI 和新增测试所在 planning translation unit，检查
首次编译的 JSON aggregate/公开接口类型；复用 r2 系统编译参数，`-fsyntax-only`，
不生成对象或运行测试。原始记录使用 `.codex-tmp/spec182-r3-b1-catalog-syntax/`。
结果：catalog 2.915s、CLI 9.920s、planning tests 4.495s，全部 exit 0；总计17.330s。
实际命令、cwd、退出码见 [record](../../../.codex-tmp/spec182-r3-b1-catalog-syntax/record.json)。
系统 g++9.4/ld2.34/Boost1.71 本轮复核。此诊断不包含 issuer 新策略或完整 client 回调，
也不证明新增配置测试运行通过；仍需统一构建、oracle 与完整回调验收。

基线 NativeInferenceClient::request 只保存 requestId/deadline，丢弃 model/input/options
与两个策略；dispatchOperation 无条件 NATIVE_REQUEST_PIPELINE_NOT_READY。
NativeRequestPreparation 已有真实 adapter 编码，但默认 client 尚未调用。
旧 placement.py::_encode_request 使用 DIRequestEnvelopeV2；原生 Provider 读取
schema、task.placement_profile、request_id、service、model_identity_hash、attempt
与 plan_deadline_ms。不能以随意新 JSON 格式替代。

## Request Wire Contract and Remaining Wiring

`NativeRequestContract` 由已固定 catalog 提供 service/task/adapter descriptor、composition
与 task descriptor 摘要；client 构造时复制，operation 持有 const owner，不借用调用方配置。
新增 constructor overload 要求 preparation/admission；原 constructor 的缺配置失败行为保留，
后续 CLI/绑定迁移由 RL-3/T012 负责。该 overload 尚不表示整条 runtime 已完成。

`encodeNativeRequestEnvelope(model,input,contract,requestId,attempt,deadlineMs)` 接收
adapter 已编码的 payload 与原 options，输出 SDK V2 canonical wire 及四种独立身份。
校验 task/adapter/schema 绑定，复用 canonical JSON/SHA-256/OpenSSL Base64；严格保持
4 MiB wire 上限。INLINE 禁止 reference，REPO_REF 禁止内联 payload 且要求加密引用字段。
REQ-REF wire 支持不等于实际取数完成：preparation 的验证/取数仍未实现。
client 在 submission 冻结 wall-clock wire expiry，dispatch 不刷新 TTL；编码失败有
requestWire 首边界，取消/超时后丢弃输出。生成恢复/会话扩展由 R4/T011 原契约接续。

**ABI**：NativeInferenceClient 新增 private owner，布局改变。共享验证时必须 fresh
构建受影响 DI/client 消费者及 Python extension；不能加载旧 layout 的 binding 或安装
consumer。NativeRequestContract/NativeEncodedRequest 为新类型；Core 源码本轮未修改。

**接线前定位并修复中的控制性缺口**：SDK model_identity_hash 是 ModelRef intent digest；
NativePlanSealer.cpp 的 V3 sealCore 却要求 context.modelDigest == descriptor.contentDigest，
artifacts/core 校验也使用 content 身份。必须显式区分 wire intent 与 source content，并
同步相关 sealer/artifact 校验和负例；禁止篡改 V2 wire 为 content digest 来让旧测试通过。
直接接续位置：NativeRequestPreparation::ensureArtifacts 的 context 检查与 result.modelDigest，
NativePlanSealer 的 V3 sealCore context 检查，以及 NativePlacementPlanCore::validate 的
artifacts.modelDigest 比较。prepareInput 的 content digest 则是源对象检查，不能一概替换。
Core cancelStreamRequest 只清理 stream，不会移除 pending collaboration；后续需复用
erasePendingCallWithTrace 的 timer/admission/key 清理并明确本地取消和远端停止的区别。
这两项属于 RL-2/RL-3 的接线前置，当前不授予完整 RL-2 STATIC_PASS 或整批测试许可。

### Identity Repair

已新增 `NativeModelDescriptor::intentDigest()` 作为 SDK ModelRef 身份的唯一 native
编码 owner；request encoder 复用它。V3 ensureArtifacts/sealCore 按 intent 核对请求，
制品 modelDigest 与 inspected source 仍按 content 核对；prepared input/source root
检查不变。core 新增 sourceContentDigest 作为 sealer 从 inspection 设置的内部核对
字段，不加入 SDK canonical wire；V3 core.modelDigest 为 intent。旧 snapshot overload
仍输出原有 content wire，默认 requester 不使用这个兼容入口。

NativePlacementPlanCore 新增字段亦改变 DI ABI，纳入同批受影响对象 fresh build。
SDK oracle generator 保留 sealer-python-oracle 历史输入，重新派生 distinct source/intent，
重签当前 V3 offers 并更新当前 placement-v3-oracle.json；15 placement/7 sealing 样例
生成 exit 0。这是对照数据作者步骤，尚无 C++ 与新数据对比 PASS。
已更新 preparation/publisher/legacy-sealer 混合调用 fixture 与 V3 source binding；新增
将 intent 冒充 source content 的拒绝用例，禁止两种身份相等的测试捷径。

### Core Local Cancellation

新增 `ServiceUser::CancelCollaboration(requestId)->bool`，调用方须在 Face I/O 线程；
仅移除现存 collaboration，缺失或普通 pending request 返回 false，不触发业务
success/timeout callback。调用既有 stream cancellation 和 erasePendingCallWithTrace，
释放 admission、取消两种 pending timer、清理 request key/nonce，并清除保留的
collaboration records/keys。显式清除持有的 scope/event key 字节；不承诺已清除其他
持有者的副本。此接口不发送远端终止确认，已开始的 Provider 执行仍需原协议的
deadline/lease 或后续应用控制；不能把本地取消记作远端停止资格。

新增 GenericDynamicApi/CollaborationStatus 的 open/ACK_CLOSED 两种本地状态回归，
检查 pending/retained data 移除、重复取消、晚到 commit 拒绝、两种 timer 不回调。
该测试直接构造已有 Core 状态，只证明本地清理；真实 requester/网络验收仍属 RL-3/T016。
源码审查已核对 cleanup owner、作用域、timer/nonce/admission 路径和线程约束；尚未编译。
后续 client runtime overload 已接这些调用，尚未验证；CLI bootstrap 与最终 Core/Provider
集成用例仍待补齐，不能用该源码接线记录代替运行证明。

### Runtime Composition

新增 `NativeRequestRuntime` 固定 contract、requester/epoch、protected policy、budget、
input layout 与具体 authenticated grant owner；client 复制配置。调用方式为
`NativeInferenceClient(user, adapters, runtime, preparation, admission)`；旧无配置入口
仍明确失败，T012/CLI 必须迁移到完整构造，不允许把旧入口算作已接通。

`planNativeRequest` 在 worker 上处理 immutable ACK closure：admission → splitter
枚举 → role preparation → placement → artifact publication → seal → signed grants →
group/dataflow projections → Core CollaborationPlan。按 candidate 原顺序，仅 typed
NativeNoFeasiblePlacement 允许继续；损坏契约/认证/发布错误不降级。累计 splitter 与
placement 运行时间受 maxPolicyMs 检查，候选数受 maxCandidates 限制。策略调用无法
强制抢占，超预算在返回后拒绝；operation 的独立 deadline 仍可先终止请求。
按 stage 将 dependency 端点展开为选定 ranks，保留 candidate 的 scope/layout 契约；
group key admission 只消费最终选中的 ACK，Core selector 返回这些原始 ACK 与 opaque
assignment，由 Core 再核验 ACK_CLOSED 成员。runtime 此阶段要求 protected grants。

client dispatch 完成 inspection 后投递真实 BeginCollaboration，传 DATA_V1 required
capability。Core ACK callback 投递 worker 规划，再投递 I/O commit；始终不在 Face
线程等待模型/crypto 工作。Begin 使用剩余总预算，Core plan 复用完全相同的 timeout；
wire/operation 的绝对截止时间不刷新。重复 ACK/晚到回调检查 phase/status。
Response 需来自 terminal Provider 的 Core transport evidence，worker 调 adapter
decodeResult；成功/失败/取消/超时共用单一终态。进入 Core 后的终态均投递本地取消，
并独立清理已知 key scopes，覆盖 Core 已先移除 pending 的正常完成情况。

新增 `Spec182V3Placement/RequestPlannerComposesAuthenticatedGrantsAndCoreAssignments`
组合用例：SDK signed offers + concrete native grant
issuer/client → Core assignment，检查 source/intent、terminal、fetch name 及取消后无新增
发布。Core ACK evidence 和 artifact/publication transport 在此仍是局部 fixture；没有
宣称网络认证、CLI 或完整 Core/Provider 跑通。原生 V2 wire/SDK oracle 仍待批末执行。

剩余：完整 runtime 配置的 CLI/catalog bootstrap、默认 client 的完整请求/竞态用例、
批次完整静态门、fresh ABI 构建及共享单测。REPO_REF 实际取数、TOKEN_FEEDBACK/
generation/conversation 按现有未完成契约推进，不因 unary 源码接线而删减验收。

## Retrieval Boundary

Context Mode project health PASS，active health exit 4（plan/tasks source hash stale）；
本批使用磁盘 pointer/tasks/contracts。CodeGraph 初次不支持 --max-nodes，改用
--max-files 后成功，但返回 staging 副本，源码判断改核 canonical 路径。
均为检索边界，不是产品运行失败；未启动集成或实验。

## Validation

### Catalog and CLI Bootstrap

已新增 `NativeRequestCatalog::load`：加载完整 SDK model descriptor，检查 owned ONNX/
initializer 字节与固定摘要，复用 Qwen metadata/Yolo source catalog splitter、canonical
preparation 和 state mapping。`NativeModelDescriptor::fromCanonicalJson` 拒绝未知或
有损字段，新增既有 SDK descriptor oracle 的输入方向验证。planner 在 prepareRoles
前调用 catalog.bindStateContracts；CLI 无独立规划实现。

新增 `examples/DI_NativeRequester.cpp` 和 Waf target，链接已有 native library，不复制
DI cpp。参数为 `--config FILE --input FILE --output FILE`；配置 schema 为
ndnsf-di-native-requester-v1，包含 catalog/core/grant/offer_admission/request/limits。
catalog schema 为 ndnsf-di-native-request-catalog-v1。模型文件/PEM/key 路径相对配置文件；
Core identity/certificate 必须已存在于 PIB，CLI 不生成身份。已有 Face 由共享 user owner
保活；SIGINT/SIGTERM 调 handle.cancel，输入输出有界，私钥/模型内容不写成功日志。
仅在成功结果后写 output。完整配置示例、help/config 拒绝门及实际 CLI 构建仍待完成。

静态发现：publication 会生成不同 manifest，原 issuer 仅允许 catalog manifest，导致
真实发布后授权失败。已新增 immutable publicationSources policy，只有已允许的原
manifest 所属 model/content/source/initializer/profile 全部匹配，且发布 JSON hash 与
签名请求的新 manifest 一致，才用原受信模型 key 为新 manifest 签发 grant。client
传实际 publication bytes，catalog 显式保留 packageManifestDigest。不动态扩充 allowlist，
不接受任意新 manifest。CLI 按已加载的固定 source 配置该策略；保留原 fixed-manifest
接口。新增 signed request→实际 Provider unwrap 及5种源身份篡改拒绝用例，尚未执行。
此处改动涉及 issuer config/layout，纳入同批 fresh ABI 验证。

### Bounded Compile Diagnostic

批内例外：新 requester/planner 连接 Core callbacks、aggregate role/assignment 类型和
DI 新布局，尚无编译器验证。先对 NativeRequestPlanner.cpp、NativeRequestEnvelope.cpp、
NativeInferenceClient.cpp、ServiceUser.cpp 做 `-fsyntax-only`，复用既有系统工具链的
实际 DI compile flags；不输出 object，不复用旧布局生成可执行程序，也不执行测试。
只为在补 CLI/bootstrap 前排除类型/声明边界错误；不授予 READY_FOR_BATCH_TESTS。
原始目录 `.codex-tmp/spec182-r3-b1-syntax/`；失败保存后再修复/重试。
首轮 NativeRequestPlanner.cpp exit 1 / 7.278s：ndn::Buffer 不接受 std::vector 赋值，
两个 assignment 字段失败，其余三个文件尚未检查。修复显式拷贝并审查 Begin/Response
相同边界，重试使用 `.codex-tmp/spec182-r3-b1-syntax-r2/`，不覆盖原始日志。
修复后 r2 四个源码 `-fsyntax-only` 全部 exit 0：planner 7.202s、envelope 2.175s、
client 7.430s、Core 10.465s，总计27.272s。原始命令与退出码见
[record](../../../.codex-tmp/spec182-r3-b1-syntax-r2/record.json)，诊断日志同目录。
g++9.4/ld2.34、系统 Boost1.71 已核对；/usr/local 无 competing Boost header。
修复包含 assignment、Begin request、Response decode 三处显式 ndn::Buffer/vector
边界拷贝。未生成 object 或可执行文件，未验证链接、ABI consumer 或运行行为。

RL-1 已加载 `/home/tianxing/.codex/skills/review-agent/SKILL.md`，只读检查
client 的 submission/dispatch/terminal/close 与完整差异、测试 friend 及 adapter。
发现编码跨过 deadline 时可能先记录 planning 失败，已补 worker 返回后的 deadline
复核和 timeout 分类；取消后丢弃结果，不持锁调用 adapter。复审 No findings。
这是 RL-1 静态结论，不是整批 READY_FOR_BATCH_TESTS。

新增 C++ 用例：提交后修改调用方 model/input/options 与释放策略；adapter 内取消；
编码期间超时但 timer 尚未投递。分别检查原输入编码、Cancelled 终态、Timeout 分类。
此时成功编码仍报既有 NATIVE_REQUEST_PIPELINE_NOT_READY，测试明确不当作请求成功。
后续新增 request wire C++ 用例（INLINE/REPO_REF、Unicode ModelRef intent golden、
固定 invocation ID、base64、原生 Provider offer 接收/过期不签名、错误输入拒绝），以及
离线 `tests/fixtures/spec182/check-request-envelope-wire.py` SDK 严格解码/摘要 oracle。
测试尚未执行。批末设置 NDNSF_REQUEST_ENVELOPE_ORACLE_OUTPUT 保存两条 wire 后运行
oracle；仅 standard-library golden 的独立计算已用于编写断言，不算 C++ PASS。
只读审查发现 Provider fixture signerKeyId 必须为 digest，已修正；完整 RL-2 审查仍待接线。

`git diff --check` PASS；新 oracle 的 ast.parse 语法检查 PASS；
`checklists/validate_design.py` exit 0 / ok=true /
errors=[]（仅文档一致性）。未构建、未执行测试。整批未闭合，T010 保持 PARTIAL；
本批文件保留未提交，两份并发 dependency/generation 文档不纳入变更单元。
