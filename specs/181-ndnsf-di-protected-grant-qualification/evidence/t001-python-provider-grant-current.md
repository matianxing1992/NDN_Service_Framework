# T001 — Python Provider 解包接入与 grant 发布

**Layer**: implemented（AEAD 派生/密文暂存、Provider 装配入口接线、
requester 进程内权威流水线、密钥加载）;executed（32 项 unit 测试全绿，
2026-09-05;79 项 grant 相关回归全绿）;无 measured 声明。Integration 层
（真实 NFD 链路）与 MiniNDN 由 T005 的 Y-B 保护纪元子用例覆盖（任务
规定），本文件如实声明该边界。

Date: 2026-09-05. Source HEAD: `af85b7a7` + commits `32b3e8b2`
（Provider 资格与 FR-013 AEAD）、`a6655d91`（requester 流水线）。

## 声称

`protection_epoch != "plaintext-v1"` 时，Python Provider 装配入口在授权
边界按规范名精确获取 grant Data → 权威签名/绑定/过期校验 → 解包内容
密钥 → 装配产物 AEAD 加密暂存（FR-013 真实消费）→ 明文租约注册 →
清理零化;任何 verifier 决策失败关闭为 `DI_PROTECTED_GRANT_REJECTED`。
Requester 侧：进程内权威签发后经既有 `publish_signed_app_data` 路径按
同一规范名发布。

## 代码实现（implemented）

### AEAD 派生与密文暂存 — `core/protected_artifacts.py`

- `assembled_kdf_context()` / `derive_assembled_bundle_key()` /
  `derive_assembled_entry_key()`：严格按 Spec170 契约公式
  `K_bundle = HKDF(epochContentKey, "NDNSF-DI/assembled/v1" ||
  modelManifestDigest || roleAssemblySpecDigest || storageProfileDigest)`、
  `K_entry = HKDF(K_bundle, entryKind)`。
- `AssembledCiphertextV1`：manifest（schema/entryKind/KDF/AEAD 标识/
  KDF-context digest/nonce/ciphertext length/digest，不含明文与密钥）+
  确定性 framing（8 字节长度前缀 + manifest JSON + 密文）。
- `encrypt_assembled_entry()` / `decrypt_assembled_entry()`：
  AES-256-GCM，AAD = KDF context;错误内容密钥、篡改密文、篡改 context
  字段、错误 entry kind 全部认证失败（ValueError）。

### Provider 装配入口 — `provider.py`

- `_qualify_protected_assembly()`（:1364 起）：精确名获取（既有
  `ndnsf.fetch_exact_data_packet` 原语，无 ValidatorNull——验证在应用
  层由 `verify_and_unwrap_grant` 完成）→ wire 解析 → 绑定/签名/过期
  校验 → 解包 → 内容密钥 lease 注册 → 装配产物密封 → 解密明文 lease
  注册;任一失败 `ProtectedGrantRejected` 并 zeroize 已注册租约。
- wrapped() 装配调用点（:2524 起）：保护纪元下接
  `_qualify_protected_assembly`，拒绝映射为
  `DI_PROTECTED_GRANT_REJECTED: <verifier 原因>`，在装配产物暴露给
  adapter 之前失败关闭。
- handler finally（:2700 起）：`protected_lease_registry.zeroize_all()`
  ——内容密钥副本与装配明文在 handler 边界零化删除。
- 配置：`DistributedInferenceProvider(create)(
  grant_authority_public_key=..., grant_recipient_private_key=...,
  grant_fetch_timeout_ms=...)`;未配置密钥时保护纪元赋值失败关闭。

### Requester 流水线 — `security/requester_grant_pipeline.py`（新文件）

- `build_in_process_grant_provider()`：ArtifactPolicyAuthority +
  AuthorityBackedGrantProvider + publisher 闭包（`publish_signed_app_data`
  路径）组装;grant Data 经 `canonical_grant_name` 规范名发布，Provider
  按同一名字获取。
- `load_authority_from_registry()`：`~/.config/ndnsf/spec180/` 权威私钥。

### 密钥加载 — `security/registry_keys.py`（新文件）

- `load_artifact_policy_authority_private_key()`：PKCS#8 PEM Ed25519,
  mode 0600 强制（组/他人可读即拒绝）、缺失/非 Ed25519 失败关闭。

### wire 编解码 — `core/protected_artifacts.py`

- `grant_to_wire()` / `grant_from_wire()`：canonical JSON payload +
  grantDigest/authoritySignature（非循环：digest/signature 不进
  signing_bytes）。

## 测试执行（executed — unit 层）

`tests/python/test_spec181_provider_grant.py`（32 tests）+ Spec 180 编码
回归基线（79 项合计）:

```
python3 -m pytest test_spec181_provider_grant.py -q
32 passed
python3 -m pytest test_spec180_protected_grant.py test_spec180_grant_provider.py \
  test_spec181_provider_grant.py test_spec181_y_n_e.py \
  test_spec181_runner_guard.py test_spec170_artifact_security.py \
  test_spec170_integrated_flows.py test_spec180_yolo_security.py -q
79 passed
```

覆盖：KDF 上下文契约文法/确定性/字段敏感性/entryKind 分化;AEAD
往返/随机 nonce/manifest 无密钥材料/序列化;错误内容密钥、篡改密文、
篡改 context、错误 entry kind 全部认证失败;wire 往返与畸形拒绝;
密钥加载（0600 通过、0644 拒绝、缺失拒绝、非 Ed25519 拒绝）;
`_qualify_protected_assembly` 注入 fetch 的 7 个用例（正确 grant 密封/
注册/零化;错误收件人在 verifier 内被拒;过期（重签构造）被拒;跨请求
绑定被拒;fetch 失败失败关闭;密钥未配置失败关闭;无 grant binding 失败
关闭;篡改密文 AEAD 层拒绝）;requester 流水线 4 用例（binding 往返 +
Provider 侧同调用解包一致、未知收件人/缺内容密钥/政策拒绝失败关闭且
零发布）。

## Integration 层（T005/T008 的 Y-B 保护纪元子用例）

真实 requester 进程内签发 → `publish_signed_app_data`（真实 NFD 面）
→ 真实 Provider 进程 `fetch_exact_data_packet` 获取与解包的完整链在
MiniNDN 上由 T005/T008 的 Y-B 保护纪元子用例执行（T001 任务规定
"MiniNDN 由 T005 的 Y-B 保护纪元子用例覆盖"）。届时本文件追加
executed 层记录。

## Verdict

PASS（T001 范围，unit 层 + 实现完整性）。三层中的 unit 层全绿;
integration/MiniNDN 层按任务规定由 T005 执行。
