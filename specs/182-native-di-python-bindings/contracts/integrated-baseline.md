# Integrated Baseline and Successor Transfer

**Revision**: 6
**Source Status**: VALIDATED / COMMITTED
**Baseline Commit**: `c770f18bb7bf42c3b8a8274b5c029b4883141f60`
**Implementation Status**: NOT_STARTED

## Source Identity

本基线整合本机 DI 与远端 UAV 成果，替代“先完成181最终关闭”的激活前提。NDNSF 两个父提交为 `d4a5e39ce5b4a023f6e55d2440c60aa998983f8f` 和 `4391af81cd24ff5510aa52b48ab9cec0fdec1ebb`；NAC-ABE 为 `5ed23e6`（含 Experimental `c3aafa6ec5a566879942107c7b20855659c9dfb9`）。本机未提交开发源码已纳入整合审查，不能只按两个分支的 committed diff 判断功能。

已形成上述真实双亲 merge commit；随后同步 `2e7865c7a031f677e1ff382cf999392cf0aacd0a` 的 Spec182 revision6 与 Tiger 目录迁移，保留原生生产源字节。最终开发分支为 `Experimental`，`main` 保持稳定基线；`UAV-Experimental` 的提交历史必须为其祖先。源码/工件证据保留在原验证目录，不把未完成的 Tiger 双节点实验当成本次验证。

O-001 的合并提交与本页181承接表已提供；T001 仍须对最终 Experimental 树确认调用方/依赖/叶子契约，并关闭 O-002--005。修订本 Spec 不授权 push 或原生功能迁移。

## Verified Local Evidence

精确原始目录、退出码、范围与 SHA256 见 [validation record](../evidence/merge-validation-20260906.json)；诊断与修复见 [integration](../evidence/integration-20260906.md) 和 [static review](../evidence/static-review-20260906.md)。

| Gate | Result | Boundary |
| --- | --- | --- |
| NAC-ABE unit | PASS / 46 cases | 新依赖独立 prefix；不替换主机旧安装 |
| NDNSF unit | PASS / 759 cases / 63846 assertions | generated-plan 外部模型缺失时的 smoke 不等于模型资格 |
| NDNSF integration | PASS / 154 cases / 3634 assertions | GDB 正常退出；D2h runner 为确定性模型 runner |
| Python current compatibility | PASS / 2171 passed / 22 skipped | 明确选择范围；历史全量的128失败保留，不能宣称全量 Python PASS |
| MiniNDN user revocation | PASS / 12 checks | 撤销前成功、获知后拒绝、对照用户继续服务 |
| MiniNDN grant-only advance | PASS / dedicated grant gate | 新授权可调用；无关主体不重取 DKEY；ABE 参数不变 |
| MiniNDN provider revocation | PASS / 14 checks | 含主动 SIGINT 后重启，旧 Provider exit -2，重启后仍拒绝 |

实际 native closure 使用 system GCC/binutils、Boost1.71、明确 NAC prefix 和 NDN-SVS source/build pair，最大 `-j2`。Core SHA256 `5fa424800613f697ecffb04b0b4b43b17f6bed06adbd0a59903558ba97da74fd`；完整库列表见 JSON。AGENTS 保留路径/ABI 前置检查；MiniNDN 的 root PATH 还必须包含 `/usr/sbin:/sbin`。

## Existing Core Contracts to Preserve

以下均为 `existing`，不是182计划新增的第二套机制。T001 必须把实际源与这些签名核对，再将受影响调用方纳入 O-004 清单。182 若需要改变它们，先修订 CD/PO 和精确文件/函数/字段 diff；不能在移植 DI 时顺便重写 Core。

| File / class | State and methods | Ownership and parameter meaning |
| --- | --- | --- |
| `ndn-service-framework/ControllerVersion.hpp` / `ControllerVersion` | `uint64_t controllerGenerationTimestamp`, `uint64_t controllerEpoch`; `isValid()`, `compare(other)`, `wireEncode()`, `wireDecode(block)` | 重启 generation 与策略 epoch 单调版本；不同字段不可折叠为单个计数 |
| `ndn-service-framework/ControllerGenerationStore.hpp` | `acquireWriter(owner)`, `ownsWriter`, `fencingValue`, `load`, `loadRevocations`, `startGeneration(now, revocations)`, `advanceEpoch(revocations)` | 唯一写入者、原子持久化及 fencing；恢复不得重用旧权威版本 |
| `ndn-service-framework/ServiceController.hpp` / `ServiceController` | `grant(identity, service, authorizationAttribute) -> bool`; `revoke(target) -> bool`; `getControllerVersion`, `getPolicyStatus(service)`, `isRevoked(identity, service, certificateDigest, authorizationAttribute)` | grant 的第三参数是 `ndn::Name` 授权属性，不是 bool；User 与 Provider 授权属性分开。grant-only推进版本并刷新目标DKEY而不轮换ABE；revocation同样推进版本并轮换ABE。失败维持拒绝并允许受控重试 |
| `ndn-service-framework/PolicyStatus.hpp` / `PolicyStatusData`, `RevocationTarget` | version、validity、policy digest、精确 ABE 参数名/摘要、Controller certificate、revocations；`sameCertificateDigest(left, right)` | target 区分 identity/certificate/service authorization；结构与时效验证不替代签名验证；仅有效完整 SHA256 摘要容许 hex 大小写差异，opaque ID 保留原相等语义 |
| `ndn-service-framework/RevocationState.hpp` / `RevocationState` | `acceptStatus(status, nowMs, controllerSignatureValid)`, `authorize(subject, transition, now)` | 第三参数表示调用方已验Controller签名，不能盲设true；保留六个受保护转换边界；authenticated hint 不是可安装的权威状态 |
| `ndn-service-framework/ServiceUser.hpp`, `ServiceProvider.hpp` | `installControllerStatus(status, controllerSignatureValid=true)`, `getControllerVersion(service)`, `fetchPolicyStatusFromController(controller, service, attempt, expectedVersion)`, `authorizeControllerTransition`, `invalidateControllerScopedCaches`, `refreshNacDkeyForControllerStatus(service, version, abeGenerationChanged, grantOnlyDkeyRefresh)` | 原status取数、验签、缓存失效和DKEY刷新复用；install的bool由已验签调用方提供，函数不自行完成密码验证。refresh的两个bool表达 ABE generation 变化和目标 grant 刷新，禁止混用 |
| `ndn-service-framework/PolicyRefreshCoordinator.hpp` | `installCurrentStatus`, `observeHint`, `startScheduledRefresh`, `completeFetch`, `failFetch`, `retryIfDue`（install/complete的`controllerSignatureValid=true`参数同样来自已验签调用方） | hint 只触发获取；按已验状态调度刷新，保留重试/过期拒绝 |
| `ndn-service-framework/RuntimeStatusStore.hpp` | `Record`; `load(out) -> bool`, `persist(records) -> bool` | signed Data、status wire、参数身份及时刻可持久化；不保存 DKEY 或请求密钥。恢复先重验签名、未过期及未被更新状态替代的持久Data，再安装status，随后调度有界在线确认；不声称所有受保护工作都等在线确认后才允许 |
| `ndn-service-framework/RequestConfidentiality.hpp` | `RequestSecurityBinding`, `RequestKeyBundle`, `canonicalAad`, Selection envelope wrap/unwrap、content encrypt/decrypt、`NonceRegistry` | 请求/attempt/version、双端证书、Selection digest、input Data name、segment/event ID 绑定；input/response key 分离并零化。复用 Core，禁止合并为 DI KeyGrant |

## Existing Lifecycle and Execution Repairs

| File / symbol | Required preserved effect | Proof |
| --- | --- | --- |
| `ServiceController::start`, `cancelStart`, `resetStartCancellation` | 注册 barrier、独立 probe Face、真实签名 challenge 和精确 PUBPARAMS readback；测试 DummyFace policy handler 与真实 readiness 分开 | 13项 readiness、真实 NFD 双探针、完整 Controller integration |
| `ServiceUser::registerIdentityPrefixWithRetry` / `m_identityRegistration` | 所有权可取消；弱生命周期检查、8次/250ms上限；构造后回调不引用局部函数对象 | 503重试成功、销毁取消、8次停止 |
| `ServiceProvider::makeCollaborationWorkFence` / `CollaborationWorkFence` | 入队即冻结原始更早 deadline；出队/安装/执行检查 stop、version、authorization、service binding；ACK/token 清理不取消兄弟角色 | 队列撤销/版本推进、D2h121/212 |
| `ServiceProvider::fetchAndDecryptLargeDataUntil` | 共享不可延长预算与 cancellation；public 两参数入口保持兼容；失败不安装部分状态 | 完整 unit/integration |
| Provider worker/destructor and posted callbacks | Provider 拥有并 join workers；投递前与回调内共享停止检查，析构后不能访问 `this` | queued、active、posted success/failure 四时机 |
| `NdnsfCollaborationDependencyIo` compact segment decode | 内层 AAD 按 capability 原字段恢复；外层 Selection edge deadline 仍约束实际 fetch | 不同 inner/outer预算的 D2h 数值 oracle |

## Existing UAV and Application Boundaries

- `UavMissionSession`：`create/start/cancel/reconcileVehicleAndStreams/beginIncidentAttempt/addJob/updateJob/acceptTerminalReport/snapshot/restore` 保持长期 mission 所有权。
- `UavIncidentCoordinator`：`begin(requestId, now)/closeAcks(now)/commitPlan(assignments, terminalOwner, planDigest)/acceptReport/retryAnalysis(stage, newRequest, newAttempt, now)` 保持有界 incident 生命周期；恢复新 attempt 不复用旧授权。
- `GroundStationServiceContainer.inc.hpp` 的 `BeginCollaboration → ACK closure → CommitCollaborationPlan` 生产调用继续使用 Core 协议，不能退化为旁路固定 Provider 调用。
- `ViewEvidenceReference/MultiViewRecognitionJob/MultiViewModelProfile/VerifiedMultiViewInput/FusedRecognitionResult` 与 `IMultiViewRecognitionAlgorithm::execute`、`executeMultiViewJob`、`UavDetectorProvider::verifyFetchedData/acceptValidatedContent/executeMultiView` 保留精确 Data 名、签名、hash、2–6视图和逐视图 provenance。
- `tools/mvcnn_onnx_worker.py::run_real_manifest` 属于 UAV 应用 CPU ORT worker；O-004 必须显式标注其与 DI no-Python scope 的关系，不因全库搜索 Python 就将其删除或宣称已经原生化。训练、数据准备、外部模型与 DI 运行时分别登记。
- Core wrapper 与 Repo wrapper 的显式 NAC/SVS/Core prefix、空路径拒绝及 wheel 所有权测试保留。O-004 还必须覆盖现有 Python 调用方及兼容入口，不能只列新 C++ facade。

## Spec181 Transfer Ledger

| Origin | Disposition | Spec182 owner / acceptance |
| --- | --- | --- |
| Spec181 T008 / final same-source local qualification | TRANSFERRED / NOT_COMPLETE | T001登记已有/未完成矩阵；T014承接harness；T015复审；T016在182最终同源代码重新证明，不借本次小范围MiniNDN替代 |
| Spec181 T009 / local delivery seal | TRANSFERRED / NOT_COMPLETE | T017冻结代码/依赖/配置/工件/adapter/harness身份；必须在T016之后 |
| Spec181 T012 / final closure reconciliation | TRANSFERRED / NOT_COMPLETE | T017逐条对账未完成项与回退/交付；不能回写181历史为PASS |
| Spec181 completed local work | PRESERVED / historical 7/10 | 保留原任务勾选与冻结 evidence；迁移后依赖相关证明必须重验 |
| SIF / TigerCluster | TRANSFERRED / external execution | 实验机器接收明确commit、构建SIF并执行/反馈；本机仅开发、unit/integration/MiniNDN及开发交付 |

Core Spec179 在线撤销与 DI KeyGrant 撤销是不同能力。原181 FR-003/Out of Scope 对 DI KeyGrant revocation 的延期继续有效；本次合并不能把 Core 撤销 PASS 当成 DI KeyGrant 撤销完成。若未来纳入，先新增设计/状态/调用链和证明责任。

## Readiness and Recovery

O-001 的源身份与证据已落实；T001 开始时须核对最终 Experimental 提交相对该原生基线的差异，不再要求独立关闭181。O-002/003/004/005 继续 OPEN：原生 ONNX 字节契约、tokenizer ABI、完整兼容/调用方清单、no-Python 隔离设计尚未完成。T001仍未勾选；G0不满足时不得开始T002及后续实现。

恢复以本契约所列基线和最新任务 checkpoint 为准，不重启181最终实验。发现新差异先修订受影响文件/类/方法/字段、参数意义、调用链、PO和任务；不以扩大超时、删除负例、放宽授权或只验 warm path 消除失败。旧 revision1/2 JSON 与冻结历史保持原字节。
