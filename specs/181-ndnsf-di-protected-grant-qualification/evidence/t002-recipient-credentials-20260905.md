# T002 Native Recipient Credentials

**Status**: PASS (focused credential repair); T002 full acceptance IN_PROGRESS
**Layer**: production credential-loader unit regression; no network claim

## First Boundary

T002 要求 Ed25519 与 EC P-256 收件人信封。底层 verifier 已有两种
算法分支，但生产 factory 的私钥加载只接受 Ed25519，导致 P-256
在获取 grant 之前失败。新测试调用 factory 使用的同一配置加载入口，
读取临时注册表、公钥、收件人映射与真实生成的私钥，不注入替代 loader。

R1 Waf `unit-tests` 构建 PASS（13m51.109s）；筛选 `NativeProtected*`
运行 8 个用例，exit 201，唯一失败为 `NativeProtectedCredentialsLoadEcP256`：
`DI_PROTECTED_GRANT_REJECTED: provider recipient private key is not Ed25519`。
原始 `build.log`、`red.log` 保留在 ignored workspace temporary directory
下 `spec181-t002-credentials-20260905-r1/`。

## Repair Plan

保持注册表和权限检查；Ed25519 导出原始 seed，EC 仅接受 P-256
并传递给现有 `EcP256Pem` verifier；其余曲线/类型拒绝。私钥 PEM
临时缓冲在成功和异常路径均清除。修复后重建并复验同一筛选集。
T002 的完整生产验收与源码闭包仍未完成，T007 BLOCK。

## Focused Acceptance

R2 增量 Waf 构建 PASS（25.123s），同一 `NativeProtected*` 筛选的
8 个用例全部 PASS，exit 0。保留 `spec181-t002-credentials-20260905-r2/`
的 `build.log`、`green.log`。其中新增 5 项覆盖 Ed25519 seed、P-256
PEM 正例，以及 P-384、0644 私钥权限、未知 Provider 映射拒绝；
筛选还覆盖既有模型族规范化及受保护 store 行为。

```bash
./waf -o build-system-j2 build -j2 --targets=unit-tests
./build-system-j2/unit-tests --run_test='NativeProtected*' --log_level=message
```

生产 factory 调用 `loadNativeProtectedGrantConfig`；P-256 必须是有效
key pair 且曲线严格为 `prime256v1`。Ed25519 仍提取 32-byte seed。
临时 PEM 使用作用域清除，BIO/EVP/EC 对象使用 RAII；P-256 PEM
转移到既有 runtime 配置后，由 runtime 生命周期管理。

本轮将既有 factory/header 及其测试纳入源码检查点。handler 尚有
工作区混合变更，未整体纳入本轮提交。此处不证明 P-256 网络 grant
获取/解包；统一 native 构建清单需在下一次生产验证前重新生成，
此前 helper 修复后的 Y-B 记录仍只对应当时的源与构建。T002 未勾选，
下一步补全部生产验收并完成剩余 handler 源码闭包。
