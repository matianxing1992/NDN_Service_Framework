# Tasks: Native NDNSF-DI with Optional Python Bindings

**Revision**: 11 | **Status**: DRAFT / T001 DONE
**Input**: [spec](spec.md), [code design](contracts/code-design.md),
[proof](contracts/proof-design.md), [work units](contracts/work-units.md)

## Execution Progress

本表是所有执行者共同维护的**当前子任务进度唯一入口**；点击任务查看 Read、Write、Steps、Verify。
父任务清单保留阶段验收；Current Checkpoint 保存摘要与历史，不作为第二张状态表。
状态：NOT_STARTED（无独立执行记录）、READY（依赖及门禁满足）、IN_PROGRESS（正在执行）、
PARTIAL（已有工作但验收不全）、BLOCKED（已确认阻塞）、DONE（该卡完整验收通过）。
PARTIAL 不表示依赖放行；T001 release 及 plan Gate Order 继续约束执行。
本次按持久 checkpoint 保守登记，未逐卡重跑验收，不以文件存在或结构检查计算完成百分比。
每个工作单元成功/失败/阻塞后、commit 和回复前更新对应行及证据；新增工作先补卡和进度行。
维护规则见 [task progress](../../skills/speckit-code-design/references/task-progress.md)。

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [T001-A Identity and Dependency Closure](contracts/execution-units.md#t001-a-identity-and-dependency-closure) | DONE | — | [closure](evidence/t001-ab-closure-20260907.md)；DOC + 依赖契约 vs 持久探针核对通过；onnx 4/4 与 tokenizer 84+14 全新复现 PASS；rust 1.90.0 独立工具链核验；Cargo 边界已在 rust-prefix 上重跑通过（tokenizer-r2） | 2026-09-07 |
| [T001-B Lifecycle and Capability Closure](contracts/execution-units.md#t001-b-lifecycle-and-capability-closure) | DONE | — | [closure](evidence/t001-ab-closure-20260907.md)；DOC + 双向映射核对通过；O-004 处置写入 runtime-boundaries（Rev 8）与 symbol-design（C21/Readiness）；registration generation/late ACK/Selection/共享 lease 已冻结于 lifecycle 设计；parity 按 owner 任务继续，不属本卡 | 2026-09-07 |
| [T001-C Dispatch and Selector Freeze](contracts/execution-units.md#t001-c-dispatch-and-selector-freeze) | DONE | T001-A, T001-B | [closure](evidence/t001-c-freeze-20260907.md)；build identity/L0 命令/每卡 selector 已从实际 Waf 注册冻结到 [case-manifest](../../tests/fixtures/spec182/case-manifest.json)（23 cppSuites + 6 kexpr + 3 system，全部带 author/executeOwner）；proof/code-design/work-units Rev 8、O-002/O-004 关闭；DOC 通过 | 2026-09-07 |
| [T002-A Installed Library Boundary](contracts/execution-units.md#t002-a-installed-library-boundary) | DONE | T001-C | [L0 evidence](evidence/t002-a-l0-20260907.md)；首次 L0 成功（r2 fresh staging）：consumer 独立编译 rc=0、运行打印 `SPEC182_INSTALLED_CONSUMER_NATIVE_DI_OK` rc=0、无 libpython、staging 无 DI `.cpp/.cc` 副本、NAC-ABE 5-symbol gate 通过；空 registry freeze 语义自修正（[failure-log](../../docs/failure-log.md) 2026-09-07） | 2026-09-07 |
| [T003-A Qwen Split Candidates](contracts/execution-units.md#t003-a-qwen-split-candidates) | DONE | T002-A | [acceptance](evidence/t003-a-qwen-split-20260907.md)；CPP(Spec182QwenSplit/*) 5 cases 全绿（合法 cover、边界 budget、非法 rank/图输入），expected 逐条对照冻结 `QwenThreeStageSplitter`，支持范围保持 rank-one；新增 2 case 于 di-native-planning.t.cpp | 2026-09-07 |
| [T003-B Yolo Split Candidates](contracts/execution-units.md#t003-b-yolo-split-candidates) | DONE | T003-A | [acceptance](evidence/t003-b-yolo-split-20260907.md)；CPP(Spec182YoloSplit/*) 7 cases 全绿（固定 cover、错误 component/图/外来模型拒绝、确定性 + budget 截断），expected 对照冻结 `Yolo26Splitter`/`RegisteredYoloCandidate`；新增 2 case 于 di-native-planning.t.cpp | 2026-09-07 |
| [T003-C Placement and Registry](contracts/execution-units.md#t003-c-placement-and-registry) | DONE | T003-A, T003-B | [acceptance](evidence/t003-c-placement-20260907.md)；CPP(Spec182Placement/*) 9 cases 全绿（residency→freeBytes→provider 稳定序、budget/ref tie-break、同输入同结果、非法向量/全拒），对照冻结 `propose_v3` 共享语义区间；新增 2 case 于 di-native-planning.t.cpp | 2026-09-07 |
| [T004-A Canonical Plan Sealing](contracts/execution-units.md#t004-a-canonical-plan-sealing) | DONE | T003-C | [acceptance](evidence/t004-a-plan-sealer-20260907.md)；CPP(Spec182PlanSealer/*) 7 cases 全绿（canonical 封印 + M22 单源投影、encode 固定 7-key 片段字节一致 + 独立 JSON oracle、逐维度篡改敏感、错误 endpoint/缺 grant/错 ACK digest 首边界拒绝、plaintext 无 grant 封面）；planned suite 登记于新文件 di-native-plan-sealer.t.cpp；真实 Core commit/Provider parser 对照留 T016 | 2026-09-07 |
| [T005-A InProcess Authority](contracts/execution-units.md#t005-a-inprocess-authority) | DONE | T004-A | [acceptance](evidence/t005-a-inprocess-authority-20260907.md)；CPP(Spec182GrantAuthority/*) 6 cases 全绿（固定 request/时钟向量、expiry/自授/issuer 不完整/wrong-key-recipient 面原因码族拒绝、secret 生命周期无 key 驻留、注入 policy 传播）；issue() 补 requester==provider 拒绝（Python frozen 对照）；planned 独立文件拆分由既有同文件切片取代 | 2026-09-07 |
| [T005-B Requester Grant Publication](contracts/execution-units.md#t005-b-requester-grant-publication) | DONE | T005-A | [acceptance](evidence/t005-b-requester-grant-20260907.md)；CPP(Spec182GrantClient/*) 8 cases 全绿（构造门、view 完整性先于端口副作用、过期 deadline 在副作用前 fence、过期 grant 于 authority 边界拒绝、canonical exact-name/回落与 determinism、错名 publication 恰好一次消费且不复活）+ 集成 Spec182GrantClientFlow 1 case（真实 ServiceUser::publishSignedAppData + exact-name fetch，KeyLocator/content 校验）；生产代码本卡无改动（既有切片语义经测试确认）；真实 Provider crypto 消费 T016 | 2026-09-07 |
| [T006-A Canonical Source Identity](contracts/execution-units.md#t006-a-canonical-source-identity) | DONE | T002-A | [acceptance](evidence/t006-a-canonical-source-identity-20260907.md)；CPP(Spec182OnnxIdentity/*) 11 cases 全绿（24 v1 + 14 accepted extended 全模型 golden 逐字段、typed/raw pair 摘要恒等、v2 per-tensor 12 accepted 逐字节 + 5 拒绝、bf16 两编码归一、revision 分类、v2 descriptor binding 门、external/function-attr 内联等价、overflow/非法路径/限额拒绝） | 2026-09-07 |
| [T006-B Certified Extraction and Wire](contracts/execution-units.md#t006-b-certified-extraction-and-wire) | DONE | T006-A | [acceptance](evidence/t006-b-certified-extraction-wire-20260907.md)；CPP(Spec182OnnxExtraction/*) 11 cases 全绿（4 accept 逐字节 parity + 独立 sha256 交叉检查，7 reject 精确 reason family）+ Spec182NativeAssembly 3 cases + Spec182OnnxIdentity 11 cases 回归；官方 ONNX 1.17 full-pb 统一（--onnx-prefix）；data_location proto3-optional presence 奇点修正 byteParity（frozen sha 77300e13，diff 唯一 delta）；完整回归 5 个环境性失败（TPM/NFD）与本卡无关 | 2026-09-07 |
| [T006-C Bounded Native Worker](contracts/execution-units.md#t006-c-bounded-native-worker) | DONE | T006-B | [acceptance](evidence/t006-c-bounded-native-worker-20260907.md)；CPP(Spec182OnnxWorkerProtocol/*) 27 cases 全绿（frame 截断/溢出/重复帧逐字节状态机、compose/finalize 语义、metadata envelope 规范往返与 poison、真子进程 cancel 1304ms TERM→KILL escalation、信号死亡/静默/垃圾 stdout、PREFLIGHT 族）+ 842 cases 完整回归（除环境性 StreamFacade）；修复 isSha256Digest 长度门 66→71 root cause（failure-log 2026-09-07）；L0 staged install 链接验证（libexec/ndnsf-di，ELF/ldd clean，pythonWrapper 消息为设计内路径） | 2026-09-07 |
| [T006-D Protected Provider Activation](contracts/execution-units.md#t006-d-protected-provider-activation) | DONE | T006-C | [acceptance](evidence/t006-d-protected-provider-activation-20260907.md)；CPP(Spec182OnnxActivation/*) 9 cases 全绿（无 workerLocation 早拒、真实 worker 固定 manifest 装配两次同 digest、root/source digest mismatch、certified graph poison、cancel/timeout/pinned-hash tamper/信号死亡均在 sign 前拒绝且 checkNothingActivated）+ 回归锁 SubprocessChainRejectionPropagatesItsOwnCode 1 case；修复 T006-C 冻结 worker child catch off-by-one（14 vs 15 字节族前缀，全部链拒绝曾错标 DI_NATIVE_ONNX_WORKER_INTERNAL；failure-log 2026-09-07）；852 cases 完整回归（除环境性 StreamFacade）；Selection-after 真实冷装配 T016 | 2026-09-07 |
| [T007-A Full Tokenizer Ownership](contracts/execution-units.md#t007-a-full-tokenizer-ownership) | DONE | T002-A | [acceptance](evidence/t007-a-full-tokenizer-ownership-20260907.md)；CPP(Spec182NativeTokenizer/*) 3 gate cases + CPP(Spec182TokenizerFull/*) 7 cases 全绿：冻结三 profile 84 向量经真实静态链接 native ABI 逐字节对照、special flags 双路、负/超界/词表外 id 与无效 UTF-8/超限文本、digest-first 早拒（artifact 缺失与 digest mismatch 均在 engine create 前）、owner 5 轮复用确定性与 8×40 并发串行化；dlopen bridge 移除、bridgeLibrary option 删除、factory 兼容签名保留；wscript TOKENIZER_BRIDGE uselib（pinned Rust staticlib，--locked --offline -j2，独立 target 目录）链入 DI shlib 与全部 C++ link consumers；859 cases 完整回归（除环境性 StreamFacade，裸跑段错误负结果已记 failure-log 2026-09-07）；case-manifest T007-A 改名+7 named cases；失败修正：wscript 结构损坏 NameError + boost 1.71 vector 打印 | 2026-09-07 |
| [T007-B Stable Text Decoder Pair](contracts/execution-units.md#t007-b-stable-text-decoder-pair) | DONE | T007-A | [acceptance](evidence/t007-b-stable-text-decoder-pair-20260907.md)；CPP(Spec182TokenizerStable/*) 9 cases 全绿：冻结 stable-vectors.json 5 fixtures 28 rows （byte-fallback-special 11/bytelevel 13/reject-fuse 0/legacy-ascii 2/legacy-unicode 2，whole-file sha a80597b3…6254）经真实静态链接 native ABI 逐 cut 对照 decodeStable （skip/final 两维），每行 final==full 且前缀均为 finalText 文本前缀；合法 U+FFFD 保留、truncated/invalid 字节按 Rust-std 归因（tight E0/ED/F0/F4 约束）、specials skip 消失/retain 关 run、12 步交错无状态双遍一致、Fuse decoder stable 全调用 fail-closed 且完整 decode 不受影响、负/超界 id ABI 前拒；lib.rs Profile 分派 + stable_prefix 实现冻结算法，crate pin 零变化；GenerationTextDecodersFactory 收口 GenerationDecodersFactory；生产注入 留 T009-C/T011-B；859 cases 完整回归（除环境性 StreamFacade）；case-manifest T007-B 改名+9 named cases；失败修正：waf 外部 STLIB 内容不触发 relink（删产品重链）+ surrogate std-vs-python 代理分歧（按 std error 归因镜像重写；failure-log 2026-09-07） | 2026-09-07 |
| [T008-A Native Input and Artifact Preparation](contracts/execution-units.md#t008-a-native-input-and-artifact-preparation) | DONE | T003-C, T006-D, T007-B | [acceptance](evidence/t008-a-native-input-and-artifact-preparation-20260907.md)；CPP(Spec182Preparation/*) 12 cases 全绿：Qwen bytes-identity 与 YOLO JSON 文档 parse+identity 两模型 mapping（乱序 key 证明不 re-encode、malformed 双侧拒）、foreign requestId/attempt/modelDigest/graphDigest 四维在端口副作用前拒（calls==0）、空/重复 roles 与 role gap/多余/同尺寸外来集 mismatch、5 个非 NDN catalog 源名形状拒、malformed digests、mislabeled adapter 身份与版本 mismatch、无端口 fail-closed、deadline/cancel/nameless/fresh 清理边界；ensureArtifacts 增加 proposal↔control/model/graph 身份绑定与 roles 非空唯一、post-port 精确 role-cover、binding 源 NDN-name 门；planner 零改动（T003-A 先例）、生产 adapter 注入留 T009-C/T016、I-file 由 T008-B 承接（PO-013 无 harness 代做）；879 cases 完整回归（除环境性 StreamFacade）；case-manifest T008-A 行登记 suite + 11 named cases | 2026-09-07 |
| [T008-B Authenticated Offer Admission](contracts/execution-units.md#t008-b-authenticated-offer-admission) | DONE | T008-A | [acceptance](evidence/t008-b-authenticated-offer-admission-20260907.md)；CPP(Spec182OfferAdmission/*) 15 cases 全绿：provenance 前置（trust flag 假仍拒、signer/locator/wire 缺空与 sha256/NDN/KEY-命名空间形状错 → UNAUTHENTICATED）、coherent 外来签名 signer!=provider 拒（python 冻结规则）、policy containment（signer/provider/service 成员）与五维 request/attempt/service/model/graph binding mismatch、policy 不可用五面（digest/roles/backends/sequence/residency）、未来或过期 ACK 与过期 policy、唯一 accept 径 view 六维构造 + 无状态确定性双验；NativeOfferAdmission.hpp/.cpp 独立拆分 + NativeAckEvidence 补全 Core AckAuthenticationEvidence 形状（trustSchemaValidated/signerKeyLocator/wireDigest/capturedAtMs 镜像 python）；policy 非 second Trust Schema/无 key map、caller trusted=true 与 Python verifier callback 以 API 缺位拒绝；preparation 收窄净 −~100 行 + 死 hashBytes/死 include 清除（警告清零）；I/di-native-preparation.t.cpp 仍无 seat（PO-013/T008-A 理由续记，真实 Core admission 留 T016）；893 cases 完整回归（除环境性 StreamFacade）；case-manifest T008-B 行 suite + 14 named cases | 2026-09-07 |
| [T009-A Core Scoped Registration](contracts/execution-units.md#t009-a-core-scoped-registration) | DONE | T006-D, T007-B | [acceptance](evidence/t009-a-core-scoped-registration-20260907.md)；CPP(Spec182Registration/*) 6 cases 全绿（新 suite di-native-provider-host.t.cpp）：ServiceRegistration move-only RAII（addScopedService 双载/addScopedCollaborationHandler，close 一次性消费 + closed atomic + Face post cleanup 闭包只 capture RegistrationControl/state，provider 先死 no-op）与 pending/collab registrationState 代次绑定（accept commit 在 finishAckDecisionOnEventLoop 写 pendingKey→state，与 pendingRequests 同生命周期，cleanupPendingRequestState 精确释放）+ dispatch 双路径同 fence（worker dispatchRequestExecutionAsync 入口与 pool-0 gateInlineRequestExecution，mismatch 拒 "generation changed"）；6 cases 覆盖晚到 ACK commit（ack worker 窗口 close → closed gate 正向降级负向，pending 不 store、handler 不执行）、旧代次 Selection after reregister（真实 worker decode + io-post commit 后 fence 同步拒，两代 executions 0）、同步面 inline 同 fence + gen2 正向对照执行成功、真实 cleanupPendingRequestState 后 collab mismatch gate 拒兄弟 role、provider 析构后 handle closed/valid/close no-throw、精确 pendingKey cleanup 不伤同 requester 独立 userToken 后继请求；编排关键修正：worker 面 accept commit 经 io post 落 Face 线程（drain handler pool 只保证 decode 完成），case 2 需 drain + pump io 后再 close（根因是编排非实现，inline 对照证明绑定写正常）；legacy 注册路径零行为变化、无 DI 类型/新 wire；899 cases 完整回归（除环境性 StreamFacade，负结果见 failure-log 2026-09-07）；case-manifest T009-A file 落位；跨服务 NFD 用例留 T016 | 2026-09-07 |
| [T009-B Shared Execution Lease State](contracts/execution-units.md#t009-b-shared-execution-lease-state) | DONE | T009-A | [acceptance](evidence/t009-b-shared-execution-lease-state-20260907.md)；CPP(Spec182SharedLease/*) 3 cases 全绿（新 suite di-native-provider-host.t.cpp）：SharedExecutionLeaseState（Core table + prepare mutex，host boot epoch，原四参数 constructor 保留并委托自有 state）+ shared_state overload + 非 Prepare 操作 find-then-route target 绑定检查（跨 target 行 LEASE_SERVICE_MISMATCH 且响应不泄漏 lease 细节，未知 lease 走 Core LEASE_NOT_FOUND，requester/epoch/state/replay 仍全由 Core 判定）；3 cases 覆盖双 target 争同物理槽 FIFO 等待队列（只允许一个 Prepare、host 不能双订、owner Abort/Release 后对称易主、retryAfterMs 100）、Executing lease 上另一 target 的 Commit/Abort/Renew/Release 全拒 + Released 行仍绑定 + 同 target 幂等 replay（状态未变窗口）与 Core 授权（REQUESTER_MISMATCH/STALE_EPOCH）、target 实例析构不释放执行中槽（shared state 保留 Executing 行、B 与重新 serve 的 A 都不能盗用、A owner 流 Release 后 B 继续 Prepare/执行）；旧 DiExecutionLeaseService 3 cases 零改动全绿（单 target 回归）；902 cases 完整回归（除环境性 StreamFacade）；关键修正：boost 1.71 逗号组合 selector 不可靠改单 selector 执行、FIFO 等待需同 requestId 重试、Core replay state 再验证使弃 idempotency 重放得 INVALID_TRANSITION（用例按真实语义修正）；跨服务真实双服务 PO-014 留 T016 | 2026-09-07 |
| [T009-C Shared Provider Host Wiring](contracts/execution-units.md#t009-c-shared-provider-host-wiring) | DONE | T009-B | [acceptance](evidence/t009-c-shared-provider-host-wiring-20260907.md)；CPP(Spec182ProviderHost/*) 6 cases 全绿（新 suite di-native-provider-host.t.cpp）：NativeInferenceProvider host 单例落地（首次 serve 发布单固定 lease 入口 addScopedService EXECUTION_LEASE_SERVICE_NAME + SharedExecutionLeaseState(host boot epoch)，makeLeaseRouter 按 targetServiceName 路由——miss=LEASE_SERVICE_MISMATCH、draining 且非 Abort/Release=LEASE_TARGET_DRAINING、内部错误 in-band LEASE_INTERNAL_ERROR、wire 恒 status=true 无异常跨 Core 回调）+ serve fence 族（boot 身份/槽位一致性 invalid_argument、同名 active duplicate logic_error 先于 config 检查、executionLeaseTargetService==serviceName 且 table 必须 null 由 host 注入共享表、draining record 替换不继承旧 lease/fence/bindings、guard handler 只盖 drain 窗口）+ close/stop 幂等（registration move-only RAII、close 抬 draining 再 core close、generation 直通、valid()=state 非空 close 后仍 true、stop 全关 + serve→runtime_error、provider owner reset 不提前关闭、host dtor 后 handle 安全）+ example 迁移（DI_NativeProviderExecutable.cpp serve 提前到 installTask、main 等待 serveCompleted cv、删旧 exec-lease 双注册块、config 不再注入 lease table、ackHandler/runtimeObserver 经 def seam、单一注册路径 CD-014）；6 cases 覆盖双 target 共享 host/duplicate 拒不波及 sibling、config 一致性三拒绝 + 顺序优先、close→同 name re-serve（generation 递增、旧 handle 保持 closed）、stop 语义全族、close 后晚到 ack 真实 Core 边界（ack 仍被询问/pending 到 cleanup boundary，fence 在 dispatch 层同 Spec182Registration selector 1/2）、固定入口真实 Core 全链 dispatch（A/B 双 target、close A 后 B 仍 Completed=PO-014、draining 晚到 Prepare 应答不牵连、re-serve 后新 target 正常）；collab handler 真实执行与真实 NFD 多入口（I/di-native-provider-host.t.cpp）留 T016；旧 Spec182Registration 6/Spec182SharedLease 3/DiExecutionLeaseService 3 零改动全绿；908 cases 完整回归（除环境性 StreamFacade）；wscript 零改动（unit-tests ant_glob ndnsf-di/*.cpp 自动收录）；case-manifest T009-C file 落位 + 6 named cases；失败修正：build-nac182 13:03 重配置丢 --with-examples 使 di-native-provider target 消失（补 configure 6.6s 恢复，非代码）、Core close fence 位置假设错误改测真实边界、dot-style provider. 遗留两处、测试常量与 T009-B 冲突 rename | 2026-09-07 |
| [T010-A Request Operation Terminal State](contracts/execution-units.md#t010-a-request-operation-terminal-state) | PARTIAL | T005-B, T008-B, T009-C | [Core I/O repair](evidence/t010-a-core-io-20260907.md)；postToIo/isOnIoThread、result I/O 等待拒绝、共享 user 寿命；-j4 构建 PASS，ClientState 11/11、既有 2/2 PASS；完整请求/成功竞争、有界通知待完成 | 2026-09-07 |
| [T010-B Complete Request Orchestration](contracts/execution-units.md#t010-b-complete-request-orchestration) | PARTIAL | T010-A | [baseline](evidence/task-progress-registry-20260907.md)；request 仍返回 NATIVE_REQUEST_PIPELINE_NOT_READY；编排未接通 | 2026-09-07 |
| [T010-C Stream Acceptance and Replacement](contracts/execution-units.md#t010-c-stream-acceptance-and-replacement) | NOT_STARTED | T010-B | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T011-A Sampling Parity Repair](contracts/execution-units.md#t011-a-sampling-parity-repair) | PARTIAL | T010-C | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T011-B Stable Epoch Emission](contracts/execution-units.md#t011-b-stable-epoch-emission) | PARTIAL | T011-A | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T011-C Conversation Journal and Continuation](contracts/execution-units.md#t011-c-conversation-journal-and-continuation) | PARTIAL | T011-B | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T012-A Native Binding Types and Lifetime](contracts/execution-units.md#t012-a-native-binding-types-and-lifetime) | PARTIAL | T011-C | [baseline](evidence/task-progress-registry-20260907.md)；已有 Python focused 17/17 记录；native 验收未完成 | 2026-09-07 |
| [T012-B Compatible Python Facades](contracts/execution-units.md#t012-b-compatible-python-facades) | NOT_STARTED | T012-A | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T013-A Maintained Caller Migration](contracts/execution-units.md#t013-a-maintained-caller-migration) | NOT_STARTED | T012-B | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T013-B Legacy Runtime Retirement](contracts/execution-units.md#t013-b-legacy-runtime-retirement) | NOT_STARTED | T013-A | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T014-A Isolation Collector Semantics](contracts/execution-units.md#t014-a-isolation-collector-semantics) | PARTIAL | T013-B | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T014-B Qualification Harness Registration](contracts/execution-units.md#t014-b-qualification-harness-registration) | PARTIAL | T014-A | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T015-A CrossTask Convergence](contracts/execution-units.md#t015-a-crosstask-convergence) | NOT_STARTED | T014-B | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T016-A Local Qualification](contracts/execution-units.md#t016-a-local-qualification) | NOT_STARTED | T015-A | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T017-A Development Handoff](contracts/execution-units.md#t017-a-development-handoff) | NOT_STARTED | T016-A | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |

## Current Checkpoint

2026-09-07 T010-A Core I/O / **PARTIAL**：已提供原 Face 调度入口，并禁止在 Core
I/O 线程阻塞 result；operation 保持 user 寿命，client close 不关闭共享 Core。
-j4 必要构建 PASS（87.60s），ClientState 11/11、既有 client 2/2 PASS；见
[Core I/O evidence](evidence/t010-a-core-io-20260907.md)。后续完整请求接线前，先纠正
T004 的七字段片段与生产 Selection parser 不兼容、伪工件摘要等前置缺口。

2026-09-07 T010-A active deadline / **PARTIAL**：独立 steady timer 保证排队或慢通知
不延后请求终态；terminal 移除定时项，测试注入收为 private，补严格 ACK/总预算和
wait 参数检查及异步异常隔离。-j4 增量构建 PASS（21.40s），ClientState 8/8、
既有 client 2/2 PASS；design validator、diff 检查 PASS。首次错误 suite filter 未运行
用例，原始失败及修正记录见 [deadline evidence](evidence/t010-a-deadline-20260907.md)。
下步补 Core I/O 调度与完整 request 接线；T010-A/B/C 和最终资格仍未完成。

2026-09-07 Actual progress audit / **T010-A PARTIAL**：21/36 子卡 DONE、父任务 5/17；
未以子卡数量关闭仍有集成编写/接线义务的父任务。requester 仍有 pipeline-not-ready 分支。
接续已有串行执行器切片，修复观察者同步阻塞 cancel/observe，通知改为独立串行队列，
解除 idle worker 自持有；提交异常转失败 handle。-j4 必要构建 PASS，ClientState 3/3、既有 client 2/2 PASS。
见 [audit evidence](evidence/t010-a-lifecycle-audit-20260907.md)。下一步完成 T010-A/B 的 Core
生产接线和成功/取消/deadline 竞争，继续 stream、会话、绑定、迁移及 T015/T016/T017。


2026-09-07 Build parallelism / **PASS**：用户授权本开发机（实查 6 逻辑 CPU、约 12 GB RAM）
后续原生构建默认 -j4，已同步 AGENTS/CLAUDE、plan 与当前执行/验证指引。
现场构建已在用 -j4；只读短样本未见持续换页，未做加速比/全程峰值验收，也未另启构建。
资源快照、使用范围和降档规则见 [build policy](../../docs/native-build-parallelism.md)。
文档 validator 与 diff 检查 PASS；新增说明已显式解除 docs 默认忽略规则，随仓库保存。
本单元为工作流文档更新，不改产品任务状态；下一步由现有构建执行者记录结果和资源，再继续当前验收。

2026-09-07 T006-B Certified Extraction and Wire / **T006-B DONE（父 T006 待
T006-C/D）**：按 T006-B 卡 Verify（CPP(Spec182OnnxExtraction/*)）在 planned
文件 `tests/unit-tests/di-native-onnx-recipe.t.cpp`（case-manifest 登记）建
suite `Spec182OnnxExtraction` 11 cases 全绿：4 accept 逐字节复现冻结 python
wire（inline/external/rank-subset/function-local-domain）+ 独立
sha256(modelBytes)==modelDigest 交叉检查；7 reject 逐字面 exact reason
family（RECIPE/GRAPH/NODE_COVER/IO_DTYPE/LAYER_RANGE，r7 ghost-input 的
python 构造期 IO_CONTRACT 与 native S5 GRAPH 差异已留档）。驱动
NativeOnnxRecipeAssembler 的 S3–S7 certified pipeline：官方 ONNX 1.17
full-protobuf 统一（--onnx-prefix，含 --with-tests/--nac-abe/--ndn-svs 完整
configure）。关键修正：(1) data_location proto3-optional presence 奇点——
external 源行与 python 冻结 wire 逐字节相同，byteParity False→True 重新冻结
（sha 6f289a01→77300e13，11 行 diff 仅该标志），suite 断言升级 byte equality；
(2) If fixture 需 set_type(GRAPH) + cond 零维 shape（full checker 要求）；
(3) 既有 Spec182NativeAssembly 3 cases（含 nested-external If）与
Spec182OnnxIdentity 11 cases 回归全绿；完整回归 5 失败为 TPM/NFD 环境性、
与 ONNX 改动不可达。evidence
[t006-b](evidence/t006-b-certified-extraction-wire-20260907.md)。
下一步：T006-C（Bounded Native Worker，依赖 T006-B 已满足）。

2026-09-07 T006-A Canonical Source Identity / **T006-A DONE（父 T006 待
T006-B/C/D）**：按 T006-A 卡 Verify（CPP(Spec182OnnxIdentity/*)）在 manifest
登记文件 `tests/unit-tests/di-native-assembly.t.cpp`（existingSuite
Spec182NativeAssembly 3 cases 原样回归）建 suite `Spec182OnnxIdentity`
11 cases 全绿：24 v1 + 14 extended accepted 全模型 golden（graphDigest/
initializerDigest/contentDigest/modelDigest/modelHex）逐字段复现、2 rejected
逐字面一致；typed/raw pair 摘要恒等（packing 恒等冻结）；v2 per-tensor
12 accepted payloadHex/byteLength 逐字节 + 5 拒绝；bf16 两编码归一；revision
分类（STRING/BF16-raw/typed-complex→2，external 先内联再分类）与 v2
descriptor binding 门（rev-2 缺 v2 descriptor 拒绝、rev-1 legacy 兼容）；
external S2 规则（同相对 location、offset 有界、无 length/length "0"
rest-from-offset、绝对/../双向 binding/双 location 拒绝）+ function
attribute 深度扫描内联等价；overflow/负 dim/限额/垃圾 parse 边界拒绝。
生产 seam：canonicalOnnxSourceIdentity（owned source + 内存 external
校验 + 原图 identity——parse 后无任何 shape inference）+ 版本化
normalization（normalizedOnnxInitializerPayload/onnxInitializerNormalization
Revision/checkOnnxAssemblerDescriptorBinding，hpp 无 onnx/protobuf 类型）。
实现期三处修正均有绿测兜底：graphFactsJson hex 缺 JSON 引号（python
reference quoted-string 对照，修后 38 全模型 graphDigest 全对）、INT4/UINT4
raw 展开（low-nibble-first、INT4 符号扩展，generator 位型为证）、两测试侧
构造（18-byte weights；initializer-limit 用例过 source 门后精确触发）。
execution-units U 路径差异按 registry 约定留档；无 wscript 改动。
evidence [t006-a](evidence/t006-a-canonical-source-identity-20260907.md)。
下一步：T006-B（Certified Extraction and Wire，依赖 T006-A 已满足）。

2026-09-07 T005-B Requester Grant Publication / **T005-B DONE、父 T005 DONE**：
按 T005-B 卡 Verify（CPP(Spec182GrantClient/*)）在 manifest 登记文件
`tests/unit-tests/di-native-grant-client.t.cpp` 建 suite `Spec182GrantClient`：
既有 2 个 module-level cases 迁入（名不变）+ 6 个新 cases 全绿 —— 构造门；
view 完整性先于任何端口副作用（issue/publish 计数 0）；已过 deadline 在
fence（原因码族、零副作用，cancel/expired-wait 不复活）；已过期 grant 在
authority expiry 边界拒绝（deadline 仍在未来 → 族前缀只能来自 issue 前检查、
policy port 未运行）；canonical exact-name 字面组件布局 + model-manifest→
model digest 回落 + 同向量 determinism（issue=2/publish=2 恰好一次各）；
错名 publication 族拒绝且恰好消费一次、重试为全新尝试、无 pending 状态可
复活（runtime-boundaries"只消费/忽略，不复活"行为面）。生产代码本卡无改动
（既有切片语义与测试一致）；issue 后二次 deadline 复检无时钟注入不可确定性
触发（T010/T016 executor 层覆盖）。集成：`Spec182GrantClientFlow`（
tests/integration-tests/di-native-requester-grant.t.cpp，wscript 注册 +
di_integration_sources 补 NativeGrantClient.cpp）1 case 全绿 —— 真实
ServiceUser::publishSignedAppData 发布 + exact-name fetch replay，KeyLocator==
requester 证书、content 逐字节一致；authority crypto 真验证/Provider 消费 T016。
execution-units U/di-native-grant.t.cpp 与 manifest 文件差异按 registry 约定
留档。回归（Spec182GrantAuthority/PlanSealer/NativePlanning）全绿。
evidence [t005-b](evidence/t005-b-requester-grant-20260907.md)。下一步：T006-A
（Canonical Source Identity，依赖 T002-A 已满足）。

2026-09-07 T005-A InProcess Authority / **T005-A DONE（父 T005 待 T005-B）**：
按 T005-A 卡 Verify（CPP(Spec182GrantAuthority/*)）在 planned 文件
`tests/unit-tests/di-native-grant.t.cpp` 登记 suite `Spec182GrantAuthority`
6 cases 全绿（固定 request/时钟向量、expiry/自授/身份不完整/issuer
不完整原因码族拒绝、注入 policy 拒绝原样传播、空 issue port 构造拒绝）；
issue() 补 requester==provider 自授权限拒绝（Python frozen issue 对照，
Steps"拒绝 caller 自授权限"）；crypto 面经注入 IssuePort，未新增网络
authority/自写密码算法。NativeArtifactPolicyAuthority 为既有切片
（NativeGrantClient.hpp/.cpp 同文件、T002-A 已安装），planned 独立文件
拆分按 registry 复用约定不重做。回归（grant-client 2 cases +
Spec182PlanSealer + Spec182NativePlanning）全绿。evidence
[t005-a](evidence/t005-a-inprocess-authority-20260907.md)。下一步：T005-B
（Requester Grant Publication，依赖已满足）。

2026-09-07 T004-A Canonical Plan Sealing / **T004-A DONE、父 T004 DONE**：
按 T004-A 卡 Verify（CPP(Spec182PlanSealer/*)）在 planned 文件
`tests/unit-tests/di-native-plan-sealer.t.cpp` 登记 suite `Spec182PlanSealer`
7 cases 全绿（canonical 封印 + M22 单源投影、encode 固定 7-key canonical
片段与布局字面量/独立 JSON oracle 一致、逐维度篡改敏感、错误 endpoint/
缺 grant/错 ACK digest 首边界拒绝、plaintext 无 grant 封面、encode 拒绝面
与 canonical 转义）；reconfigure 使新 .t.cpp 进入 unit-tests ant_glob
（configure rc=0），回归两 suite rc=0。修复
NativePlanSealer.cpp::quote() 控制字符 canonical 转义（escape case 发现的
缺陷；可达 ASCII 输入字节零变化）。核心密封字节被真实 Core/Provider
parser 消费、planDigest 密码学再校验、逐 role 投影迭代留 T010/T016。
evidence [t004-a](evidence/t004-a-plan-sealer-20260907.md)。下一步：T005-A
（InProcess Authority，依赖已满足）。

按 T001-C 卡 Verify（DOC；从实际 Waf 注册推导命令；planned 测试全带 author owner；
父 T001 完整验收满足）完成。已把 [case-manifest](../../tests/fixtures/spec182/case-manifest.json)
（schema spec182-case-manifest-v1）写入 tests/fixtures/spec182/：23 张实现卡各一个
selector（与 execution-units Verify 的 CPP suite 名逐一对照，无重复），owning 文件
（planned 文件标注 "(planned)"）、现有 suite/case 计数原位登记、layers 与
executeOwner 全部显式；python kexpr（T012--T014 六个）与 system entries
（closure runner、MiniNDN、installed-consumer）同步冻结。run identity 为实际注册：
tests/wscript `unit-tests` program（ant_glob unit-tests/**/*.cpp excl sanitizer）→
`build/unit-tests`；`./waf build --targets=unit-tests -j2` 构建、Boost.Test
`--run_test=<Case>` 选择具名 case、`--list_content` 确认注册非空。契约同步：
proof-design.md Rev 7（冻结句 + L0 installed-library 命令及 NAC-ABE 5 符号 gate）、
code-design.md Rev 8（Open Questions O-002/O-004 → CLOSED，表内已无 OPEN 项）、
work-units.md Rev 8、spark-execution.md 冻结句。check-prerequisites/validate_design/
git diff --check 见 [c-freeze evidence](evidence/t001-c-freeze-20260907.md)。
下一步：NAC-ABE ABI（5 符号）匹配后重跑 T002-A 首次 L0；T002-A 依赖现已满足，
当前 BLOCKED 仅剩工具链缺口。

2026-09-07 T001-A/B closure / **T001-A DONE、T001-B DONE、T001-C READY**：
按 T001-A/B 卡 Verify（DOC + 依赖契约/双向映射核对）完成设计关闭，证据
[closure](evidence/t001-ab-closure-20260907.md)。实际执行与核对：ONNX probe
4/4 与 tokenizer ABI probe 84 exact+14 negative 在现工具链上全新复现 PASS；
product tokenizer bridge `--release --locked -j2` rc=0（raw
`.codex-tmp/spec182-t001-dependencies/tokenizer-r2/`），failure-log 的 Cargo
exit127 边界已在该独立 rust-prefix（rustc/cargo 1.90.0）上解决；identity 向量
24+16+17 与设计声明及冻结 sha256 一致，A7-07 缺陷诊断保留原位。O-004 收口
处置写入 runtime-boundaries.md（Rev 8）与 symbol-design.md（C21/Readiness），
registration generation/late ACK/Selection/共享 lease 设计已在 lifecycle 设计
冻结。字段/方法/错误 parity 按各契约原文归 T012 及 owner 任务。产品构建仍被
NAC-ABE ABI 缺口阻塞（下一个工具链边界）。
下一步：T001-C Dispatch and Selector Freeze（依赖已满足）。


2026-09-07 O-004 mapping closure / **T001-B IN_PROGRESS**：formal `api` 的 18 项
`UNREVIEWED` 全部完成 owner 语义映射（`PARTIAL_EXISTING_TYPE 10`、
`PLANNED_TYPE 17`、`UNREVIEWED 0`），分类写入
`checklists/build_api_migration_manifest.py` 的 `API_MAPPING_OVERRIDES`
（`InferenceApplication→NativeInferenceClient/NativeConversationCoordinator`、
`RequestRef→NativeInferenceHandle` alias、部署契约十项 → T008/T010
`runtime-boundaries.md#cd-013`、`ProviderDeploymentOffer(s)→NativeProviderPlanningView`、
`ModelIntent/OptimizationObjective→T003 策略输入、RequestContract/
RequestableDeployment→T010 请求契约），manifest 按当前 source commit 重新生成
（344 项），`public-api-migration-review.md` 状态更新为 O-004 MAPPED
（parity open）。字段/方法/错误 parity 与真实调用方逐项核对仍属 T012 及对应
owner 任务义务；O-004 静态映射收口不关闭 T001 或任何产品任务。
下一步：T001-C selector/build identity/case manifest 冻结（依赖 T001-A/B
完整验收），然后工具链（Cargo 安装、NAC-ABE ABI 匹配）→ T009 → T010。


2026-09-07 Skill surface / **PASS**：按用户要求精简共享个人技能 105→18，GSD 69 个技能、
34 个 agent 注册和 4 个 hook 退出活动配置，原文件留本机归档；项目 12 个 Spec Kit 技能保持。
constitution 1.5.0 改由 Spec Kit 进度/证据负责多阶段恢复，历史 `.planning` 保留。
配置语义核对通过；已运行客户端需重启才能卸载旧 agent/hook，未声称本会话已刷新。
产品状态不变；详见 [registry evidence](evidence/task-progress-registry-20260907.md#skill-surface)。
下一步重启客户端核对精简目录，然后继续 T001 收口。

2026-09-07 Native boundary follow-up / **T001 IN_PROGRESS**：补齐 ONNX
protobuf 生成源到 provider、fault-provider、assembly-parity、smoke 和
integration 的 Waf 闭包（`76d074af`），移除装配器废弃 JSON/路径 helper
（`e0a4e248`）；Python 扩展不再重复编译 `NativeGrantVerifier.cpp`，只链接
安装的 DI 库（`2eb259f7`）。嵌套 graph external initializer 的绑定/内联和
超大输入上限已由 `b4f02615` 覆盖。相关静态检查及 Spec182 Python 门禁仍
通过；Cargo 缺失、NAC-ABE ABI mismatch 和完整请求编排未解决，产品仍 **0/17**。
下一步在匹配工具链上完成 bridge/全量链接，再推进 T010/T011；不把本地
focused 结果写成 T016 qualification。

2026-09-07 Spec Kit default / **PASS**：通用 tasks-template 默认包含 Execution Progress 和
Current Checkpoint；任务生成/执行技能增加覆盖检查、增量更新及保留既有状态规则。
仅工作流文档更新，不改变上表产品状态。检查见 [registry evidence](evidence/task-progress-registry-20260907.md#spec-kit-default)。
下一步所有新任务清单沿此模板生成，Spec182 继续按现有 T001 缺口推进。

2026-09-07 Progress registry / **PASS**：36 个执行单元统一登记到上表，详细卡改为通用执行契约。
旧 Spark 路径保留历史入口；本次仅整理进度和规则，未实现或验收产品，不改变父任务勾选。
检查和状态来源见 [registry evidence](evidence/task-progress-registry-20260907.md)。
下一步关闭 T001 公开 API/设计 release 缺口，并处理已记录的构建依赖边界，再按 Gate Order 推进。

2026-09-07 Native component and binding implementation slice / **T001 IN_PROGRESS**：
已加入原生 ONNX recipe assembler（protobuf structural assembly、external
initializer digest binding、deterministic serialization）、grant issue/publish
ports、request preparation/admission ports、conversation journal coordinator、
Provider host registration seam，以及 `pythonWrapper` 的单一薄 pybind11 DI
入口。新增 C++ focused tests 均完成语法检查；Python binding/build-boundary
测试 **17/17 PASS**。绑定只映射 native DTO/错误/句柄/策略类型，`NativeServiceUser`
通过生命周期保持创建 native client，不在 Python 复制 planner 或状态机。
显式 `NDNSF_LIBRARY_DIR` 现在同时要求 Core 和 DI shared library，避免链接回退。
ONNX protobuf 生成源已纳入 Waf；全量 Waf/native qualification 仍受既有
NAC-ABE ABI mismatch 阻断，不能据此关闭 T002/T006/T009/T010 或 T012。
新增 installed-consumer Waf 目标（checkpoint `aba90194`），其公共头语法检查
通过；Spec182 相关 Python 门禁 **29/29 PASS**，设计校验 `errors=[]`。
Rust tokenizer bridge 的本机 release 构建在 `cargo: command not found`
（exit127）处未开始，原始边界见 `.codex-tmp/spec182-tokenizer-r1/`，不能
替代真实 bridge/ABI 验证。T001/O-004保持OPEN，产品任务仍 **0/17**；下一步
在可用同源 Cargo 工具链上完成 bridge/consumer 检查，并继续 T010/T011 的真实
request/stream 接线后再做 T015 静态收敛审查。

2026-09-07 Tokenizer identity guard / **T001 IN_PROGRESS**：修正
`NativeTokenizer` 在加载动态 bridge 前校验 tokenizer 文件摘要，并拒绝空输出
缓冲区；新增 3 个身份/工厂负例单测，独立 Boost.Test **3/3 PASS**（commit
`54595eee`）。Cargo 缺失仍使 Rust bridge 的真实构建保持 **NOT_RUN**，不关闭
T007/O-004。

2026-09-07 Public export inventory / **T001 IN_PROGRESS**：新增[API migration review](contracts/public-api-migration-review.md)和可复现AST snapshot，覆盖api27/sdk76/root174，共277导出，264定义/10assignment/3外部owner。发现正式api中23个名称尚无四份主契约的精确映射；部署catalog、请求handle和provenance不能由现有request概述替代。snapshot逐条UNREVIEWED，动态wildcard/继承/实例字段仍需核对；不是迁移完成。下一步逐行为完成正式api映射及动态层清单，O-004/T001保持OPEN，产品0/17。未修改产品源码、未运行native产品测试。

2026-09-07 Compatibility manifest source review / **T001 IN_PROGRESS**：`build_api_migration_manifest.py` 已改为保留类方法的参数注解/默认值、顶层函数签名和 assignment expression；`RequestRef` 明确解析为 `InferenceRequestHandle`，`RequestableDeployment` 明确保留其 `Union` 表达式。生成物覆盖显式277、动态67、总计344项；formal api 当前静态状态为 `PARTIAL_EXISTING_TYPE 8`、`PLANNED_TYPE 1`、`UNREVIEWED 18`。这只补足 O-004 的机器可读审阅入口，不等价于字段、错误、状态、caller 或 native 行为闭环；未关闭 O-004/T001，未修改产品源码，未运行 native 产品测试。已完成 `py_compile`、manifest invariant check 和 `git diff --check`；下一步继续逐项补齐 O-004 后才释放 T001-C。

2026-09-07 Compatibility caller classification / **T001 IN_PROGRESS**：manifest 继续保留全部 token mention，并新增 `maintainedCandidates`、`tests`、`generatedCopies`、`other` 分组；`packaging/*/build` 副本不再与维护入口混为一谈。分组仍是静态审阅辅助，不替代 owner 的真实调用语义判断，也不改变 `UNREVIEWED`/O-004 状态。已完成生成物 invariant、`py_compile` 和 `git diff --check`；未修改产品源码，未运行 native 产品测试。

### Prior Provider Lifetime

上述inventory单元277个导出键无重复，36个定义文件SHA256与当前源码一致；strict structure、design validator（214本地链接）及diff whitespace检查PASS。AST扫描不导入运行依赖，分namespace生成均exit0；初次全集工具输出截断后改为分namespace读取并合并，未把截断结果作为snapshot。

2026-09-07 Spark execution preparation / **DISPATCH_DESIGNED**：用户指定Spark为后续实现执行者。
已增加[execution cards](contracts/spark-execution.md)，保留17个父任务，补齐定向Read、精确Write、步骤、依赖和planned selector；
共享设计技能及本机tasks/implement入口支持bounded-executor。T001设计关闭与selector release仍是产品前置门，
本轮不替其他设计工作宣告关闭O项，也不勾选任何产品任务。36卡/17父任务覆盖、依赖、211链接、strict structure及三项validator拒绝反例PASS；
技能通用校验器与既有Spec Kit metadata的schema差异及替代检查见[spark preparation](evidence/spark-execution-preparation.md)。
下一步由T001-C汇总已有设计关闭证据、冻结实际选择器和构建入口，再从T002-A按依赖交给Spark执行。
**Spark trial NOT_RUN；产品仍0/17。**下方Provider等记录保留各自设计范围与历史验证事实。

### Spark Checkpoint

历史准备见 [preparation checks](evidence/spark-execution-preparation.md)。
当前状态统一到 [Execution Progress](#execution-progress)，此处不再维护第二张表。

### Prior Provider Lifetime Design

2026-09-07 Provider lifetime design / **T001 IN_PROGRESS**：在[lifecycle contract](contracts/native-provider-lifecycle-design.md#provider-lifetime-control)定义受mutex保护的RegistrationControl、close/post/析构互斥、锁外释放capture及post失败后续清理；补齐ACK同步/异步、Selection、普通lease执行fallback、完成发布检查。源码确认pending cleanup可能早于兄弟role结束，因此Selection将代次转入协作生命周期，work fence不依赖pending表。M47同步指向新增Core scoped接口；不是包装不存在的unregister。Provider生命周期设计范围已明确，O-004其余公开类型/调用及整体T001仍未完成，产品0/17。下一步归并设计清单并核对剩余缺项，不重启已闭合算法研究。

### Prior Registration Generation

上述lifetime设计单元strict structure、design validator（186本地链接）及diff whitespace检查PASS。未构建/运行native产品；新增生命周期单测仍NOT_RUN，文档检查不计T009完成。

2026-09-07 Registration generation / **T001 IN_PROGRESS**：源码确认ACK复制旧handler异步执行、Selection重新查当前service handler，单独DI closed包装不能隔离重注册旧请求。已在[lifecycle contract](contracts/native-provider-lifecycle-design.md#registration-generation-decision)定义Core scoped registration、pending代次绑定、ACK完成/Selection/work fence检查及幂等close清理范围；不增加wire或授权owner，不删除legacy API。尚需补齐Provider存活控制与同步fallback位置，T009仍BLOCK、T001/O-004未完成、产品0/17。本轮无native产品构建/测试；下一步沿新增Core范围完成可实施设计，不重新讨论已确定的代次机制。

### Prior Shared Lease Design

上述registration设计单元strict structure、design validator（183本地链接）和diff whitespace检查PASS；只支持文档checkpoint，不表示Core扩展已实现或生命周期测试通过。

2026-09-07 Shared Provider lease design / **T001 IN_PROGRESS**：核对Core addService/addCollaborationHandler会覆盖同名handler，且没有公开unregister；当前单服务CLI为固定lease入口创建单target私有表，不能直接逐serve复制。新增[Provider lifecycle contract](contracts/native-provider-lifecycle-design.md)，定义一个host共享lease表/prepare mutex、单入口target路由、跨target绑定验证、关闭期间只清理旧lease及精确类/字段/constructor迁移。此为拟议多服务集成风险的源码推导，没有运行native产品；现有单服务结果不被改判失败。

本单元strict structure、design validator（182本地链接）及diff whitespace检查PASS；无产品构建/测试。T001/O-004及T009仍未完成：下一步关闭registration generation、晚到ACK/Selection及Core入口生命周期，再完成其余公开类型清单。产品0/17，SIF/Tiger不介入。

### Prior Stream Caller Closure

2026-09-07 Stream caller/recovery closure / **T001 IN_PROGRESS**：补齐[token stream contract](contracts/native-token-stream-design.md#production-caller-inventory)的paired factory、认证摘要绑定、唯一生产CLI注入点及三个integration consumers；定义NativeInferenceOperation的accept/replacement/final检查。源码确认现有AutomaticStreamingHandle接受前缀只在内存，runtime journal不在逐token接受链；保留进程内replacement与已提交conversation恢复各自边界，禁止终止token后重启生成，新增final text一致性义务。A7-08相关设计已定义，产品修复/测试仍OPEN；O-004其余公开API/schema/Provider注册生命周期仍需关闭，T001未完成、产品0/17。

本轮仅源码核对与契约修订，没有native构建或运行测试。strict structure、design validator（180本地链接）及diff whitespace检查PASS。下一步统一收口O-004剩余注册生命周期及完整公开类型/调用清单，避免再重复已关闭的tokenizer算法问题。

### Prior Qwen Stream Design

2026-09-07 Qwen stream design / **T001 IN_PROGRESS**：读取交付清单固定revision的tokenizer.json，12,807,982 bytes及SHA256完全匹配，确认ByteLevel decoder；本地另一Qwen工件身份单独记录，不混用。新增[token stream design](contracts/native-token-stream-design.md)，定义stable API/第六私有ABI、所有权、ByteLevel与ByteFallback不同算法、终止flush及epoch候选/提交接线。A7-08剩余完整调用方和journal接受边界仍OPEN，T001未完成、产品0/17。

独立`/usr/bin/python3 tests/fixtures/spec182/dependency-probes/check-bytelevel-stream.py`实际exit0：65,536个two-byte序列、9个长/非法/截断序列和3个whole-token fallback检查PASS；Python标准库增量UTF-8结果与固定HF0.20.3完整decode对照。strict structure、design validator（178本地链接）和diff whitespace检查PASS。仅reference诊断，不执行新增ABI/native产品。下一步关闭事件接受/恢复与全部factory调用方，随后统一收口O-004。

### Prior Stream Boundary

2026-09-07 Stream boundary / **T001 IN_PROGRESS**：新增可移植[reference checker](../../tests/fixtures/spec182/dependency-probes/check-stream-boundaries.py)，固定tokenizers0.20.3及既有fixture哈希，7个边界输入实际PASS/exit0。确认完整UTF-8的byte run仍会被后续无效byte改写；合法U+FFFD不能删除，skip special不构成run边界。算法与新版HF原生stream能力比较写入[generation contract](contracts/native-generation-design.md#verified-boundary-and-planned-bytefallback-algorithm)。本轮没有native产品构建/测试。

A7-08 的本轮收口原则：完整decode仅用于完整文本验收，不可替代stream边界；`decodeStable`与`eventSink`恢复边界仍是T007/T011/O-004闭环项。

A7-08仍OPEN：ByteFallback适配选择已明确，但真实Qwen decoder pipeline、完整调用/字段与事件接受后失败的恢复边界尚未关闭。源码确认eventSink接受后仍执行反馈发布与runtime commit，不能承诺靠decoder局部rollback撤回事件。T001未完成、产品0/17；下一步从实际Qwen工件和既有journal/commit路径关闭这些剩余设计，不重跑已固定reference用例。本单元strict structure、design validator（173本地链接）及diff whitespace检查PASS；参考运行命令为`/usr/bin/python3 tests/fixtures/spec182/dependency-probes/check-stream-boundaries.py`。

### Prior Generation Design

2026-09-07 Generation design / **T001 IN_PROGRESS**：[native generation contract](contracts/native-generation-design.md)补齐GenAI/HF/ORT复用比较与现有sampler的参数、double精度、去重惩罚、截断后归一化及旧会话兼容处置。A7-10 已按复用边界清单闭环：GenAI仅作能力对照，不作为默认生产路径；A7-11 CLOSED。A7-08 的完整decode/stream边界与A7-09 的采样数学均为设计约束，不等于实现/通过；A7-08/A7-09与O-004仍OPEN，T001未完成、产品0/17。修订plan旧授权句；不新增生成引擎或产品依赖，不操作实验机器。

本单元检查 **PASS**：prerequisites、strict structure、design validator和diff whitespace。独立Python reference源SHA256 `3affb6f438a4134bb8e69222d79b3f2ec5a6b256bf0022af636b065627397c11`；将log概率按`struct.pack/unpack('<f')`量化后输入`[-0.5108256340026855,-1.2039728164672852,-2.3025851249694824]`，seed8/top_k3/top_p0.8/temperature1/step0实际选0；重复惩罚例也实际选0，assert通过/exit0。仅运行标准库reference，不构建native或运行产品测试。已有ONNX normalization草稿与failure-log修改保留，不在本单元宣称O-002关闭。

下一步关闭stream decoder的preview/commit/flush/restore算法和逐调用方清单，再完成O-004及T001。下方审计检查点为历史事实，不覆盖本段状态。

### Prior Native Reuse Review

2026-09-06 Native reuse review / **BLOCK for implementation**：核对原生库复用与当前源码，ONNX/ORT及HF Rust tokenizer方向合理；新增A7-08流式decode前缀不稳定、A7-09已有C++ Top-P归一化及重复惩罚与Python reference不一致，A7-10缺GenAI复用对照、A7-11计划旧授权语句。详见[native reuse review](evidence/native-reuse-review-20260906.md)。reference诊断exit0：多byte-token文本展示prefix重写，两个采样输入Python实际返回0而native源逻辑推导为1；未运行native产品。raw `.codex-tmp/spec182-native-reuse-review-20260906-r1/`。T001/O-002/O-004保持OPEN、产品0/17；下一步在T001冻结复用/stream/采样兼容处置，再由T007/T011/T016实现和证明。本轮仅审计记录，不改产品源码或既有oracle。

本审计记录检查 **PASS**：Spec Kit prerequisites、strict structure、design validator（163本地链接、17任务/0完成）、`git diff --check`；reference诊断exit0及具体结果见同一review/raw。这些只允许保存审计记录，不关闭A7-08/A7-09、O-002/O-004或T001。

checkpoint首轮被本地pre-commit全索引引用检查拒绝（exit1）；已核对hook提供`NDNSF_LOCAL_CHECKPOINT=1`专用模式，后续本地提交使用该模式且保留禁止路径检查，原始`commit-r1.json`不覆盖。未修改hook、未push；其他工作正在追加的typed-complex诊断不纳入本审计提交。

### Prior ONNX Contract Checkpoint

2026-09-06 T001 ONNX contract / IN_PROGRESS：补齐[native ONNX assembly design](contracts/native-onnx-assembly-design.md)的owned类型、9个函数、8步算法、recipe/manifest字节与native worker生命周期；修复直接进程内替换会丢失硬超时回收的设计缺口。原始Python实现仍未修改；独立reference提取24个普通numeric raw/typed向量PASS。额外诊断确认BFLOAT16 raw摘要内容错误、STRING跨进程摘要不稳定，见[identity evidence](evidence/identity-reference-20260906.json)及failure index。**O-002/O-004仍OPEN，T001未完成，产品实现0/17**。下一步冻结这两种表示的稳定身份/兼容处置，并继续完整能力和Provider注册清单；不复制错误oracle、不以I/O dtype代替initializer能力范围。

本单元检查 **PASS**：strict structure、design validator（160本地链接、17任务/0完成）、diff whitespace；24个model hash、12对identity、6条诊断及未修改reference源hash核对一致。generator锁定原onnx/numpy/source版本，补锁后typed诊断仍通过。Context Mode健康检查PASS，但relevance检索返回了较早的Dependency Design子节，checkpoint以实际tasks顶部为准；CodeGraph的临时副本结果仍剔除，算法按精确生产路径核对。未构建/测试产品，不把发现旧缺陷或补全设计计作T006完成。

### Isolation Design Checkpoint

2026-09-06 T001 isolation design：**O-005 CLOSED**。已冻结[native isolation design](contracts/native-isolation-design.md)的最小root/namespace、权限与服务白名单、逐进程exec/映射/endpoint观测、harness函数/字段和I01--I08反例。bwrap0.4.0/strace5.5工具正例exit0且CapEff=0/NoNewPrivs=1，缺解释器反例在exec边界ENOENT/exit1，均符合预期；raw `.codex-tmp/spec182-t001-isolation-r1/`。没有运行NDNSF、MiniNDN、SIF/Tiger，也未实现T014 detector。当前O-001/O-003/O-005按各自设计范围CLOSED，**O-002/O-004仍OPEN，T001仍未完成**；下一步补完整ONNX算法和迁移/注册/状态清单。

本隔离设计单元检查 **PASS**：strict structure、design validator（149本地链接、17任务/0完成）、`git diff --check`。已静态核对工具选项、namespace/文件根与外部harness分界；最小工具正反例不外推NFD/Repo/Controller或全部后代观测的运行证明。此前依赖设计单元已本地提交`5187e733`，tracked tree随该单元清理，原始构建/日志未入Git；未push。

### Dependency Design Checkpoint

2026-09-06 T001 / IN_PROGRESS：用户已授权在Experimental完成Spec182，按原目标实施与本地验收；上一轮只审计的范围已结束。先关闭O-002--005，不提前迁移业务或执行最终集成/MiniNDN。已补齐tokenizer特殊token参数，建立[依赖设计与探针契约](contracts/native-dependency-design.md)。T001保持未勾选；SIF/Tiger继续外部负责。

T001运行边界：ONNX1.17.0原生依赖-j2构建及四向量探针 **PASS / exit0**，四个模型字节与原Spec181 oracle完全一致。tokenizers0.20.3/Rust1.90.0静态ABI与C++consumer构建 **PASS**，84个完整ids/text对照和14个拒绝检查 **PASS**；两探针ldd均无Python。ABI、生产桥接路径/字段/所有权、许可及依赖锁已冻结，**O-003 CLOSED**。O-002的完整算法/叶子契约、O-004完整兼容与状态设计、O-005隔离方案仍OPEN；T001和T007未完成。Rust工具链R1/R2下载TLS失败在R3更换HTTPS实现后恢复。详见同一依赖设计记录及failure index；不计产品任务完成。

本依赖设计单元检查 **PASS**：prerequisites、strict structure、design validator（143本地链接、17任务/0完成）、`git diff --check`；lock与两个oracle及三个tokenizer JSON的hash一致，Cargo.lock中72个registry依赖均有精确checksum。probe静态审查已在运行前完成，输入/expected来自旧版本，未改变产品源或历史oracle。恢复时Context Mode提供的`probe.cpp write` timeline查询未通过高熵identifier/source guard，改用持久tasks/contracts与实际日志核对，不从被拒绝的session检索推断状态。下一步补O-002/O-004/O-005；不重跑已通过且输入未变的探针。

### Prior Audit Checkpoint

2026-09-06 revision 7 / SOURCE_ALIGNMENT_COMPLETE：用户确认另一台机器已接收，本轮只审计/修订Spec182。以Experimental `81e251a4ef1d8e6a394dc5f0c38bc44e44bfc973`核对代码，修正未提交合并/旧integration失败的过时表述；固定NAC/SVS/NDNSD身份与旧运行证据失效范围。O-001按源码身份和181承接范围CLOSED；O-002--005仍OPEN，T001未勾选，实现仍 **0/17**。当前SVS/NDNSD组合 **UNQUALIFIED**，接收不等于实验通过。审计发现与关闭责任见[audit](audit.md)，具体版本见[integrated baseline](contracts/integrated-baseline.md)。

本轮未构建或执行产品unit/integration/MiniNDN/SIF/Tiger，未管理接收机器，未恢复已停止的ABI构建。下一步只继续T001的ONNX/tokenizer依赖、兼容/字段/注册寿命及隔离设计关闭；后续182开发验证仍按下方任务分工，上一轮delivery-only移交不取消T016义务。

本轮实际文档检查 **PASS**：Spec Kit prerequisites、strict structure、`checklists/validate_design.py`（136链接、17任务/0完成、依赖无环、19 FR/11 SC/14 CD/16 PO）、`git diff --check`；12类137字段AST名称/类型/默认值一致，19 FR/11 SC/16 PO/14负例/48方法/137字段条目相对审计输入未删改。以上不计产品验收；O-002--005保持OPEN。改动仅12份Spec182文档，历史evidence及产品源码不变。

## Historical Checkpoints

以下为各时点事实；其旧“下一步”、失败、未push及验证结果均不覆盖上方当前checkpoint。旧结果只适用于当时身份。

2026-09-06 Source Handoff COMPLETE / SOURCE_READY：用户明确本轮只交付，编译与测试在另一台机器执行。本机额外构建已停止并确认无遗留编译进程；后续NDNSD/全部ABI消费者构建、unit/integration、两wrapper验证、MiniNDN、SIF/Tiger全部TRANSFERRED，不再作为本机交付条件。D001范围修订为固定版本及ABI要求移交，D001--D004已完成；四库Experimental、可迁移输入/模板与root skills已发布。已有检查与中断事实保留，不冒称完整验证PASS。接收步骤及证据见 [source handoff](../../Experiments/TigerCluster/docs/source-handoff.md)。Spec182仍0/17，不计T001--T017完成。

2026-09-06 Existing Presentation Checkpoint：补存此前未跟踪的`docs/NDNSF-UAV/slides/UPDATES.tex`及对应4页PDF；标题和全部命名frame的PDF文本核对PASS。旧版`UPDATES_UAV.pdf`、LaTeX缓存、原始实验输出及本地助手状态继续保留为本地产物，不计源码或182进度。活动指针/managed plan均指182，最终索引刷新后project与active健康检查PASS。

2026-09-06 Branch Closure：`Experimental` 已快进至整合提交 `e91ecc91`，本地只保留 `main`、`Experimental`；原生验证工作树改为detached并保留二进制/raw，临时分支已删除。主工作区原426项源码/文档状态保存在具名恢复stash `4bb5e0a5`，实际成果已归并，不能直接pop旧测试覆盖修复。未push。另将用户既有UAV slides源文件/PDF单独checkpoint：30个frame与30页PDF、标题/日期和两个新增Geo-Capture页的文本对应检查PASS；这是既有演示文档保存，不计182产品验收。

2026-09-06 Experimental Consolidation：原生合并修复已成为真实merge commit `c770f18bb7bf42c3b8a8274b5c029b4883141f60`，包含远端UAV历史；同步本机 `2e7865c7` 的revision6和Tiger目录布局。最终工作分支统一为Experimental，main保持稳定基线。原生生产代码与已验证基线逐字节一致；unit **759/759**、integration **154/154**、current Python **2171 passed / 22 skipped**、三个真实MiniNDN授权/撤销场景 **PASS**，见 [integration closure](evidence/integration-20260906.md)。此前152/154是已修复的历史失败，不再控制合并状态。

新增 [integrated baseline](contracts/integrated-baseline.md) 固定Core/UAV复用接口、生命周期修复与181承接。O-001已提供实际commit/证据/承接表，T001仍须核对最终Experimental差异并关闭O-002--005；182实现仍 **0/17**，其最终产品验收 **NOT_RUN**。不再独立续跑181最终资格，下一步是T001设计关闭。

2026-09-06 revision 6：合并重复审查与报告；实现任务只做静态审查、相关unit及必要构建，集成与MiniNDN在全部实现后由T016统一运行。类/方法/字段设计和既定真实运行用例保留。

本轮只改技能与文档；实现 **0/17**，产品STATIC_REVIEW与unit/integration/MiniNDN均 **NOT_RUN**。
文档结构、107链接、依赖及技能引用检查PASS；既定PO-001--014/负例/运行用例与符号字段表的保留检查PASS。范围见 [workflow simplification](evidence/workflow-simplification.md)。
O-001--005和合并修复状态未被本轮关闭。下一步执行T001确认基线与设计就绪。

## Tiger Directory Checkpoint

2026-09-06 Consolidation Review Closure：当前Tiger源码一并归入Experimental。静态复审修复ACK/选择/请求关联、原生错根拒绝证据组合、有限应用进程组清理；新增负例先复现19项失败，修复后新工具目录 **58/58 PASS**，旧目录兼容及关联工具 **162 passed / 3 skipped**。B001/B002开发检查更新，B003实际SIF/双节点/复用验收仍未完成，Local R8 FAIL保留。用户已停止实验，本轮未运行SIF/Tiger；详见 [baseline checkpoint](../../Experiments/TigerCluster/docs/two-node-baseline.md#usage)。

2026-09-06 Two-node baseline IN_PROGRESS：新增[基础实验契约与进度](../../Experiments/TigerCluster/docs/two-node-baseline.md)，独立B001--B003负责共享runtime、双节点profile、真实NDN/NDNSF探针及实际运行复用；不改变Spec182原生迁移任务状态。B001/B002实现与静态审查完成，相关unit **24/24 PASS**；本地/远端SIF哈希一致。B003尚待精确SIF集成与实际双节点/复用运行，尚无实验PASS。

2026-09-06 Usage Guidance：本地工作约定已补充Tiger唯一入口、共享owner、旧路径同步和双机分工；可随Git交付的说明见[Tiger README](../../Experiments/TigerCluster/README.md#compatibility-and-review-boundary)。路径/链接及文档一致性检查PASS；本轮只改说明，不运行产品测试、不改变实现进度或验收状态。

2026-09-06 Follow-up R2：64个迁移文件的哈希/权限/旧新路径、29个共享文件与审查工作树对比、Tiger文档本地链接均PASS，无新增同步差异。两份已同步测试未变，沿用R1的58/58工具单测证据，本轮不重复执行。审查工作树完整integration日志为152/154 PASS、2 failed；整合未完成，目录迁移无需追加修改，等待该owner交付最终基线。详见 [follow-up evidence](evidence/tiger-directory-migration-20260906.md#follow-up-r2)。

2026-09-06 Review Sync R1：64个迁移文件及共享lib/bin无新差异；两份测试修正已从审查工作树同步，相关工具/collector单测 **58/58 PASS，exit0**，关闭上轮原因码断言失败。源码迁移与原生实现状态不变；证据见下方migration evidence。

2026-09-06：Tiger目录迁移完成，64文件内容/权限与路径静态检查PASS；工具单测50 PASS / 1 FAIL，独立原布局已复现相同原因码失败，留给原工具owner。纯路径checkpoint保留49个已跟踪文件原HEAD内容，已有修改及15个未跟踪文件在新路径继续保留，不混入迁移提交。详见 [migration evidence](evidence/tiger-directory-migration-20260906.md)。本工作不计T001--T017实现或产品验收。

## Validation Standard

唯一规则见 [validation workflow](contracts/pre-test-static-review.md)。
T002--T014的任务[x]仅代表本任务实现、静态审查、相关单测和必要构建完成；
其Proof行引用完整行为义务，跨组件/跨进程与MiniNDN证明登记给T016，不要求各任务提前运行。
T015在全部实现后补审整体接线；T016执行完整unit→integration→MiniNDN并关闭全部必需PO。
不得将真实集成重命名为unit/smoke提前执行。Static review PASS != Behavior PASS。
每任务只保留简短结果或一份evidence链接，最终核对diff与证据，不另建S0/S1报告。

## Phase 1: Design and Native Components

- [x] T001 [US5] **Successor Baseline and Design Closure**。冻结合并基线与181承接表、所有 schema/公开调用方/能力清单及原生依赖，关闭 O-001--005；修订叶子签名与任务至可执行。Dependencies: Merged baseline closure and Spec181 handoff。
  Design: FR-015,FR-016,FR-017,FR-018; CD-001--014。Proof: PO-012。
  [T001 contract](contracts/work-units.md#t001-successor-baseline-and-design-closure)。

- [x] T002 [US1] **Installable Native Library Contract**。existing Provider runtime 可独立安装/链接；planned requester 公开声明先冻结，完整request由T010实现、T016运行验收。Dependencies: T001。
  Design: FR-001,FR-012; CD-001,CD-009。Proof: PO-001。
  [T002 contract](contracts/work-units.md#t002-installable-native-library-contract)。

- [x] T003 [US2] **Native Split and Placement Decisions**。两个原生模型 splitter 与默认 placement 对固定输入生成合法且确定的方案。Dependencies: T002。
  Design: FR-003,FR-009,FR-016; CD-002。Proof: PO-002。
  [T003 contract](contracts/work-units.md#t003-native-split-and-placement-decisions)。

- [x] T004 [US1] **Canonical Native Plan Sealing**。合法 proposal 转成可被真实 Core/Provider 接受的规范计划；非法投影在首边界拒绝。Dependencies: T003。
  Design: FR-002,FR-004; CD-003。Proof: PO-003。
  [T004 contract](contracts/work-units.md#t004-canonical-native-plan-sealing)。

- [x] T005 [US1] **Native Requester Grant Path**。原生 requester 签名/申请/发布 grant，实际 Provider 验证并消费密钥。Dependencies: T004。
  Design: FR-005; CD-004。Proof: PO-004。
  [T005 contract](contracts/work-units.md#t005-native-requester-grant-path)。

- [ ] T006 [US1] **Native Cold ONNX Assembly**。Selection后原生装配与既有固定bytes一致；原生worker保留有界取消/清理，删除Python helper及其文件IPC。Dependencies: T002；O-002 closed。
  Design: FR-006,FR-016; CD-005。Proof: PO-005。
  [T006 contract](contracts/work-units.md#t006-native-cold-onnx-assembly)。

- [ ] T007 [US4] **Native Tokenizer Execution**。原生 encode/decode 完整文本，与固定 tokenizer oracle 一致，无子进程解释器。Dependencies: T002；O-003 closed。
  Design: FR-007; CD-006。Proof: PO-006。
  [T007 contract](contracts/work-units.md#t007-native-tokenizer-execution)。

- [ ] T008 [US1] **Native Request Preparation and Admission**。原生输入/认证模型/工件准备与 offer policy 校验闭合，GraphAdapter/TaskAdapter 端口由 native 实现。Dependencies: T003/T006/T007。
  Design: FR-001,FR-002,FR-004,FR-009,FR-016; CD-013。Proof: PO-013。
  [T008 contract](contracts/work-units.md#t008-native-request-preparation-and-admission)。

- [ ] T009 [US3] **Shared Native Provider Host**。CLI/C++/Python 共用服务注册、准备/执行接线与停止语义。Dependencies: T006/T007。
  Design: FR-001,FR-009,FR-010,FR-012; CD-014。Proof: PO-014。
  [T009 contract](contracts/work-units.md#t009-shared-native-provider-host)。

## Phase 2: Invocation and Compatibility

- [ ] T010 [US1] **Complete Native Request Lifecycle**。独立 C++ requester 从模型/输入到真实 Response，cancel/deadline/late callbacks 保持单一终态。Dependencies: T003/T004/T005/T006/T007/T008/T009。
  Design: FR-001,FR-002,FR-008; CD-001,CD-013,CD-014。Proof: PO-001,PO-003,PO-007,PO-013,PO-014。
  [T010 contract](contracts/work-units.md#t010-complete-native-request-lifecycle)。

- [ ] T011 [US4] **Native Conversation Continuation**。原生 requester 续接/有限恢复与既有 epoch/state runtime 协作，文本/lineage 正确。Dependencies: T010。
  Design: FR-008,FR-016; CD-007。Proof: PO-007,PO-008。
  [T011 contract](contracts/work-units.md#t011-native-conversation-continuation)。

- [ ] T012 [US3] **Thin Python Native Bindings**。支持的 Python 调用转发同一 native 库，无 Python strategy trampoline/业务状态机。Dependencies: T011。
  Design: FR-010; CD-008,CD-009。Proof: PO-009。
  [T012 contract](contracts/work-units.md#t012-thin-python-native-bindings)。

- [ ] T013 [US3] **Default Route and Legacy Retirement**。所有 maintained callers 默认原生；旧运行时退出默认 import/调用图。Dependencies: T012。
  Design: FR-011,FR-016; CD-010。Proof: PO-010。
  [T013 contract](contracts/work-units.md#t013-default-route-and-legacy-retirement)。

## Phase 3: Proof and Delivery

- [ ] T014 [US5] **Runtime Dependency Exclusion Gate**。harness 将被测 native scope 与 Python harness 隔离，能拒绝已知 interpreter/libpython/helper 旁路；交付正式 MiniNDN harness/collector 并完成本地单测；真实隔离反例在T016运行。Dependencies: T013。
  Design: FR-001,FR-011,FR-012,FR-014; CD-011。Proof: PO-001,PO-010,PO-012。
  [T014 contract](contracts/work-units.md#t014-runtime-dependency-exclusion-gate)。

- [ ] T015 [US5] **Design-code Convergence Audit**。整体静态读码核对FR/CD/INV/PO、生产接线、test/oracle/harness与依赖身份，控制性发现清零；记录整体审查结论并进入T016。Dependencies: T014。
  Design: FR-013,FR-017,FR-018; CD-001--014。Proof: PO-001--016。
  [T015 contract](contracts/work-units.md#t015-design-code-convergence-audit)。

- [ ] T016 [US5] **Local Native Qualification**。全部实现和T015审查完成后，同源完整unit→integration→YOLO/Qwen MiniNDN/no-Python及必要检错证明通过，核对最终diff与证据。Dependencies: T015 PASS。
  Design: FR-001,FR-005,FR-006,FR-007,FR-008,FR-010,FR-011,FR-012,FR-013,FR-016; CD-011。Proof: PO-001--016。
  [T016 contract](contracts/work-units.md#t016-local-native-qualification)。

- [ ] T017 [US5] **Native Development Handoff**。唯一开发交付版本、维护文档与两个入口示例；外部实验单列 TRANSFERRED。Dependencies: T016 PASS。
  Design: FR-014,FR-015; CD-012。Proof: PO-012。
  [T017 contract](contracts/work-units.md#t017-native-development-handoff)。

## Dependencies & Execution Order

Merged baseline closure and Spec181 handoff → T001 → T002；T003/T004/T005 依次收口；
T006/T007 依赖 T002 和已关闭 native dependency design；
T003/T006/T007 → T008；T006/T007 → T009；
T003--009 → T010 → T011 → T012 → T013 → T014 → T015 PASS → T016 → T017。
所有任务遵循FR-018/019和统一验证规则；每任务具体范围见work-units。只有最早未关闭门可进入其对应实施。每次失败先保留新 raw/evidence、更新本 tasks/failure index。
最终 checkpoint 前核对 task 状态与实际 diff/PO；不得 blanket stage 预存修改。
