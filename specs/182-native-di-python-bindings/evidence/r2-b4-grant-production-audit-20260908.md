# R2-B4 Production Grant Chain

## Source Audit

基线 `0dc4e67e`。本轮只读源码审查纠正 T005 的剩余工作描述；没有运行密码、
网络或完整请求验收。Context Mode project/active health 均 PASS；CodeGraph 命中
canonical NativeGrantClient 源码，也返回旧 staging 副本，后者不作为事实来源。

1. **GA-01 / OPEN**：`NativeGrantClient.hpp::NativeGrantRequest` 没有
   GrantRequestV1 的 grantViewDigest、allowedResidencyTiers、purpose、issuedAtMs、
   requesterSignature。`NativeGrantClient::acquire` 直接构造未签名请求调用 issue。
   对照 `core/protected_artifacts.py::GrantRequestV1` 和
   `security/grant_provider.py::AuthorityBackedGrantProvider.__call__`，并非 wire parity。
2. **GA-02 / OPEN**：`NativeArtifactPolicyAuthority::issue` 只检查字段非空、
   expiry 和 requester/provider 不同，然后调用 IssuePort。生产 cpp 下无该端口的
   真实签发构造调用方；没有 requester signature verification、operator-bound
   model/epoch/residency policy、recipient key lookup、content-key ownership 或
   encryption/signature creation。`security/artifact_policy_authority.py` 和
   `security/requester_grant_pipeline.py` 明确拥有这些行为，不能留在 Python。
3. **GA-03 / OPEN**：authority 对结果仅检查非空和 expiry 相等；acquire 不验证
   authority signature、grant payload binding、recipient 等于 selected Provider。
   `di-native-grant-client.t.cpp::frozenGrant` 及首个正例返回 `/recipient/a`，而
   selected Provider 为 `/provider/a`，仍按成功断言。其证据只能证明端口转发。
4. **GA-04 / OPEN**：acquire 仅接受 PublishPort；生产构造尚未连接
   `ServiceUser::publishSignedAppData`。只有调用 issue 后的 deadline 检查，没有
   publication 后 terminal/deadline fence，也没有取消输入。Core 已有 postToIo/
   isOnIoThread；复用这些入口，禁止在 Face 线程阻塞或用晚到返回恢复成功。

Provider 的 `NativeGrantVerifier` 已有独立验证和 unwrap 实现，继续复用并保留。
`ProviderGroupCoordinator` 属于 DI 的 group capability/operation owner；Core 提供
通用认证/传输。group capability 是另一授权对象，不能用伪 grant 或裸 epoch key
填充来绕过上述缺口。R2-B3 的 projection 构造成果保留，不赋予其密码验收含义。

## Registered Implementation Batch

**R2-B4 / NOT_STARTED (implementation)**，owner=current executor；属于 T005-A/B，
输入依赖 R2-B3 已验证的 sealer/grantView，T004 整卡与最终请求资格仍开放。
本登记不解除任何原有 acceptance dependency；成员仅按实现依赖顺序推进。

| Member | Behavior and read/write boundary | Implementation dependency |
| --- | --- | --- |
| GA-1 Signed Request and Policy Issuance | NativeGrantClient + planned NativeArtifactPolicyAuthority；按现有 GrantRequestV1/KeyGrantV1 精确规范字节补签名请求、独立 operator policy、recipient/content-key lookup、封装和 authority 签名；只复用 OpenSSL 和既有 grant binding/zeroization 原语 | R2-B3 grantView；CD-004 |
| GA-2 Verified Publication | 同一 native client 先验证 authority 答复身份/签名/摘要，再经 ServiceUser 的 I/O executor 发布规范 exact name；deadline/cancel 覆盖发布前后及晚到回调，保持 owner 寿命 | GA-1 static gate |
| GA-3 Sealed Projection and Provider Consumption | C++ 测试从真实 sealer grantView 到签发、binding、finalize/project，再经现有 verifier 解出对应 content key；独立 Python oracle 只用于规范 wire/密码交叉核对，网络 harness 编写后留 T016 | GA-2 static gate |

Read：CD-004、execution-units T005-A/B、runtime-boundaries cancellation、上述四个
Python source owners、NativeGrantVerifier、ProtectedRuntime、ServiceUser signed Data API。
Write：NativeGrantClient.hpp/.cpp、NativeArtifactPolicyAuthority.hpp/.cpp（设计已登记）、
必要的共享私有 grant wire/crypto helper、di-native-grant.t.cpp、di-native-grant-client.t.cpp、
di-native-plan-sealer.t.cpp、integration/di-native-requester-grant.t.cpp、case-manifest
及离线 fixture。移动 verifier helper 时保留其公开行为，不能复制第二份规范编码。

构造配置绑定 requester/authority 的不同身份与密钥、允许模型集合、epoch、residency
和 Provider 公钥映射。私钥是构造时的 native owner/handle，不放进请求值或日志。
content key 由 model manifest/epoch 对应的既有 key owner 提供，并与 artifact 身份
核对；不能临时生成与已发布加密模型无关的密钥。Ed25519/X25519/P-256 支持范围
须与实际迁移 API 和已冻结 oracle 对齐，不能只实现最容易通过的一个算法。

新增类型/具体签名在 GA-1 编码前按 symbol/value contract 冻结；若需要改变既有
native public layout，先核对 ABI 消费者及受影响构建树，不能盲用旧二进制。

## Validation and Remaining Work

逐成员加载官方 review-agent 做只读审查，整批审查后统一构建及相关 C++ 单测。
共享选择器为 Spec182GrantAuthority、Spec182GrantClient、Spec182PlanSealer 和
实际受影响 NativeGrantVerifier suite（执行前从现有 manifest/源码确认精确名字）。
复用兼容树、默认 -j4，仅构建依赖图中受影响对象；若 ABI 改变则按真实依赖扩大范围。
负例至少覆盖未签名/篡改请求、未授权模型/epoch/tier、错 requester/authority/key/
recipient、内容密钥缺失、答复 binding/digest/signature 篡改、发布拒绝/超时/取消。
机密对象失败路径必须可核对释放，不能只检查 happy-path 字段。

本轮结果是 **AUDIT_COMPLETE / IMPLEMENTATION_NOT_STARTED**，T005-A/B 和父 T005
保持未完成。旧单测 PASS 原样保留，但不再描述为仅等 T004 依赖复核；生产签发本身
缺失。T010 默认 requester 继续由 R3 接线，最终跨进程及 no-Python 证明仍归 T016。

## Documentation Checks

官方 review-agent 只读复核本轮文档差异与上述 canonical 源码：未引入协议或
验收降级；历史记录未重写。`validate_design.py` exit 0，结果见
[design.json](../../../.codex-tmp/spec182-r2-b4-audit/design.json)；`git diff --check`
PASS。两份其他会话的 dependency/generation design 改动不纳入本 checkpoint。
