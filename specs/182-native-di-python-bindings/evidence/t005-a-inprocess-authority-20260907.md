# T005-A InProcess Authority — 验收执行证据（2026-09-07）

Status: DONE。Selector：CPP(Spec182GrantAuthority/*)，承载 suite
`Spec182GrantAuthority`，按 manifest card T005-A 登记于 planned 文件
[tests/unit-tests/di-native-grant.t.cpp](../../../tests/unit-tests/di-native-grant.t.cpp)
（existingSuite null、existingCases []、namedCasesPlanned []；layers L1/2/6，
L6 executeOwner 为 T016）。reconfigure 后 `--list_content` 确认注册非空。

## 执行命令与结果

```
./waf -o build-nac182 configure <t002a-l0-r2 同参>   # rc=0（6.6s；ant_glob 纳入新 .t.cpp）
./waf -o build-nac182 build --targets=unit-tests -j2  # rc=0（增量 10.4s）
build-nac182/unit-tests --run_test=Spec182GrantAuthority  # rc=0，6 cases，*** No errors detected
build-nac182/unit-tests --run_test=Spec182GrantAuthority,<grant-client 2 module cases>,
  Spec182PlanSealer,Spec182NativePlanning                    # rc=0 回归，全部通过
```

运行时 LD_LIBRARY_PATH 前置 nac-abe-integration-182/install/lib。raw 输出见
`.codex-tmp/t005a-build.log`、`.codex-tmp/t005a-test.log`。

## 切片现状与卡路径差异（registry 接管约定）

执行卡 Write 预期 NativeArtifactPolicyAuthority 独立
.hpp/.cpp；实际 `NativeArtifactPolicyAuthority`（含 NativeGrantRequest/
NativeKeyGrant/IssuePort）已作为 T001-era 切片与 NativeGrantClient 同置于
`NativeGrantClient.hpp/.cpp` 并随 T002-A L0 安装（r1/r2 staging 头一致）。
按 registry 基线"核对复用，避免重新实现已有部分"，本卡不重做文件拆分；
planned suite 文件 di-native-grant.t.cpp 与 selector 保持，卡语义验收落在
既有 authority 类型上（Card Write 的 planned 文件拆分留档为差异记录）。
issue() 实现职责（header 注释）：crypto（requester 签名/recipient-key
envelope/authority 签名）由注入 IssuePort 承载，authority 仅持有校验 ——
不引入网络 authority，不自写密码算法（CD-004）。

## Verify 对照（6 cases）

- 固定证书/时钟向量
  `IssueAcceptsFrozenRequestAndFixedClockVector`：固定
  nowMs=1000/expiresAtMs=4000 向量下 issue() 把冻结 request（requester/
  provider/requestId/attempt/planCoreDigest/modelManifestDigest/epoch/
  artifactDigest）原样送达 policy port（逐字段断言），返回 grant 的
  expiresAtMs 强制等于 request；同向量重复 issue（纯 port）逐字节一致。
  时钟为显式 time_point 参数，不依赖 wall clock。
- expiry 原因码（固定边界）
  `IssueRejectsExpiredGrantAtFixedClockBoundary`：now==expiresAtMs 与
  now>expiresAtMs 均拒、now=expiresAtMs-1 通过；拒绝携带注册原因码族
  `DI_PROTECTED_GRANT_REJECTED:` 前缀（code-design：异常全文非协议
  oracle，只断言族前缀）。
- caller 自授权限拒绝（Steps + Python "grant requester cannot be the
  selected Provider" 对照）
  `IssueRejectsSelfAuthorizedRequesterWithReasonFamily`：requester==
  provider 以原因码族拒绝 —— 本卡补齐的生产缺口（既有 validateRequest
  未查该维度，Python frozen 在 expiry 之后显式拒绝）。
- wrong-recipient/key 拒绝面（issuer 边界 + 注入 policy 传播）
  `IssueRejectsIncompleteIssuerResponseWithReasonFamily`：issuer 返回空
  grantName/空 recipient/非 digest grantDigest/空 wireJson/expiresAtMs
  与 request 不符 → 各以原因码族拒绝（"substituted" 面）；
  注入 port 的 epoch/model-manifest 类 policy 拒绝原样传播（authority
  不吞改原因码）。密码学层 wrong-recipient-key（envelope 认证失败）属
  既有 NativeGrantVerifier 切片（CD-004 REUSE）与真实 Provider 消费
  （T016），本卡不越界实现。
- 身份完整性
  `IssueRejectsIncompleteRequestIdentity`：空 requester/provider/requestId、
  attempt=0、非 digest、空 epoch、expiresAtMs=0 → invalid_argument 于
  port 运行前拒绝；全部拒绝后冻结向量仍通过。
- secret 生命周期
  `AuthorityConstructionRejectsEmptyIssuePort`：issue port 构造期必填
  （无默认 issuer）。secret/私钥/内容密钥不在 request 字段、不在
  authority 状态（类型无 key 成员，构造仅收 IssuePort；key 只存在于注入
  port 闭包与 verifier 的 recipient key/credentials 切片）—— 结构层面
  保证，运行断言为空 port 拒绝与请求无 key 字段。
- 真实 Provider 消费/网络 authority：本卡不新增网络服务（Work-unit
  ForbiddenChanges）；T016 执行真实 grant publication/fetch 与 Provider
  验证消费（executeOwner 标注）。

## 修改

- tests/unit-tests/di-native-grant.t.cpp（新增，planned）：suite
  `Spec182GrantAuthority` 6 cases。
- NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantClient.cpp：issue()
  新增 requester==provider 自授权限拒绝（原因码族
  DI_PROTECTED_GRANT_REJECTED，Python frozen issue 语义对照）。既有
  validateRequest/expiry/issuer 完整性校验与 acquire 路径未改动。
- planned 文件拆分 NativeArtifactPolicyAuthority.hpp/.cpp：保持 planned
  （registry 基线切片同文件形态取代），不在本卡新增空壳文件。
