# Spec181 设计-代码收敛审计（T007）

**Audit basis**: 12 审计原则（`.specify/memory/speckit-audit-principles.md`）。
**Audit scope**: 真实生产链——进程内权威、grant 解包双侧、装配、
runner、候选工具链。
**Date**: 2026-09-05。**Source identity**: `Experimental` 分支
`af85b7a7` 起的 spec181 提交序列（Phase 0 修正 → T001/T002/T003/T004/
T006 实现，见各证据文件）。

四层证据分离：文档声称（proposed）/ 代码实现（implemented）/ 测试
执行（executed）/ 实验测量（measured）逐层声明，禁止跨层包装。

---

## 原则 1：意图一致性

**结论：PASS。**

- spec.md 的完成目标（"一个不可变 YOLO 候选在 MiniNDN 本地小模型
  CPU 上通过 Y-A/Y-B/Y-N 全矩阵，通过收敛审计与本地资格，随后 SIF
  replay 与一次 Tiger Y-B"）与 tasks.md 的 12 任务逐一对应，无目标
  偷换。
- 边界遵守：撤销子系统不在本分支（`protected_artifacts.py:9-13`
  docstring 与 `revocation_sequence=1` 被动字段 `:400`）;独立权威
  服务为延期项（`requester_grant_pipeline.py` 明示 in-process
  functional slice）。
- FR-013 的"内容密钥必须被真实消费"是设计自审结论（plan.md
  Architecture Decision 5），T001 以 AEAD 密封落地
  （`provider.py:_qualify_protected_assembly`）。
- 拒绝的替代方案：无隐藏的合成拒绝残留——`_validate_negative_marker`
  （`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:2924-2935`）只接受
  注册原因，合成 `PROTECTION_EPOCH_REJECTED` 已删除。

## 原则 2：必要性与 Occam

**结论：PASS。**

- AEAD 派生复用既有 `HKDF`/`AESGCM` 原语（`protected_artifacts.py`
  已 import），未引入新密码学库;native 侧全部 OpenSSL EVP
  （`NativeGrantVerifier.cpp`），无第三方依赖。
- `verify_and_unwrap_grant`/`AuthorityBackedGrantProvider`/
  `canonical_grant_name` 全部复用 Spec 180 资产（提交 `d36438c2`），
  未重造。
- `NativeGrantVerifier` 编译进 `_ndnsf` 扩展（`pythonWrapper/setup.py`）
  而非重复实现于 Core 库——DI 层组件归属正确。
- 变异构造器（`grant_mutations.py`）同时服务 runner 聚焦 probe、
  user.py binding seam 与 T003 向量生成，单一实现三处消费。

## 原则 3：架构与归属

**结论：PASS。**

- 通用编码/密码学在 `core/protected_artifacts.py`（Core 层）;操作者
  密钥加载在 `security/registry_keys.py`;requester 组合逻辑在
  `security/requester_grant_pipeline.py`;Provider 装配接线在
  `provider.py`（APP 层）;native 验证在 `cpp/ndnsf-di/`
  （DI 层）。无 YOLO 特判进入 Core——`_PROTECTED_ASSEMBLY_STORAGE_PROFILE`
  是通用 workdir profile，非 workload 特判。
- runner（`Experiments/`）不承载密码学逻辑，只调用
  `security.grant_mutations`。
- 错误码家族统一：Python 侧 `ProtectedGrantRejected` →
  `DI_PROTECTED_GRANT_REJECTED`;native 侧同名字符串家族
  （`NativeGrantVerifier.cpp` verifyAndUnwrapNativeGrant 各拒绝分支）。

## 原则 4：跨文档一致性

**结论：PASS（随 T005 结果更新 executed 层）。**

- spec.md FR-013 的派生公式
  （`K_bundle = HKDF(epochContentKey, "NDNSF-DI/assembled/v1" ||
  modelManifestDigest || roleAssemblySpecDigest ||
  storageProfileDigest)`）与实现
  `protected_artifacts.py:assembled_kdf_context()` 逐字一致;契约
  `specs/170/contracts/artifact-assembly-v1.md:204-212` 同源。
- 信封 alg 字段三方一致：Python `RecipientEnvelopeV1.__post_init__`
  （`protected_artifacts.py:92-100`）、native
  `verifyAndUnwrapNativeGrant` 的 X25519/P-256 分支、Spec170 契约。
- 绑定上下文 canonical JSON 双侧一致（T003 向量锁定证明——
  `tests/fixtures/spec181/grant-vectors-v1.json` 9 case 双侧逐字节
  一致）。
- traceability.md 的 FR→task→evidence 映射已随实现更新（revision 4
  + Source-owner status）。

## 原则 5：代码事实核查

**结论：PASS。**（CodeGraph 证据 + 编译/测试验证）

- `verify_and_unwrap_grant`（`core/protected_artifacts.py:444`）：
  调用方 = provider.py `_qualify_protected_assembly`、T003 双侧测试、
  Y-N-E 变异测试;全部真实调用。
- `AuthorityBackedGrantProvider.__call__`
  （`security/grant_provider.py:101-155`）：签发 → 校验 → binding;
  `requester_grant_pipeline.build_in_process_grant_provider` 组装真实
  发布路径。
- `_qualify_protected_assembly`（`provider.py:1364`）在 wrapped()
  装配调用点（`provider.py:2524`）真实接线;handler finally 零化
  （`provider.py:2700`）。
- `verifyAndUnwrapNativeGrant`（`NativeGrantVerifier.cpp`）：
  C++ unit tests 消费固定向量（`distributed-inference-native-grant-verifier.t.cpp`）;
  pybind 面 `verify_and_unwrap_native_grant`（`_ndnsf.cpp`）由
  T003/Y-N-E 测试真实调用。
- 编译事实：waf build-system-j2 全绿（2 次构建）;`_ndnsf` 经
  `scripts/spec180_native_build.py build` 重建
  （`SPEC180_NATIVE_IDENTITY_OK`）。
- 测试执行事实：unit 层 32+28+7+6+9+3 用例全绿;spec181/runner
  合计 143 passed;全量 Python 回归 2682 passed（1 个预存基线漂移
  `test_authorization_evaluation` 与本 spec 无关，已记录）。

## 原则 6：安全与分布式正确性

**结论：PASS（本地;网络层由 T005 矩阵执行验证）。**

- 认证链：requester 签名（`GrantRequestV1.sign`）→ 权威策略校验
  （`ArtifactPolicyAuthority.issue`）→ 权威签名（Ed25519）→ Provider
  双侧验证（签名 + 非循环 digest + 过期 + 绑定 + AEAD）。每层失败
  关闭。
- 重放/新鲜度：grant 过期强制（`verify`/`verifyAndUnwrapNativeGrant`
  的 expiry 分支）;`(K_entry, nonce)` 一次性（`os.urandom(12)`
  每次加密）。
- AAD 绑定：信封 AAD = canonical binding context——跨请求/attempt/
  core/model/纪元复用信封在 AEAD 认证层失败（Spec 180 设计 +
  T003 向量 6 个负例证明）。
- 密钥材料：权威私钥 mode 0600 强制（`registry_keys.py:
  _check_private_mode`）;manifest 不含明文密钥
  （`test_manifest_exposes_no_plaintext_key_material`）;租约零化
  （`PlaintextLeaseRegistry.zeroize_all` 在 handler finally +
  失败路径）。
- fail-closed：保护纪元下无 grant binding / fetch 失败 / 密钥未配置
  / AEAD 失败 → `DI_PROTECTED_GRANT_REJECTED`（全部负例测试覆盖）。
- native `ProtectedRuntime` 状态机：verifyGrant 失败关闭（R002）、
  cancel 从 FailedClosed 排空（修正后 6 C++ 用例）。

## 原则 7：任务可执行性

**结论：PASS。**

- 12 任务（R001-R004 + T001-T012 中本切片落地 8 项）每个任务绑定
  具体文件、行为结果、三层验收;无机械拆分（tasks.md 内聚规则）。
- 每任务证据文件头部声明证据层;不存在"写测试/实现/跑测试"的伪
  拆分。

## 原则 8：验证设计

**结论：PASS（本地执行部分;MiniNDN 矩阵 6/7 通过，Y-N-I 修复重跑中）。**

- 纯函数/编码层 → unit（无 NFD、无跨进程）;真实生产链 →
  integration（进程内权威真实签发、双侧 verifier 真实调用）;
  网络功能 → MiniNDN（T005 的 Y-N 七子用例 + T008 的 Y-A/Y-B/Y-N）。
- 小模型 CPU 为资格对象（plan.md AD5）;GPU 只在 S5 Tiger。
- parity 固定向量（T003）使双侧一致性可复现、非机会性。

## 原则 9：证据完整性

**结论：PASS（现有证据）;T005/T008 的 measured 层待矩阵完成。**

- 每个证据文件头部四层声明（r001-r004、t001-t004、t006 均遵守）。
- T001/T002 证据明确声明 integration/MiniNDN 由 T005 覆盖，不把
  unit 冒充 integration（诚实声明边界）。
- Spec 180 的 29 个编码测试作为回归基线（不冒充本 spec 资格）。

## 原则 10：冻结证据保护

**结论：PASS。**

- Spec 180 全树冻结（spec.md 输入声明 + r003 审计确认 105 个
  证据文件带失效横幅）;本 spec 不修改 Spec 180 文件。
- 本 spec 证据新目录（`specs/181/evidence/`），引用式继承不复制。
- 预存失败（`test_authorization_evaluation` 基线漂移）如实记录、
  不静默跳过（deselect 运行已记录）。

## 原则 11：迁移与回滚

**结论：PASS。**

- plaintext-v1 路径零改动：`_qualify_protected_assembly` 仅在
  `protection_epoch != "plaintext-v1"` 时进入;所有 Spec 175/180
  plaintext 回归保持绿色（2682 passed）。
- 撤销/独立权威/纪元轮换三项延期在 spec.md Out of Scope 有明确
  集成条件与删除标准;`revocation_sequence=1` 线编码不变。
- `GRANT_WIRING_AVAILABLE = False`（R004 门禁）在 Y-B 保护纪元
  子用例接线前保持拒绝（翻转时机 = T005 Y-B 落地吸收 commit）。

## 原则 12：结论门禁

**当前裁决：CONDITIONAL PASS**——条件 = T005 MiniNDN 矩阵七子用例
全部注册结果通过（6/7，Y-N-I 修复重跑中）+ Y-B 保护纪元 grant 往返
（重跑中）+ T008 本地资格（Y-A/Y-B/Y-N）完成。
BLOCK 项：无。HIGH：无。

审计发现列表（每项附 file:line 与关闭回归）：
- （已关闭，T006 阶段）runner 矩阵 UNAVAILABLE 路径残留 → 删除 +
  143 项回归。
- （已关闭，R002 阶段）`cancel()` FailedClosed 早退导致无法干净
  排空 → 修正 + 6 C++ 用例。
- （无发现）其余原则未发现需修复项。
