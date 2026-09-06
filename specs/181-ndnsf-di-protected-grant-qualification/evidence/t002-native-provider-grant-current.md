# T002 — native Provider 解包

> **Current scope correction (revision 6, 2026-09-05)**: T002 完整任务验收 PASS，见 [acceptance map](t002-acceptance-20260905.md)。真实 native Ed25519/P-256 正负链、进程集成、EC 泄漏修复、worker 生命周期、公共准备与 adapter/handler 接线均有检查；追加 generation worker 修复后 48 cases / 366 assertions PASS。T007 其余审计及正式资格仍 BLOCK；新源在下次 native live 前须刷新统一 manifest。下方仅保留历史 verifier 范围记录。
> 当前裁决与下一步以 [audit.md](../audit.md) 为准，以下保留为原始范围记录。

**Layer**: implemented（NativeGrantVerifier + pybind 最小面 + C++ unit
tests）;executed（C++ parity 测试 3 用例全绿、Python parity 测试全绿，
2026-09-05）;无 measured 声明。

Date: 2026-09-05. Source HEAD: commit `7ef314f1`
（T002 native verifier + pybind）。

## 声称

native Provider 侧具备与 Python 完全字节兼容的 grant 验证与解包：
规范 JSON 解析、canonical signing-bytes 重构、Ed25519 权威签名验证、
全部绑定字段与过期校验、收件人信封解包（Ed25519 seed → X25519、
EC P-256 → ECDH-P256，与 Python 信封 `alg` 一致）、HKDF-SHA256 +
AES-256-GCM。绑定失败维持 `DI_PROTECTED_RUNTIME_BINDING_MISMATCH`
语义;其余 verifier 决策注册 `DI_PROTECTED_GRANT_REJECTED` 家族。

## 代码实现（implemented）

### `cpp/ndnsf-di/NativeGrantVerifier.{hpp,cpp}`（新文件）

- `verifyAndUnwrapNativeGrant()`：绑定比较（先，保持既有
  binding-mismatch 语义）→ grant digest 一致性 → 过期 →
  Ed25519 签名验证 → 信封类型/收件人密钥匹配 → ECDH（X25519
  或 P-256）→ HKDF(info = "NDNSF-DI/key-grant/v1" || canonical
  binding context) → AES-256-GCM 认证解密。全部 OpenSSL EVP
  （无第三方库）。
- canonical JSON 重构（:29-121）：`jsonEscape`/`bindingContextJson`/
  `ParsedGrant::signingBytes` 与 Python
  `json.dumps(sort_keys=True, separators=(",", ":"), ensure_ascii=False)`
  字节一致——这是 T003 parity 锁定的前提。
- Ed25519 → X25519 私钥转换（SHA512(seed)[:32]，与 Python 一致）。

### `pythonWrapper/src/ndnsf/_ndnsf.cpp`

- `m.def("verify_and_unwrap_native_grant", ...)`：pybind 最小面
  （wire JSON + authority 公钥 raw hex + recipient seed hex + 绑定
  参数 + now_ms → {verified, reason, content_key}）。
- `pythonWrapper/setup.py`：NativeGrantVerifier.cpp 直接编入 _ndnsf
  扩展（DI 层组件，不属于 Core 库）。

### 与 ProtectedRuntime 的衔接

`ProtectedRuntime::verifyGrant()` 的诚实化失败关闭（R002）由
`NativeGrantVerifier` 提供真实验证后端;handler 的
`DI_PROTECTED_GRANT_UNAVAILABLE` 拒绝路径（`NativeProviderHandler.cpp:1890`）
在 factory 返回已验证 runtime 后自然消失。工厂接线（真实 fetch +
verifyGrant 安装进 `protectedRuntimeFactory`）随 T005 的 Y-B 保护纪元
子用例在 MiniNDN 上落地（该子用例是任务规定的 integration 载体）。

## 测试执行（executed）

### C++ 层（parity 向量消费）

`tests/unit-tests/distributed-inference-native-grant-verifier.t.cpp`
（3 用例）：

```
./build-system-j2/unit-tests --run_test='*GrantVerifier*' --log_level=message
Running 3 test cases...
*** No errors detected
```

`NativeGrantVerifierMatchesPythonParityVectors` 逐用例消费
`tests/fixtures/spec181/grant-vectors-v1.json` 的 9 个向量：正例内容
密钥与 Python 期望逐字节一致;wrong-recipient/cross-request/
cross-attempt/cross-plan-core/cross-model/cross-epoch/expired/
forged-authority 全部在 native 侧拒绝且原因属于注册家族。

### Python 层（双侧 parity）

`tests/python/test_spec181_native_grant_parity.py`（3 组）:

```
python3 -m pytest tests/python/test_spec181_native_grant_parity.py -q
3 passed
```

同一向量文件：Python verifier 与 native verifier 对全部 9 个用例
逐项一致（ok/contentKey 完全相同）。

## 吸收关系

R002 的失败关闭路径由本任务提供的真实验证后端吸收（诚实门的内容
已就位，factory 接线随 T005 Y-B 落地）。

## Verdict

PASS（T002 范围）。native 验证/解包与 Python 字节兼容并由固定向量
双侧锁定;绑定语义与错误码家族符合任务要求。
