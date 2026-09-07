# Symbol Design and Usage Contracts

**Revision**: 7 | **Status**: DRAFT / PARTIAL
**Authority**: 本文补充 [code design](code-design.md) 与 [runtime boundaries](runtime-boundaries.md)。
本文定义 planned 代码结果；不是运行实现。完整 merged source baseline 见
[baseline](integrated-baseline.md)。revision3工作树快照仅作历史。未决依赖/嵌套 schema 在
[value contracts](value-contracts.md) 中逐项可定位，受影响实现不得标 READY。

## Coverage and Documentation Rules

本规范覆盖14个 CD：ADD/MODIFY 的类、方法、字段、CLI/绑定/构建入口；
REUSE 不重写 Core 的内部状态。NativeCanonicalOnnxAssembler、NativeStandaloneTokenizer、
NativeExecutionPlanJson、NativePlanning、NativeQwenPlanner、NativeYoloPlanner、
NativeOnnxRecipeAssembler、NativeProviderHandler、NativeEpochCoordinator、
NativeProtectedProvider、NativeGrantVerifier 是文件/模块简称，不因此虚构同名 class。

所有新公开声明写英文 Doxygen，包含下面各条 Purpose、参数、返回/错误、
ownership/lifetime、thread safety；绑定写同义英文 docstring。
private 状态与关键 commit/security/resource 分支写说明性英文注释。
普通循环下标/搬运临时量为 LOCAL_DETAIL，可在实现中选择；
request/attempt、deadline、digest、secret lease、缓存失效和队列计数不豁免。
删除项要求删除对应旧说明和更新调用例；REUSE 只核验上游注释与契约，不重注释整库。

## Class and Module Ledger

全部新增类 before=absent；目标头/源由对应 CD 全路径表定位。
跨异步拥有的数据一律复制/持有 shared lifetime，禁止借用 Python 对象执行业务。

| ID / CD / class | Purpose / why this owner | Owned fields / lifecycle | Methods / usage / code comment obligation |
| --- | --- | --- | --- |
| C01 / CD-001 NativeInferenceClient | 请求方唯一 DI 操作 owner；组合原有 Core，不代替 Core 协议 | user/adapters/grants/conversations/preparation/admission、operations、executor、closed；构造校验依赖，close 后拒绝请求 | constructor/request/close/destructor；注释说明提交不代表执行成功，共享 Face 不归 client 关闭 |
| C02 / CD-001 NativeInferenceHandle | 可复制的结果/取消/观察入口；不独立推进请求 | shared operation 引用；client 关闭后仍可读终态 | requestId/status/result/cancel/observe；说明等待超时不取消，observer 不控制成功 |
| C03 / CD-001 NativeDiError | 稳定非秘密错误值/异常，替代跨语言文本判断 | code/domain/boundary/requestId/attempt；不可含 key/payload | 构造时验证原因码；bindings 映射同一类别，不从文本反推原因 |
| C04 / CD-002 NativeModelSplitStrategy | 模型角色切分提案接口，不能授权 | immutable strategy identity；无网络状态 | identity/enumerate/virtual destructor；注释输入冻结、输出由 sealer 再校验 |
| C05 / CD-002 NativePlacementStrategy | 角色到 Provider/device 的提案接口 | immutable strategy identity；只读 snapshot | identity/propose/virtual destructor；不持有发布/授权端口 |
| C06 / CD-002 NativePreSplitFirstPlacement | 保留已支持 placement 排序与确定性 tie-break | 冻结参数，无 residency 权威缓存 | identity/propose；has_model 不证明 exact residency |
| C07 / CD-002 NativeQwenLayerSplit | Qwen cover/rank-one 默认切分；模型变化归 adapter | 已验证 split 参数 | identity/enumerate；未支持的 tensor parallel 拒绝，非本次新增能力 |
| C08 / CD-002 NativeYoloComponentSplit | YOLO FullModel/component cover 与 ingress/egress | 已验证 split 参数 | identity/enumerate；保留 source candidate priority，不能被缓存排序覆盖 |
| C09 / CD-002/013 NativeModelAdapter | graph/task/state/runner 差异契约；不接管协作状态 | immutable descriptor/task/state contract；native runner factory | inspect/encodeInput/decodeResult；模型输入输出语义与通用调用分离 |
| C10 / CD-002 NativeAdapterRegistry | 按 adapter ID 查找已注册原生实现 | entries、frozen；bootstrap 单写后只读 | registerAdapter/find/freeze；重名/未注册/冻结后改写明确拒绝 |
| C11 / CD-003 NativePlanSealer | 唯一 DI 规范计划构造/校验，Provider 独立复验 | 无跨请求状态 | sealCore/grantView/finalizeSecurity/project/encode；注明 canonical bytes 和网络提交前校验 |
| C12 / CD-004 NativeGrantClient | 原生 requester 签名申请、发布及绑定 | authority port、Core publication port、credential handles；请求 lease | acquire；网络动作在 Core executor，secret 不入 handle/日志 |
| C13 / CD-004 NativeArtifactPolicyAuthority | 模型工件 grant policy issuer，独立于 requester placement | issuer credential、artifact policy；不拥有 Core ControllerVersion | issue；先认证/授权再封装，不重新实现密码算法 |
| C14 / CD-005 assembler module | 按已认证recipe原生格式操作，Provider拥有激活/缓存；不可取消第三方调用放具名native worker | options/源/结果guard、steady deadline、owned worker PID/FD/offset；详见native-onnx-assembly-design | 两个prepare overload及assembleNativeCertifiedOnnxModel；OA01--OA09与S1--S8细化；post-Selection不变 |
| C15 / CD-006 NativeTokenizer | digest-bound 原生 encode/decode 适配器 | tokenizer artifact path/digest、unique_ptr<Impl>；Impl的handle/digest/mutex见native-dependency-design | constructor/encode/decode/destructor；O-003已固定C ABI与串行所有权；不逐 token 重建 |
| C16 / CD-007 NativeConversationCoordinator | requester turn/checkpoint 原子晋升；复用 Provider state | journalRoot、committedRecords、inflightTurns、writer lease | beginTurn/abortTurn/prepareCheckpoint/commitTurn/restore；不得覆盖已提交 predecessor |
| C17 / CD-008 bindings module | 纯转换/GIL/事件投递，不复制业务算法 | native shared handles 与 Python callback lifetime | bindDistributedInference；异常和取消语义来自原生，无策略/runner trampoline |
| C18 / CD-013 NativeRequestPreparation | 原生 task/graph/catalog/input/artifact I/O owner | registry、authenticated catalog/Repo/publication ports、request control | prepareInput/inspectModel/ensureArtifacts；已发布 immutable record 只按 TTL 过期，不假称可撤回 |
| C19 / CD-013 NativeOfferAdmission | 在 Core 包认证之后验证 DI candidate/offer 绑定 | immutable offer policy；没有独立 Trust Schema | verify；不接受 caller trusted=true，校验完成前 offer 不得影响规划 |
| C20 / CD-014 NativeInferenceProvider | 提取 executable 的共享注册/runtime 接线 | shared provider/adapters、registrations、stopped | constructor/serve/stop/destructor；不停止共享 Core，不合并管理权限 |
| C21 / CD-014 NativeServiceRegistration | 本服务注册 RAII 句柄，保护在途 callback 寿命 | shared registration record、closed | close/destructor；重复关闭幂等，关闭后拒绝新 admission |

C01/C02/C20/C21 析构不可抛异常或在 Core I/O 线程 join；最终清理由自有
executor 完成，callback 捕获 weak/shared operation 而不是已销毁 this。
client/provider 不可复制；handle/registration 通过 shared internal record 分享同一权威。
内部表示允许 pImpl，但下列状态不允许藏成未定义 context。
数据值类型的全部 observed source 字段和 planned 映射见 value-contracts；
不存在“只有名字的 DTO 就算设计完成”的例外。

## Method Contracts

前后签名以 CD 的代码块为基础；本表补每个方法的 purpose、算法/效应/失败和消费者。
签名调整必须同步这里和 CD，不由实现者自由补重载。

| ID / symbol | How / preconditions / outputs / failure | Caller / mutation / comment |
| --- | --- | --- |
| M01 NativeInferenceClient constructor | 保存经过校验的6个 native dependency；adapters 已冻结；conversations 可空；缺必需项参数错误 | C++ bootstrap / binding；创建自有 executor，Core 启动仍属应用；不发 Request |
| M02 request(model,input,split,placement,options) → NativeInferenceHandle | 验证 model/task/options → 原生 prepareInput → Core BeginCollaboration → ACK_CLOSED → inspect/admission → split/placement → ensure → seal/grant/commit → native decodeResult | CLI/binding；requestId 取唯一 owner；异步业务失败进入一次终态；不持 GIL/Face 阻塞 |
| M03 close() → void | fence closed → 禁止新 request → cancel 自有 operation → 注销观察；共享 Core 不关闭 | 应用 shutdown/析构；幂等；清理完成与本地终态分开 |
| M04 ~NativeInferenceClient() | 调非抛异常 close 路径，释放自有引用；不 join 当前 executor | RAII；仍在途 callback 不能访问失效 this |
| M05 requestId() const → string | 返回 operation 已分配 ID 的稳定 URI；ID 创建时序 O-004 明确后实现，不返回伪 ID | caller 日志关联；只读非秘密 |
| M06 status() const → NativeRequestStatus | 从 operation 获取一致快照，绝不查询 Python 第二状态机 | UI/poll；无网络或业务状态变化 |
| M07 result(milliseconds waitTimeout) const → NativeInferenceResult | 等待该 operation；有结果返回 native payload/元数据；业务失败抛 NativeDiError；本次等待超时单独报告 | 应用/binding 释放 GIL；禁止 Core I/O 线程阻塞调用 |
| M08 cancel() → void | serial executor fence 未来计划/attempt；stream 调现有 cancelStreamRequest；远端按既有 control/deadline 收束 | caller/close；幂等，不能承诺即刻 remote abort |
| M09 observe(function<void(const NativeInferenceEvent&)>) → void | 注册只读事件消费者到独立有界队列；异常隔离；溢出为 delivery error 不伪造业务结果 | telemetry/binding；捕获 lifetime、停止投递后释放 callback |
| M10 identity() const → NativeStrategyIdentity | 根据固定 strategy name/version/参数 canonical digest 返回值 | planner/sealer；不从 mutable cache 构造身份 |
| M11 enumerate(model,graph,budget) const → vector<NativeSplitCandidate> | 认证模型/graph匹配；按模型 cover规则生成有界候选；零候选/越预算明确失败 | client ACK后调用；无 I/O；非法输出由 sealer 拒绝 |
| M12 propose(snapshot,candidate) const → NativePlacementProposal | 先过滤角色/backend/device/资源，再按冻结时刻有效 lease/residency 和 tie-break 排序；不得更改 candidate priority | client 每候选调用；无可行方案为 planning failure |
| M13 registerAdapter(string id,shared_ptr<const NativeModelAdapter>) → void | 启动期校验 descriptor ID与注册键一致；空/重复/冻结后变更拒绝 | native bootstrap；写 entries；非 request 动态插件 |
| M14 find(const string& id) const → shared_ptr<const NativeModelAdapter> | 只读查找，未注册显式错误；返回共享只读 lifetime | preparation/client；不能按模型名 switch 隐藏分支 |
| M15 freeze() → void | 校验 entries 后设置只读；幂等；所有 client 构造前完成 | bootstrap；后续查找无锁写竞争 |
| M16 inspect(const NativeModelDescriptor&) const → NativeGraphSnapshot | 读取已认证 source并验证 model/adapter/graph identity，不能使用 hint替代签名 | preparation.inspectModel，ACK后工作线程；I/O端口由 preparation提供 |
| M17 encodeInput(const NativeApplicationInput&,const NativeRequestOptions&) const → NativePreparedInput | 校验 schema/transport；按 task进行一次 native编码；Qwen分词调用C15；已有 bytes不重复编码 | prepareInput；普通 metadata 不能覆盖认证字段 |
| M18 decodeResult(const vector<uint8_t>&) const → NativeInferenceResult | 校验 result schema和 native model输出，按原任务语义映射；不从 marker构造成功 | client Core response后；Python只呈现返回值 |
| M19 sealCore(snapshot,proposal) → NativePlacementPlanCore | 独立验证 cover/owner/DAG/ingress/device/ACK binding；构造 canonical core和digest | client；无授权副作用；失败不提交Selection |
| M20 grantView(core,provider,policy) → NativeProviderGrantView | 只派生该Provider保护角色的准确视图；provider必须属于core | grant申请；不接受第二份 request/attempt/digest |
| M21 finalizeSecurity(core,grants,policy) → NativeSealedPlan | grants覆盖所有保护角色且无冲突；保留当前Core授权前置；封印后只读 | client；缺/过期/错recipient失败，不能降plaintext |
| M22 project(plan,provider) → NativeSelectionProjectionV3 | 从同一sealed plan派生角色/依赖/授权投影 | Core commit adapter；字段来源单一，Provider仍独立验证 |
| M23 encode(projection) → vector<uint8_t> | 使用冻结canonical规则序列化，按已有边界校验长度/digest | publication/commit；bytes oracle由独立旧向量提供 |
| M24 acquire(view,deadline) → NativeGrantBinding | 签名申请→authority issue→Core signed publication→确认实际名字/digest；在工作executor等待有界响应 | requester；终止时释放secretlease，已发布Data仅TTL失效 |
| M25 issue(request,now) → NativeKeyGrant | 核对调用身份、工件policy、recipient、有效期，再用原密码原语封装/签名 | grantClient；失败不输出可用key，不持Core状态权威 |
| M26 prepareNativeCanonicalOnnxRole(fetchers,projection,options) → NativeModelRunnerSpec | 保留fetch/digest/recipe/resource/cache逻辑，runPythonHelper替换为native assembler；授权先于明文/加载 | Provider preparation；失败清理临时 buffers/lease |
| M27 prepareNativeCanonicalOnnxRole(ctx,projection,options) → NativeModelRunnerSpec | 从真实 CollaborationContext绑定fetchers，再委托M26 | native Provider handler；不再复制格式算法 |
| M28 assembleNativeCertifiedOnnxModel(source,recipe,control) → NativeCertifiedAssembly | 父进程有界调用具名native worker，worker执行认证identity/node/external/checker/ORT与确定性序列化；超时/cancel回收；不激活缓存 | M26；source/recipe alias/control及私有OA02--OA09按native-onnx-assembly-design，O-002剩余证据未关闭 |
| M29 NativeTokenizer(path,expectedDigest) | 验证实际artifact digest并创建一次 native backend；异常释放部分资源 | adapter注册/decoder factory；不启动解释器 |
| M30 encode(const string& text,bool addSpecialTokens=true) const → vector<int64_t> | 保持normalization/BPE/byte fallback；addSpecialTokens控制tokenizer配置的特殊token处理，与旧add_special_tokens一致；输入UTF-8行为按冻结向量 | Qwen task；返回拥有数据的token数组；true/false均需对照 |
| M31 decode(const vector<int64_t>& ids,bool skipSpecialTokens=true) const → string | ID范围与UTF-8规则按同一tokenizer；skipSpecialTokens控制是否跳过特殊token，与旧skip_special_tokens一致；返回完整文本 | terminal/native client；不能仅返回token IDs冒充成功；true/false均需对照 |
| M32 ~NativeTokenizer() | 用锁定ABI的释放函数释放handle；不抛异常 | RAII；并发调用结束后释放，O-003定义线程策略 |
| M33 makeNativeStandaloneTokenizerDecoder(options,expectedDigest) → function<string(const vector<int64_t>&)> | 捕获shared native tokenizer；重复调用重用backend | generationTextDecoderFactory；options删除pythonExecutable/pythonModule |
| M34 beginTurn(continuation,input) → NativeConversationTurn | 校验父记录/权限/attempt lineage，创建inflight turn，不覆盖committed record | native request；无conversations配置时拒绝continuation |
| M35 abortTurn(turn,error) → void | fence turn，取消后续工作，释放临时checkpoint；保留上次committed | failure/cancel；幂等，不能假称远端已回滚 |
| M36 prepareCheckpoint(turn,completedAttempt) → NativeConversationCheckpoint | 验证完整已接受结果/Provider状态引用/parent，生成暂存checkpoint | terminal commit path；旧attempt/不完整状态拒绝 |
| M37 commitTurn(turn,checkpoint) → NativeConversationRecord | 核验前态未变、单写者lease→atomic durable commit→晋升记录 | completed turn；故障不得部分覆盖旧记录 |
| M38 restore(journalRoot) → void | 按版本读取并校验已提交record，拒绝未知/部分写入和错误parent | bootstrap；不逐对象dump内存，不改Core runtime status store |
| M39 bindDistributedInference(pybind11::module_&) → void | 注册native类/枚举/错误、方法docstrings；GIL仅类型转换/observer时持有 | _ndnsf module init；链接同一库，不编译第二份DI核心 |
| M40 prepareInput(model,value,options) → NativePreparedInput | 解析认证model/task并调用M17；产生deadline/transport-owned值 | client Request前；错误时无网络Request |
| M41 inspectModel(input) → NativeInspectedModel | ACK后原生M16产生认证graph，对应immutable source | planning准备；graph identity不来自Provider hint |
| M42 ensureArtifacts(model,proposal,control) → NativeArtifactBinding | candidate选定后描述/发布canonical工件，核对实际引用；先于seal/grant | client；只准备canonical source，角色装配仍在Provider |
| M43 verify(ack,policy,context) → NativeProviderPlanningView | Core provenance有效→policy signer/candidate/service→offer签名→request/model/graph/expiry→typed view | ACK admission；拒绝 caller trusted=true |
| M44 NativeInferenceProvider(provider,adapters) | 保存现有Core/provider和native registry；不触发另一Face | bootstrap/binding；配置未闭合拒绝 |
| M45 serve(service,config) → NativeServiceRegistration | 验证native runner/角色/能力，调用既有注册/handler/runtime工厂 | CLI/binding；重复service拒绝，不接受Python callable |
| M46 stop() → void | 停止本host新admission，关闭自有registration，保留sharedCore；在途按guard/deadline清理 | 应用shutdown；不得停另一服务 |
| M47 NativeServiceRegistration::close() / destructor | 幂等关闭自身Core scoped registration，立即fence新工作，Face序列清理所属entry；旧callback持有安全owner | RAII/host.stop；新增Core API、代次和存活锁见[provider lifecycle](native-provider-lifecycle-design.md)，当前planned而非既有unregister |
| M48 NativeInferenceProvider destructor | 调幂等stop，释放本host对象，不抛异常/同步等待Core事件 | RAII；关闭证据来自真实registration/runtime |

M10/M11 覆盖C04/C07/C08；M10/M12覆盖C05/C06；virtual destructor默认释放
immutable参数，不具有协议副作用。NativeModelAdapter 的析构必须virtual且不抛异常。
M16 的I/O权限来自C18注入的native ports；策略M11/M12仍纯函数。

## State and Parameter Dictionary

本表定义新owner状态与API参数。值对象字段见value-contracts；现有Core字段保持原owner。
Name相同但作用域不同的字段不能共享可写状态。

| Field / owners | Type / source / initial | Meaning / readers and writers | Lifetime / validation / annotation |
| --- | --- | --- | --- |
| user / C01 | shared_ptr<ServiceUser>，应用构造传入，必需 | Core网络/认证访问；C01只调用公开API | shared非独占；不关闭Face；注释 Core remains authoritative |
| adapters / C01,C18,C20 | shared_ptr<const NativeAdapterRegistry>，已freeze | ID→native模型实现；请求只读 | 必需；freeze前不可交client；不可混Python对象 |
| grants / C01 | shared_ptr<NativeGrantClient>，已配置凭证/issuer | 保护角色申请grant | 生命周期覆盖在途请求；不含caller明文keydict |
| conversations / C01 | shared_ptr<NativeConversationCoordinator>，可空 | continuation owner | 空仅限无状态请求；不能默认丢弃continuation |
| preparation / C01 | shared_ptr<NativeRequestPreparation>，必需 | 输入/图/工件I/O owner | 构造校验；失败仍由operation清理 |
| admission / C01 | shared_ptr<const NativeOfferAdmission>，必需 | offer policy校验 | 不变；Core信任不能由此伪造 |
| operations / C01 | map<requestId,shared operation>，空 | 每请求唯一状态；executor写，handle快照读 | terminal后保留到最后handle释放；不记录secret |
| executor / C01 | 自有serial work executor，构造创建 | DI状态串行提交；Face操作post至Core | 不能在Face阻塞；close不join自身线程 |
| closed / C01 | bool=false，executor唯一写 | 阻止新请求和晚到业务晋升 | close置true不可复活；禁止另一个Python标志控制它 |
| phase / operation | enum NEW/PREPARING_INPUT/REQUESTING/PLANNING/COMMITTED/TERMINAL | 状态转换由DI executor，Core网络状态独立 | terminal只一次；非法事件拒绝/诊断 |
| requestId / operation | ndn::Name，Core分配后固定 | 跨ACK/plan/grant/结果绑定 | API不允许与Core分配矛盾；URI仅显示形式 |
| attempt / operation | uint64，首次1，受支持恢复才递增 | fence旧结果/状态，不是Core ControllerVersion | cancel后不递增；不得接受caller覆盖 |
| deadline / operation,control | steady time_point，本次总预算计算 | 本地工作/等待上界 | 时钟回拨不延长；保留独立wall expiry wire字段 |
| deadlineMs / signed values | uint64 epoch-ms，来源已认证契约 | wire/grant/lease有效期 | 不与monotonic时钟直接比较；转换策略O-004锁定 |
| result / operation | optional<NativeInferenceResult>，初始无 | 一个native业务结果，由terminal owner写 | 非秘密payload类型按task验证；失败不设成功 |
| error / operation | optional<NativeDiError>，初始无 | 稳定业务错误；与local wait timeout区别 | 首终态写一次；不存key/rawcipher secrets |
| events / operation | bounded native event queue，空 | token/进度的观察投递，不是业务权威 | 溢出显式delivery error；可靠stream缺口独立失败 |
| waitTimeout / M07 | chrono::milliseconds，caller每次提供 | 本次同步等待预算，不是request总deadline | 非负；0为不等待检查；Core线程拒绝阻塞 |
| observer / M09 | function<void(const NativeInferenceEvent&)> | 观察回调；不参与策略/判成功 | 持有到unregister/close；异常隔离，Python进入时持GIL |
| splitStrategy,placementStrategy / M02 | shared_ptr<const strategy>，caller选择native实例 | 指定方法，实现C++执行 | 非空；request期间shared；identity不可变 |
| timeoutMs,ackTimeoutMs / NativeRequestOptions | int，已验证config/显式参数 | 总调用预算与ACK收集预算 | timeout>ack>0；无未经验证数字默认 |
| task / NativeRequestOptions | InferenceTaskDescriptor的native值 | 输入/选项/结果schema引用 | 原字段逐项映射；不允许任务名猜schema |
| generation / NativeRequestOptions | optional<NativeGenerationOptions> | token限额/采样/tokenizer/stream规则 | 无则stateless；字段来自generation contract，不含Python callable |
| continuation / NativeRequestOptions | optional<NativeConversationContinuation> | 已认证旧会话引用 | 有值必须配置C16；parent/attempt/freshness校验 |
| entries,frozen / C10 | map<string,shared_ptr<const adapter>>；bool=false | 启动期注册，freeze后纯读 | 重名拒绝；freeze后无写，避免请求间策略漂移 |
| journalRoot / C16,M38 | filesystem::path，显式配置 | 本地会话持久化目录 | 不等于Core RuntimeStatusStore路径；权限与单写lease校验 |
| committedRecords,inflightTurns / C16 | map keyed by conversation/turn identity，restore后/空 | 已提交权威与未提交暂存分开 | 只由C16写；原子晋升，不把temporary当可恢复记录 |
| tokenizerPath,expectedDigest / C15,M33 | path,string，已声明artifact来源 | 使用哪一个tokenizer及认证字节身份 | 实际文件digest匹配；注释不可每token替换 |
| backendHandle / C15 | Impl中unique_ptr<void,ndi_token_destroy>；精确C ABI见native-dependency-design | 复用tokenizer内存实例 | 全调用持mutex、Result RAII先释放buffer、shared owner活至调用结束；禁止裸指针混用allocator |
| provider,registrations,stopped / C20 | shared Core；map服务记录；bool=false | 仅本host注册与停止状态 | sharedCore不被stop；在途record由callback持有 |
| registrationRecord,closed / C21 | shared registration记录；bool=false | 注册注销和callback lifetime | 幂等close；真实Core注销能力O-004未证实则BLOCK |
| code/domain/boundary/requestId/attempt / C03 | 既有错误码、边界标识、请求身份 | 为caller区分参数/规划/授权/执行错误 | native→Python固定映射；不以异常文本为协议 |
| cacheDir,providerIdentity / assembler options | existing string配置 | 内容缓存位置、实际Provider身份 | 保留既有路径/权限检查；身份不由策略覆盖 |
| pythonExecutable/pythonModule/helperTimeoutMs / assembler options | existing字段，DELETE或替换 | 退出解释器桥接，超时转request control | 删除所有CLI/config/callers和旧注释；不留下静默fallback |
| shouldCancel/signManifest/protectedRuntime/roleAssemblySpecDigest / assembler options | existing native callbacks/shared owner/digest | 保留取消、签名、授权、精确role recipe绑定 | 不提供Python回调；按真实来源独立校验；secretlease在protectedRuntime |
| getArtifact/fetchEncryptedLargeData / NativeCanonicalOnnxFetchers | existing typed native callbacks | 认证图/source读取端口 | M27从ctx绑定后委托M26；测试只替外部端口，不替装配算法 |

## Declaration Comment Examples

以下为 planned 英文注释内容示例，放在未来公开头文件的相应声明旁；最终签名或行为变化时同步修改。
字段注释说明默认值和边界，不重复字段名。

~~~cpp
/**
 * Submit a model request through the shared native DI lifecycle.
 * @param model Authenticated model reference; never a name-only trust claim.
 * @param input Application value encoded once by the native task adapter.
 * @param splitStrategy Immutable native split policy retained until termination.
 * @param placementStrategy Immutable native placement policy with no I/O side effects.
 * @param options Task and time budgets; request identity is assigned by the owner.
 * @return An operation handle; submission is not proof of remote execution.
 * @throws NativeDiError Invalid arguments or missing configuration before submission.
 * Asynchronous failures are reported by the handle. Do not block the Core I/O thread.
 * Cancellation fences local work; remote cleanup follows its own control/deadline.
 */
// NativeInferenceHandle request(...); // Full signature: CD-001.

/// Maximum policy evaluation time in milliseconds (existing default: 100).
/// Measured with a monotonic clock; distinct from the overall request deadline.
/// Validated once when creating the immutable native budget.
// NativeCandidateBudget::maxPolicyMs; // Integer width and cap: O-004.
~~~

Python绑定docstring必须写明：调用进入同一native request；参数必须为绑定的原生策略对象，
不接受Python策略override；等待超时不等于取消；异常code与native一致。
setter如会改变已提交请求则不得暴露；只读结果/状态不能通过Python对象写回owner。

策略接口的virtual destructor为默认、非抛异常、多态安全释放；派生identity/enumerate/propose
分别继承M10--M12契约，在C06--C08补各自模型规则及参数字段，不能省略override审计。
普通值类型按值复制/移动；持有worker/credential/registration的owner不得默认复制，
必须在T001明确copy/move可用性和析构清理次序。未声明的构造/释放函数同样进入覆盖清单。

## Application Examples

DESIGN_EXAMPLE / NOT_COMPILED：以下为库实现后的调用片段，依赖由真实应用
bootstrap创建；不伪造构造尚未冻结的图/安全DTO。配置例必须在T001字段闭合后
补成可编译consumer，并由T002/T010/T012验证。

~~~cpp
// user: a running Core ServiceUser with the merged security configuration.
// adapters: frozen native registry; grants/preparation/admission: configured native ports.
// conversations: native journal owner, or nullptr for stateless requests.
// model/input/options: validated values defined by the task adapter and value contracts.
// split/placement: shared immutable C++ strategy instances.
NativeInferenceClient client(user, adapters, grants, conversations, preparation, admission);
auto handle = client.request(model, input, split, placement, options);
handle.observe([](const NativeInferenceEvent& event) { /* present non-secret progress */ });
auto result = handle.result(std::chrono::milliseconds(1000));
// A local wait timeout permits another result() call; it does not cancel the request.
handle.cancel(); // Fences this operation; remote cleanup has its own deadline evidence.
client.close(); // Does not stop a shared Core Face.
~~~

旧 Python 入口由 .from_config / request_model / request_task 等兼容面转发上述
client；caller不再构造Python planner、grant callback或每token coordinator。
原 InferenceProvider.serve(service, callableRunner, capabilities=...) 的callable
形式必须迁移为native runner/config（类型不匹配显式错误）；语义相同的原生执行
必须先存在，不能仅拒绝旧接口便宣称兼容完成。

~~~cpp
// provider/adapters and service/config come from the native bootstrap contract.
NativeInferenceProvider host(provider, adapters);
auto registration = host.serve(service, config);
registration.close(); // Closes this registration, not another shared service.
host.stop();
~~~

成功例和取消例只规定调用顺序；错误code/观察订阅生命周期、backend ABI、
完整nested field类型仍按O-002--004闭合。不得宣称代码片段已编译。

## Merged Core Reuse

REUSE原生API来自此次合并源码，不新增同名DI安全状态：
ServiceUser::BeginCollaboration（basic和extended overload）、
CommitCollaborationPlan、cancelStreamRequest、publishSignedAppData、
publishCollaborationData/waitForVerifiedCollaborationData/clearVerifiedCollaborationData。
输入参数精确声明见 baseline 绑定的 ServiceUser.hpp；DI传service/request payload、
ACK/总预算、真实callbacks/capabilities/stream options，不要求caller选Core加密内部字段。
返回requestId/commit bool/verified records分别处理，false不能作为已执行成功。

RequestSecurityBinding、RequestKeyBundle、SelectionKeyEnvelope、AeadEnvelope、
ControllerVersion、RevocationState、RuntimeStatusStore属于Core已有机制。
新DI代码不得缓存一份可覆盖它们的controller version或authorization bool；
policy snapshot仅提供计算输入，受保护转换仍由Core/Provider重新检查当前权限。
模型artifact grant和Core请求/响应key是不同安全对象，不互相替代。
合并后Core撤销存在，不意味着DI artifact grant撤销/独立网络issuer已经全部完成；
各自范围和证据分别保留。

## Readiness Boundary

本文使类职责/方法行为/状态/注释和用法可审查；它不是“所有叶子API已冻结”的声明。
value-contracts列出的nested类型、O-002/003原生依赖、O-004的注销/错误/持久化字段，
以及O-005隔离方案尚未闭合。历史合并integration失败已修复，当前SVS/NDNSD组合仍未获完整运行资格；不得把两个状态混淆。Core没有公开逐服务注销接口，M47须按[runtime boundary](runtime-boundaries.md#current-registration-boundary)补齐设计。T001必须逐条关闭设计缺口，不能跳过进入T002。

## Static Review Use

源码对照设计的审查、任务内unit和全部实现后的integration/MiniNDN统一见 [validation workflow](pre-test-static-review.md)。本附件只定义技术契约，静态或文档检查不代替运行证明。
