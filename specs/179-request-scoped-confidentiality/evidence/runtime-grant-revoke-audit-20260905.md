# Spec179 运行时服务权限授予/回收（grant/revoke）审计报告

Date: 2026-09-05 (CDT).  Request: 审计当前设计与代码，判断运行时服务权限的
授予与回收是否**安全且有效**。Mode: **report only**（用户未要求自动修复）。

Scope: Controller 侧 `grant()`/`revoke()`，运行时（ServiceUser/ServiceProvider）
的 PolicyStatus install、grant-only DKEY-only refresh、permission 通道、
PolicyRefreshCoordinator、RuntimeStatusStore restore（FR-039）与配置信任锚
验证（FR-040）交叉点。Audit principles per
`.specify/memory/speckit-audit-principles.md`（12 原则）。证据按
文档声称 / 代码实现 / 测试执行 / 实验测量四层分离；无 BLOCK 级发现。

---

## Verdict: CONDITIONAL PASS

运行时 grant/revoke 的**安全强制方向正确且被充分验证**：所有 protected
transition 六 cut-point 走 `RevocationState::authorize`（fail-closed），
身份/证书/服务授权撤销触发全局 ABE generation 轮换 + 全族缓存清除（不依赖
grant-only 机制），status 通道由 Controller 签名 + 配置信任锚验证，restore
whole-load fail-closed。两个非安全方向发现需要记录与修复（详见下）：

| # | 层 | 严重度 | 一句话 |
|---|---|---|---|
| A | 代码实现 | MEDIUM（功能/规范，非安全） | grant-only target-only DKEY refresh 的 pending 消耗与 refresh 消费守卫不对称：status 先于 permission 安装时（倒序），equal-version install 消耗 pending 却不刷新，该次 grant 的 DKEY 刷新静默丢失 |
| R1 | 代码实现 | MEDIUM（记录于 2026-09-03/04 审计） | `revoke()` 中 ABE rotation 失败 → 内存 revocation + 已持久化 version 前进、返回 false、无 crypto 轮换/重试/对账 —— 混合状态未记录/未测试 |
| B | 代码实现/文档 | LOW（可用性/运维） | grant 的运行时发现依赖一次性 permission fetch 及其有限重试窗口；长期运行进程在窗口外 grant 需 App 层显式重调才生效；SC-022 "explicit fetch as fallback" 归属可被误读为 runtime 自动 |

修复建议（供后续任务化，本次不改代码）：
- A: 将 refresh 消费与 pending 消耗置于同一守卫（equal-version accepted 且
  pending 命中时也执行 DKEY-only refresh），或把 `invalidate` 拆为
  versionChanged 触发的缓存失效与 grantOnly 触发的 DKEY-only refresh 两段；
  User/Provider 双侧同改（ServiceUser.cpp:3905-3911 ↔ ServiceProvider.cpp:11972-11976）；
  补"倒序"集成用例。
- R1: 失败记录（typed state）→ 启动/下次 revoke 时 reconcile 重试；补注入用例。
- B: spec.md SC-022 明确 fallback 归属（runtime 不自动重取 permission；
  App 层通过 `fetchPermissionsFromController` 承担），或提供
  scheduled permission refresh 旋钮。

---

## 1. 文档声称（spec/contracts）

- **Grant-only 语义（normative）** spec.md:130-166：grant-only 添加
  `/PERMISSION/<service>` 或 `/SERVICE/<service>`（spec.md:13），不旋转 ABE
  master/public params（spec.md:203；FR-038 spec.md:558）；替换 target
  identity 的完整 monolithic policy（spec.md:133）；"A grant-only refresh
  MUST advance a DKEY-only refresh fence"（spec.md:147-148），旧 DKEY 仅
  保持其已持有的 attribute 范围（spec.md:26、159）。
- **SC-022** spec.md:598："For a grant-only change, only the granted
  identity's policy changes and one target-only DKEY fetch (normally
  initiated after signed status installation, **with explicit fetch as
  fallback**)"。
- **Refresh 权威** FR-019 spec.md:539：ControllerVersion 变化时原子安装
  新 authority + refresh-coordinator；public-param name/digest 变化时清
  缓存。FR-021 spec.md:541：即使无 peer 通告新版本，status 过期前也必须
  scheduled refresh。
- **撤回语义** spec.md:29、604-607：identity/certificate/attribute 撤回
  → 全局 generation 轮换 + filtered DKEY 重发；reauthorization 使用当前
  post-revocation generation（spec.md:190-215）。
- **FR-039/040**（traceability.md:28-30）：restore 仅未过期/未取代的
  status、DKEY 永不持久化、live install 在配置信任锚下验证。

**文档层与代码的矛盾点**：SC-022 "normally initiated after signed status
installation" 未限定 permission 通道先行 —— 实现仅在 permission records
先于 version-change install 到达时才满足该规范（见 Finding A）。

## 2. 代码实现（CodeGraph + 直接核读，file:line）

### 2.1 Controller 授予/回收

- `ServiceController::grant()`（ServiceController.cpp:522）：grant-only
  （无 rotation）→ 完整替换 target policy + 保留 ABE pair；reauthorize-
  after-IDENTITY-revocation 允许、CERTIFICATE revocation 不可绕过；
  已有 service grant + attribute 且无 matching withdrawal → no-op 短路。
- `ServiceController::advanceAuthorizationEpoch()`（ServiceController.cpp:464-474）：
  durable epoch 前进（ControllerGenerationStore）。
- `ServiceController::onPolicyStatusInterest()`（ServiceController.cpp:1578-1622）：
  POLICY-STATUS 为**纯 Interest 驱动应答**（`m_face.put`，FreshnessPeriod=0），
  无主动 publish；应答携带当前 status wire（含 revocations，Controller.cpp:1616）。
- **R1** `ServiceController::revoke()` 调用
  `rotateAbeGenerationAndReissuePolicies()`（ServiceController.cpp:413,
  调用点 :502）；rotate 失败路径（501-511）保留内存 revocation + 已推进
  version、返回 false —— crypto generation 隔离跳过，无重试/对账。

### 2.2 运行时 install 与 DKEY refresh 链（User；Provider 同构镜像）

- `installControllerStatus`（ServiceUser.cpp:3793-3915）：staged 双状态机
  （`RevocationState::acceptStatus` + `PolicyRefreshCoordinator::
  installCurrentStatus`，失败双回滚）→ commit（3842-3844）→
  `abeGenerationChanged` 判定（3865-3878：consumer 参数 identity 比较，
  cold-bootstrap 特例）→ **pending 命中块**（3879-3903：wave 去重 3889-3895、
  `grantOnlyDkeyRefresh` 3896-3898、**erase pending 3899-3902**）→
  **`invalidate` 仅在 versionChanged**（3906-3911）→ 无条件
  `scheduleControllerStatusRefresh`（3912）。
- `RevocationState::acceptStatus`（RevocationState.cpp:58-79）：
  older→false（:69-70）；**equal-version → `sameWire` 幂等 accepted**
  （:71-72）；新 status → `invalidateAllFamilies`（173-181：abe/message-key/
  targeted-token/selection-binding/nonce/replay 六族）。
- `invalidateControllerScopedCaches`（ServiceUser.cpp:4094-4285）：
  pending unary 在途请求的 request-scoped secrets 无条件清除（4111-4172）、
  hybrid per-service invalidate（4174-4175）、targeted pools/offers 擦除
  （4233-4269）、NAC-ABE `clearCache` 仅 `abeGenerationChanged`（4183-4191）；
  **DKEY refresh（`refreshDecryptionKey`/`obtainDecryptionKey`）只在
  `abeGenerationChanged || grantOnlyDkeyRefresh` 时发生**（4201-4211），
  否则 NOT_REQUIRED（4219-4224）。
- **pending 的唯一插入点**：`applyPermissionResponse`（ServiceUser.cpp:3684）
  records 差异块（3751-3753；Provider 11788-11790），入口
  `onPermissionResponseData`（9403-9428）。
- **permission 通道无生产自动驱动**：`fetchPermissionsFromController`
  （ServiceUser.cpp:3654）的生产调用者为零（CodeGraph callers = 仅 6 个
  集成测试用例）；examples/App_User.cpp:816 仅启动时调用一次；
  `onPermissionResponseTimeout`（9442）有限重试（`permissionFetchMaxAttempts`）
  耗尽即停。
- **status 通道独立可达**：`scheduleControllerStatusRefresh`（4287-4386）
  fire handler（4326-4385）→ `fetchPolicyStatusFromController`（9582）
  → `onPolicyStatusData`（9629-9753，install :9735、persist :9744）。
  Provider 镜像同构（ServiceProvider.cpp:11722/11854/11946-11976/12167）。

### 2.3 Finding A（新）—— pending 消耗与 refresh 消费守卫不对称

倒序时序（status v+1 先于 permission 响应 install —— 由独立 status 通道
即 scheduled refresh / restore 确认 / expiry-lead fire 触发）：

1. install v+1（grant-only、same generation）：pending 未命中 → 3881 条件
   整块跳过（不设 wave、不 erase）→ invalidate 的 4201 分支 false →
   NOT_REQUIRED（4219-4224），**无刷新**。
2. permission 响应到达 → pending 插入（3751-3753）→ tail services-fetch
   （applyPermissionResponse 尾部）→ fetch status 得**同 v+1** →
   `onPolicyStatusData` → **equal-version install**：
   `acceptStatus` equal+sameWire → accepted（RevocationState.cpp:71-72）；
   `versionChanged=false` → 3906 **不 invalidate**；但 3879-3903 pending 块
   执行：refreshForCurrentWave=true（wave 空）→ `grantOnlyDkeyRefresh=true`
   （3896-3898）→ **wave 置为 v+1（3894）→ pending erase（3902）**。
3. 结果：grantOnlyDkeyRefresh 计算后被丢弃（唯一消费者是 versionChanged
   门内的 invalidate，3906-3911）→ **该次 grant 的 DKEY-only refresh 静默
   丢失**，直到下一次真实 version 前进（wave 已变 → 恢复）。被 grant
   attribute 覆盖的内容在丢失窗口内解密失败（fail-closed 方向，非越权；
   revocation 不依赖此路径 —— 撤回落 abeGenerationChanged / isRevoked
   独立机制）。

**严重度判定依据**：无安全越权面（刷新丢失只减少能力，不增加）；
违反 spec.md SC-022（:598）"normally initiated after signed status
installation" 的实现承诺；修复面小且局部（User/Provider 对称两处）。

### 2.4 Finding B（新，设计层）—— grant 运行时发现的时效窗口

grant 后 runtime 侧能感知的通道：(a) status scheduled refresh（默认
expiry-lead ≈ 24h-1/4；knob 显式开启才短周期）→ 只知 version 前进、
无权限数据；(b) App 显式重调 `fetchPermissionsFromController`（公开 API，
生产无自动调度）→ records diff → pending。两者交错时落入 Finding A。
MiniNDN grant-only-advance 场景之所以成功：App 启动时发出的 permission
Interest 重试窗口（seconds×attempts）恰好覆盖 8s 处的 grant
（tests/minindn/run_request_scoped_confidentiality.py:764 `grantAfterMs: 8000`，
注释 :768-770 "grant is discovered lazily by the DKEY/first-use path, so no
scheduled refresh is injected"）。窗口耗尽后到达的 grant 依赖 App 层重调/
重启。影响 = 授予生效延迟（可用性/运维），revocation 不受影响。

## 3. 测试执行（代码路径 ↔ 覆盖交叉）

| 路径 | 覆盖用例 | 结果 |
|---|---|---|
| grant-only Controller 侧（ABE pair 不变 + target policy 替换 + decrypt 矩阵） | `ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy`（controller-revocation-flow.t.cpp:744）RV-U20 | green |
| grant-only runtime 正序（permission 先行 → version-change install → exactly one target DKEY fetch；重复 cycle 幂等） | `GrantOnlyRefreshIssuesOneTargetFetchWithZeroFanOut`（controller-revocation-flow.t.cpp:3096）RV-U21 | green |
| 网络层 grant-only（唯一一次 epoch≥2 target fetch、unaffected 零 fetch、unaffected epoch-2 通道存活、granted 行全成功、pre-grant 行全拒绝） | MiniNDN `grant-only-advance`（driver :751-771，gate :1251-1265） | green（`grantedEpochGE2Fetches==1`） |
| identity/certificate/service 撤回 + 全 cut-point | `RealControllerStatusRevokesEveryTargetKindAtEveryCutPoint` + MiniNDN 撤回家族 | green（RV-I01-RV-I14 等） |
| FR-039 restore（fail-closed、atomic、epoch 收敛）/ FR-040 trust anchor | `RuntimeStatusStorePersistence` 6/6（RV-U23）、`PersistedRuntimeStatusSurvivesRuntimeRestart`（RV-I32）、`HierarchicalConfiguredTrustAnchorControlsControllerStatusValidation`（RV-I33） | green |
| **倒序竞态（status 先 install、permission 后到 equal-version 吞 pending）** | 无（3096 的 "repeated cycle" 第二次 cycle records 无 diff → pending 不插入，不触及该分支） | **未覆盖** |
| **revoke() rotate 失败注入** | 无（测试无 rotate/exception 注入；rg 命中均为正常错误处理行） | **未覆盖** |
| grant 在 permission 重试窗口耗尽后到达 | MiniNDN `grantAfterMs=8000` < 启动 fetch 重试窗口 | **未建模** |

全门禁回归（build-clang-spec179-rv32，2026-09-04/05）：spec179 gate suites
全绿；仅记录在案的 pre-existing DI codec SIGFPE 与 schedule-dependent
`Spec170*` 集成失败（out of scope，`evidence/regression-red-green-20260904.md`）。

## 4. 实验测量

- 无新测量。既有冻结证据：MiniNDN 14/14 campaign（2026-09-04，
  `evidence/minindn-campaign-20260904.md`）。grant-only-advance 行
  `refreshAttempts 41` 为 PENDING/REQUESTED/NOT_REQUIRED/
  CACHE_INVALIDATED 混合日志行计数（harness :404-410/530），**权威信号为
  `grantedEpochGE2Fetches==1`**（gate :1258），即"恰一次 target-only
  DKEY refresh"在真实网络中被观察到 —— 正序路径的端到端证据，不覆盖
  Finding A/B 时序。
- FR-039 持久化与 FR-040 信任锚的测量 = RV-U23/I32/I33 执行记录
  （unit/component 层，非网络测量）。

## 5. 修复与再验证建议（任务化，非本次执行）

1. **A**：User/Provider install 中把 DKEY-only refresh 从 versionChanged
   gate 移出（equal-version accepted && pending 命中 → 仍 refresh），或
   invalidate 拆分；新增倒序集成用例（先 status 通道 install v+1、后
   applyPermissionResponse → 断言恰一次 refreshDecryptionKey）。
2. **R1**：rotate 失败 typed 记录 + 下次入口 reconcile 重试 + 文档声称
   修正（spec.md FR 域注明 rotate-failure 语义）；失败注入用例。
3. **B**：spec.md SC-022 fallback 归属文字化（App 层 API 为 fallback，
   runtime 不自动重取 permission）；或增加 scheduled permission refresh
   旋钮（默认关，与 revocation 的 scheduled status refresh 分离）。

---

## Closure addendum（2026-09-05，修复已应用 + 门禁重跑；supersedes 上方 report-only 记录）

上方 CONDITIONAL PASS 三发现（A/R1/B）已修复并经全门禁重验。本加章按
文档声称 / 代码实现 / 测试执行 / 实验测量四层记录。冻结的 2026-09-04
MiniNDN campaign（`results/spec179-minindn/`）未被触碰；本次网络测量写入
新目录 `results/spec179-minindn-fixclosure-20260905/`。

### 1. 文档声称（spec.md Amendment，2026-09-05）

- grant-only normative 第 2 条重写 + 新增 "Grant discovery is App-driven"
  归属段：runtime 永不轮询 permission records；一次已应用的 App 层
  permission renewal（或显式 refetch）为 target-only refresh 上膛；
  "armed once ... explicit refetch is the trigger and fallback" 取代
  "normally initiated after signed status installation ... explicit fetch as
  fallback"（SC-022 spec.md:598 与 FR-038 变体同文）。
- SC-022 追加 arrival-order 契约："The single target-only refresh MUST occur
  whether the signed status install preceded or followed the permission
  renewal that armed it — no arrival order may silently consume the pending
  refresh without issuing it."
- FR-017 追加 R1 契约：crypto rotation 无法完成时 Controller 仍须在后续
  每个已发布 status 中强制该 revocation（fail-closed），将 rotation 记为
  pending，并在接受任何后续 revocation 前重试。

### 2. 代码实现（修复后 file:line）

- **A**（User/Provider 对称）：`installControllerStatus` accepted 分支现为
  `versionChanged → invalidateControllerScopedCaches(...)`；
  `else if (grantOnlyDkeyRefresh)`（equal-version 迟到 pending 命中）→ 直接
  调 `refreshNacDkeyForControllerStatus`（ServiceUser.cpp:3922 /
  ServiceProvider.cpp:11988）。原 invalidate 内 DKEY 块抽为同一 helper
  （定义 ServiceUser.cpp:4276 / ServiceProvider.cpp:12352；invalidate 内调用
  ServiceUser.cpp:4211 / ServiceProvider.cpp:12214），保留 wave 去重
  （m_lastDkeyRefreshWave）、LocalMock defer 与 NOT_REQUIRED 语义
  （hpp 声明 ServiceUser.hpp:1117 / ServiceProvider.hpp:1127）。
- **R1**：`rotateAbeGenerationAndReissuePolicies` 现带幂等 fence ——
  `m_aa.getPublicParametersVersion() != generation*1000+epoch` 时才
  `rotateKeyGeneration`（避免 reconcile 重试二次滚动 master key）；
  :415 test-only env 注入 `NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE`。
  `revoke()` 旋转失败 catch 块置 `m_abeRotationPending=true` 后返回 false
  （内存 revocation + durable epoch 已生效，fail-closed，ServiceController.cpp:547-549）；
  下次 revoke 入口先 `reconcilePendingAbeRotation()`（:525），重试成功才
  接受新 target（实现 :442-453；hpp 声明与成员 :225，含重启后由 durable
  epoch 权威重派生 ABE identity 的注释）。

### 3. 测试执行（2026-09-05，build-clang-spec179-rv32，327/327 编译 30m46s）

| 层 | 用例 | 结果 |
|---|---|---|
| RV-I34 倒序 | `GrantOnlyRefreshSurvivesReverseOrderStatusFirstInstall`（controller-revocation-flow.t.cpp:3254）：先 `installControllerStatus(v2)`（status 通道先行，pending 空 → 0 fetch、wave 未设）；再 `fetchPermissionsFromController`（grant 发现）→ granted 侧 DKEY fetch 恰 1 次、unaffected provider 0 次；重复 fetch 幂等仍 1 | green（机制分析而非单独执行负向：修复前 equal-version install 吞 pending 不刷新 → 该序列断言将得 0 fetch） |
| RV-U24 注入 | `ControllerRevokeRotationFailureRecordsPendingAndReconciles`（t.cpp:3417，Controller-only）：env 注入 → 首 revoke 返回 false、params name/digest 不变、revocations==1（fail-closed 保留）；env 清除后第二次 revoke → reconcile 成功、params advance、revocations==2 | green |
| 全套 gate suites | integration `ControllerRevocationFlow` **42/42**（40/40 + 上述 2 例）、`ControllerVersionRefresh`、`RequestScopedSelection`、`RequestScopedResponseConfidentiality`、`Spec175InvocationStream`；unit `RequestScopedConfidentiality`、`ControllerRevocationPolicy`、`ControllerRevocationState`、`GenericDynamicApi`、`RuntimeStatusStorePersistence` | 全部 "No errors detected" |

### 4. 实验测量（MiniNDN fix-closure runs，2026-09-05）

固定二进制 build-clang-spec179-rv32（含 A/R1 修复）三场景真实 MiniNDN
执行，输出 `results/spec179-minindn-fixclosure-20260905/<scenario>/result.json`
（冻结的 2026-09-04 campaign 目录未触碰）：

| 场景 | 权威信号（result.json） | 结果 |
|---|---|---|
| `grant-only-advance`（正序 grant-only 无回归） | `grantedEpochGE2Fetches==1`（恰一次 target-only DKEY fetch）、`unaffectedEpochGE2Fetches==0`（unaffected 零 fetch）、`grantAbeUnchanged=true`、granted 14/14 行成功、`grantOnlyGateOk=true`、`networkEvidence=true`、executionCount 23 | gatePassed=true |
| `service-scoped-revocation-with-unaffected-control`（service 撤回复归） | `revocationApplied=true`、11/11 类不受影响/双角色控制链保留、`networkEvidence=true`、executionCount 41 | gatePassed=true |
| `user-identity-revocation`（identity 撤回复归） | `revocationApplied=true`、`networkEvidence=true`、executionCount 23 | gatePassed=true |

结论：A 修复在倒序路径上新增 equal-version 刷新（RV-I34 集成层确定性验证），
正序路径网络测量与冻结基线一致（grantedEpochGE2Fetches==1）；R1 修复仅在
rotate 失败注入路径生效（RV-U24），正常 revoke 的 MiniNDN 撤回家族无回归。

---

## 最终结论（closure verdict）

**PASS。** 原 CONDITIONAL PASS 的 A/R1/B 三发现均已修复：
- A — equal-version+pending install 现在直接签发被挂起的 grant-only
  DKEY-only refresh（User/Provider 对称 helper），SC-022 的
  arrival-order 契约由 RV-I34 集成用例确定性验证；
- R1 — rotate 失败 fail-closed 记录 + `reconcilePendingAbeRotation()`
  下次入口重试（幂等 fence 防二次滚动），RV-U24 注入用例验证；
- B — spec.md SC-022/FR-038/grant-only 归属文字化（App-driven）。

门禁重跑全绿：integration `ControllerRevocationFlow` 42/42（40/40 基线 +
RV-I34/RV-U24），全部 spec179 unit/integration gate suites "No errors
detected"；MiniNDN fix-closure 三场景 gatePassed=true（独立结果目录）。
spec.md/validation-matrix/traceability/AUDIT/docs-architecture 在同一
commit 内同步。残余项仅为 T014 upstream NAC-ABE package/push（maintainer
授权动作，非代码门禁）。
