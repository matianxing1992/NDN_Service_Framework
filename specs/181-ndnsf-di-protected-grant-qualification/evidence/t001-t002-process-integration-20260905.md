# T001/T002 Maintained Process Integration

**Status**: PASS (focused process integration and P-256 resource repair)
**Evidence layer**: implemented / wired / executed (focused process integration and ASAN)

## Scope

新增维护测试使用独立 requester、Provider 进程与真实 NFD。
requester 经正式 signed APP Data 接口发布；Python/native Provider
使用生产 grant 获取、verifier 和受管密钥。Selection metadata 为
fixture 输入，不声称替代 Core Selection 的既有完整链路验证。
Python 分支还覆盖生产装配、密文读取及资源清理。

## First Boundary

R1 两个 native 测试目标构建 PASS（42.661s）。首个真实进程用例
在 NFD 自身启动失败：最小配置缺少管理授权规则，内部 `/localhost/nfd`
FIB 注册报 validator 未回调；requester 随后连接被拒绝。该结果
不是 grant 验证失败。原始 `build.log`、`integration.log`、各进程
日志和退出记录保留在 ignored workspace temporary directory 下
`spec181-grant-integration-20260905-r1/`。修复测试配置后再执行。

R2 NFD 管理就绪后，requester 构造在获取 NAC 公共参数时失败；
fixture 需增加正式 Controller 进程，不能绕过生产初始化。另一个
独立 ASAN 诊断使用显式 wire fixture（不是网络证据）：10 次真实
P-256 verifier/runtime 调用返回 VERIFIED/Zeroized，但退出时报告
24560 bytes / 630 allocations 泄漏。两类结果分别保留在 r2 的
`integration.log` 与 `resources/native-p256-resources/asan.log`。

R3 native 定向构建 PASS（11.566s）；改用 RAII 释放 P-256 EC 引用、
point 与派生 context 后，同一 10 次调用的 ASAN 资源检查 PASS（0.32s），
不再报告泄漏。真实进程用例已通过 Controller 就绪，但 requester
发布就绪等待超时（17.96s），尚未产生 Provider grant 结果。原始证据
保留在 `spec181-grant-integration-20260905-r3/`，下一轮增加实际
requester 堆栈与 NDN 日志定位，不能把发布超时作为密码学拒绝。

R4 定向 NDN 日志与 Python 限时堆栈证明等待发生在正式
`ServiceUser` 构造：反复 `Waiting for decryption key`。测试 policy
仍是 hello 示例，没有 requester 身份与正式 bootstrap token；
Controller 公共参数就绪不能替代 requester NAC 凭据就绪。
保留 r4，再补齐该 fixture 的实际策略及证书 bootstrap。

R5 实际策略与 token bootstrap 修复后，前 5 项通过；第 6 项 Python
错误收件人已在 BEFORE_ASSEMBLY 拒绝并清理，但测试误将 Core
callback 的 wire reason 前缀用于内部 verifier 异常文本断言。
测试改为要求生产 `ProtectedGrantRejected` 类型；该层不伪造 Core
响应前缀。r5 的真实拒绝结果与日志保留，尚未运行的用例不计通过。

## Focused Acceptance

R6 维护测试 **11 PASS（50.78s）**：10 个独立 namespace 内的真实
requester/Controller/NFD/Provider 用例，以及 1 个显式 wire fixture
ASAN 诊断。r5 最终 native 定向构建 PASS（15.875s），r6 使用该二进制。

| Backend / input | Result | Verified boundary |
|---|---|---|
| native, Ed25519/P-256, valid | 2 PASS | 实际 requester APP Data → 精确获取 → 生产 runtime 解包 → 内容密钥摘要一致 → Zeroized |
| native, Ed25519/P-256, wrong recipient | 2 PASS | BEFORE_ASSEMBLY、`DI_PROTECTED_GRANT_REJECTED` |
| Python, Ed25519/P-256, valid | 2 PASS | 实际 APP Data → grant 验证 → 装配/密文暂存/磁盘读取/解密 → ORT CPU 输出 `[[6.0]]` → 清理 |
| Python, Ed25519/P-256, wrong recipient | 2 PASS | BEFORE_ASSEMBLY、生产 `ProtectedGrantRejected`、无装配明文 |
| Python P-256, disk ciphertext mutation / wrong content key | 2 PASS | grant 已验证；实际磁盘读取处变异；AEAD authentication 拒绝、生产 `ProtectedGrantRejected` |
| native P-256, repeated resource diagnostic | 1 PASS | 10 次真实 verifier/runtime 调用，ASAN 不再报告泄漏 |

10 个网络用例共收集 40 个子进程退出，状态仅 0 / 2（2 为负例 Provider
预期拒绝）；所有 Python 用例确认 canonical 不变、租约内容密钥零化、
无残留 `.onnx`。记录在 `spec181-grant-integration-20260905-r6/`。
选择元数据仍是明确的 fixture 输入；Python 断言内部异常类型，不声称
覆盖 Core callback 的 wire reason 映射。ASAN 单独使用 wire fixture，
不冒充网络证据。此前 r1--r5 失败全部保留。

维护入口为 `tests/python/test_spec181_provider_grant_integration.py` 和
`tests/integration-tests/ndnsf-di-protected-grant.t.cpp`。构建目标为
`spec181-native-grant-integration,spec181-native-grant-asan`；使用
`SPEC181_RUN_GRANT_INTEGRATION=1`、`SPEC181_RUN_GRANT_SANITIZER=1`，
将两个 `SPEC181_NATIVE_GRANT_*BINARY` 指向本轮 Waf 二进制，
`SPEC181_GRANT_INTEGRATION_ROOT` 每次指定新的私有原始目录。

## Native Regression

R6 Waf `unit-tests` 重建 PASS（14m27.912s），包含本轮拆分后的实际
factory 与共用凭据/网络源码。只运行以下定向筛选，**28 test cases、
126 assertions 全部 PASS**，exit 0；不是完整 unit 套件资格。
覆盖既有 grant parity/拒绝、收件人类型/权限、runtime 绑定、过期、
取消与清理。原始 `unit-build.log`、`native-focused.log` 保留在 r6。

```bash
./waf -o build-system-j2 build -j2 --targets=unit-tests
./build-system-j2/unit-tests --run_test='NativeProtected*,ProtectedRuntime*,NativeGrantVerifier*' --log_level=message --report_level=short
```

## Remaining Acceptance

T001/T002 的全部资源/异常边界与剩余 handler 源码闭包；本轮抽取
生产入口并修改 verifier，使此前统一 native 构建清单失效，下次完整
生产控制前须刷新。任务保持未勾选，整体 3/12，T007 BLOCK。
