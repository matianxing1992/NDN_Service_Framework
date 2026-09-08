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

### GA-1 Symbol Decision

新增 `NativeSignedGrantRequest` 保留 GrantRequestV1 全部规范字段；sign 返回新值，
签名私钥只以 EVP_PKEY handle 调用。`NativeArtifactGrantIssuer` 是 concrete owner，
构造冻结 operator 配置及 native key handle 寿命，issue(request,nowMs,expiresAtMs)
验证请求后消费 model/epoch content-key owner；不发布。原有 IssuePort wrapper
保持 ABI，GA-2 接入 concrete owner 后禁止将旧 wrapper 当生产策略实现。
共享 grant wire/签名/封装 helper 位于现有 NativeGrantVerifier.cpp，避免复制规范
编码和 HKDF；Provider unwrap 不改变。新增头/源由既有 Waf glob 收录。
本轮扩展 Write 至 NativeGrantVerifier.cpp 与新 C++ issuer 单测；没有更改现有类型布局。

### GA-1 Static Checkpoint

2026-09-08 **STATIC_PASS / TESTS_DEFERRED / R2-B4**。已新增 signed request value、
配置式 NativeArtifactGrantIssuer 和真实签发 helper。支持 Ed25519/X25519/P-256
recipient、OpenSSL key exchange/HKDF/AES-GCM/Ed25519，keyId 与 model/epoch key owner
由配置提供；返回规范 KeyGrantV1，不发布。无现有 public class layout 修改。
官方 review-agent 只读复核数据流、失败清理、密钥共享寿命和新 C++ 测试，修正：
OpenSSL 1.1.1 使用 ossl_typ.h；完整 wire 需排序而不只是 signing payload；purpose
实际冻结值为 DISK_CIPHERTEXT_ASSEMBLED；补 request/wire/signature 长度边界。
三项新 C++ cases 覆盖真实 Provider unwrap、错误 key/binding/signature、策略拒绝在
content-key consumption 之前、两种曲线及原始 X25519 public key。**仅已编写，未运行**。
GA-2 客户端签名请求输入、答复认证与 publication 生命周期尚未接入；GA-3 独立 Python
密码/wire 对照与 sealer 组合仍待完成。本批保持 PARTIAL，不单独构建 GA-1，不提交
未经测试的源码单元；下轮直接续接当前 diff，不能重写为完成的 T005。

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

### GA-2/3 Static Checkpoint

2026-09-08：新增 NativeAuthenticatedGrantClient（新类型，不改变旧 client layout）。
acquire 从 core/admitted offer/security 调用实际 grantView，按 SDK 的 role/offer/policy
字段计算完整 view digest；签名后调用 concrete issuer，使用已有 ParsedGrant 验证
authority/全部 binding/expiry/tiers/digest/signature/规范 wire，再发布并检查终态。
production 构造绑定 Core postToIo/publishSignedAppData；worker 有界等待，排队任务
持有 shared owner 和独立 pending state；取消/超时 abandon 后晚回调不写成功。
Publish/Clock 构造供传输隔离与固定时钟测试；不替代策略或密码实现。默认 requester
尚未选择此新 client，旧端口 wrapper 的退出/调用方切换继续由 T010/T013 负责。

GA-1 复审发现 placement canonical JSON 的 ensure_ascii=True 与 grant 的
ensure_ascii=False 不同；修复 signed request/full grant 使用排序 UTF-8 JSON，
加入中文 requestId 独立对照。请求和全 wire 均有 64 KiB 上限。完整答复与 ParsedGrant
重建的规范 wire 一致，额外字段、重复字段及非规范类型不能通过答复认证。

GA-3 在实际 V3 placement C++ fixture 上覆盖 grantView→issue→verify→publication
port→finalize/project→Provider unwrap，并测发布后取消、再次调用取消、错发布名、
过期前置拒绝。新 integration case 使用 concrete issuer 和同一 Core publication
factory/worker，再验证 Data 内容中的 grant；只编写，T016 执行。独立 Python oracle
核对四种 C++ 输出的 request 签名、完整 grant 规范字节、authority signature 与
content-key unwrap，使用公开固定测试 keys（Ed seeds a/b/c 和 P-256 scalar 7）。

Write 扩展至 NativeAuthenticatedGrantClient.hpp/.cpp、V3 placement test、既有
integration grant harness 和 check-grant-issuer-wire.py。官方 review-agent 逐成员
及整批只读复核已完成，修正上述字节边界后 **READY_FOR_BATCH_TESTS**。共享构建
后运行 Spec182GrantIssuer、Spec182V3Placement、Spec182PlanSealer、旧 grant suites
及现有 NativeGrantVerifier 三项 fixed-vector cases；网络/集成不在本批执行。

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

### Final Batch Result

**DONE (local implementation batch only)**。基线 879c9d8d 上完成 GA-1/2/3；旧路径审计
保持历史事实，新 production owner 已新增，默认 requester 迁移尚未完成。

- `waf -o .codex-tmp/spec182-yolo-semantic-r1/build build --targets=unit-tests -j4 -v`
  exit 0，33.757s；[build.log](../../../.codex-tmp/spec182-r2-b4/build.log) 保留实际命令。
  仅 NativeGrantVerifier、NativeArtifactPolicyAuthority、NativeAuthenticatedGrantClient、
  issuer test、V3 placement test 五项编译 + unit-tests link；Core/UAV 没有重编。
  system g++9.4/ld2.34/Boost1.71/NAC prefix 与旧树一致，vmstat 后三次 si/so=0。
- C++ selector `Spec182GrantIssuer,Spec182V3Placement,Spec182PlanSealer,Spec182GrantAuthority,Spec182GrantClient,NativeGrantVerifier*`
  exit 0，**37/37 cases、672/672 assertions PASS**，见
  [focused.log](../../../.codex-tmp/spec182-r2-b4/focused.log)。实际 sealer-grant-project
  组合在 `AdmittedPlacementSealsSdkCoreAndRejectsTampering` 的 cpu fixture 执行。
- `NDNSF_GRANT_ISSUER_ORACLE_OUTPUT` 输出仅公开固定测试配置的 wire/signatures；
  `python3 tests/fixtures/spec182/check-grant-issuer-wire.py .codex-tmp/spec182-r2-b4/grants.jsonl`
  修复 backend 参数后 exit 0，4 requests/signatures/grants/unwraps PASS，见
  [oracle-r2.json](../../../.codex-tmp/spec182-r2-b4/oracle-r2.json)；没有重跑 native build。
- `validate_design.py` 与最终 diff 检查 PASS；integration case 只编写/登记，未运行。

T005-A/B 父卡仍 PARTIAL：默认 requester 的真实注册/configuration、发布 I/O/cancel
调度及完整 Provider execution 尚需 T010/T016。issuer 的 content-key callback 必须由
实际 model/epoch 密钥 owner 连接，不能把本测试的固定 key 用于已发布模型。
旧无签名 client/IssuePort wrapper 未成为新生产实现，T010/T013 切换/退出义务不变。
下一步接通 group capability 与默认 requester，不能继续以端口测试延后生产链。

### First Validation Boundary

native build exit 0（33.757s，5 compiles + unit-tests link），37 C++ cases exit 0。
独立 Python oracle 首次在 P-256 fixture derive_private_key 缺旧库 backend 参数时
TypeError，原始错误保存于 [oracle-error.txt](../../../.codex-tmp/spec182-r2-b4/oracle-error.txt)。
此为离线工具 API 边界，不是密码验证拒绝；先记失败，再修复并单独重跑 oracle。

官方 review-agent 只读复核本轮文档差异与上述 canonical 源码：未引入协议或
验收降级；历史记录未重写。`validate_design.py` exit 0，结果见
[design.json](../../../.codex-tmp/spec182-r2-b4-audit/design.json)；`git diff --check`
PASS。两份其他会话的 dependency/generation design 改动不纳入本 checkpoint。
