# T008-B Authenticated Offer Admission — 验收执行证据（2026-09-07）

Status: DONE。Selector：CPP(Spec182OfferAdmission/*)（15 cases）。
实现：把 T008-A 期间仍驻留在 NativeRequestPreparation 内的 admission
slice（NativeAckEvidence/PolicySnapshot/BindingContext/verify）按 CD-013
runtime-boundaries ADD row 拆为独立 `NativeOfferAdmission.hpp/.cpp`，并把
ACK 证据形状补全为 Core `AckAuthenticationEvidence` 形状
（ServiceUser.hpp：trustSchemaValidated/signerIdentity/signerKeyLocator/
wireDigest + DI binding 字段 + capturedAtMs 镜像 python captured_at_ms）——
这些字段持有"已通过 Core 订阅路径验证"的结果（value-contracts：
由验证路径填写、非 caller 声明），随后形成唯一闸门：

```
verify(ack, policy, context, nowMs) → immutable NativeProviderPlanningView
```

1. **provenance 前置（DI_NATIVE_OFFER_REJECTED_UNAUTHENTICATED）**：
   `trustSchemaValidated==false` 直接拒（Core 只在通过 validated
   ServiceUser subscription path 投递 ACK 时置 True，Direct/unit fixture
   保持 false）；signer identity/key locator/wire digest 缺空、wire digest
   非 canonical sha256 形状、signer/locator 非 NDN 名、locator 不在
   `<signer>/KEY/` 命名空间（Core 从 key/cert 名提取 identity 的形状）
   ——每一项都是伪 provenance；
2. **policy 可用性（DI_NATIVE_OFFER_REJECTED）**：policyDigest 非 digest、
   acceptedRoles/backends 空、resourceSequence==0、任何 residency digest
   脏 → 在形成 view 前 fail-closed；
3. **context 绑定（DI_NATIVE_OFFER_REJECTED）**：ack 的 requestId/attempt/
   serviceName/modelDigest/graphDigest 必须等于本次 context——陈旧/外来
   offer 不得进入 planning；
4. **signer==provider（DI_NATIVE_OFFER_REJECTED）**：冻结
   ProviderOfferTrustVerifier::verify_ack 的 signer==provider 规则——
   ACK 必须由它声称的 provider 签署，任何其它身份的 coherent 签名都是
   绑定失败而非有效 provenance；
5. **policy containment（DI_NATIVE_OFFER_REJECTED）**：provider/signer
   必须落在 immutable policy 的 accepted 列表内（service 同理），
   controllerVersion 非空、offerDigest 是 digest；policy 只约束哪些
   Core-authenticated offer 可以 planning——**policy 绝不成为 second
   Trust Schema，不携带任何 key map 或 HMAC key**；
6. **wall-clock 有效性（DI_NATIVE_OFFER_REJECTED）**：ack capturedAtMs
   在未来（伪造时钟声明）或 expiresAtMs<=now、policy expiresAtMs<=now
   拒。

全部 gate 通过后才由"authenticated ACK（provider/offerDigest）+ policy
（roles/backends/residency/freeBytes/resourceSequence）"构造 immutable
view（preparationAccepted=executionAllowed=true）并 `validate()`。
"拒绝 caller trusted=true 和 Python verifier callback"以 **API 缺位**
落地：类无 ctor 参数、verify 无 trust flag 与 callback 槽、证据字段默认
false 且由基于内容的 provenance gate 强制；真实 evidence 的构造所有权
（validated subscription path 投递）由 T016 Core 集成实现。

## 执行命令与结果

```
./waf -o build-nac182 build --targets=unit-tests -j2   # rc=0（两次 11.6s/
#   15.0s，含新 TU 自动拾取；configure 复用 T006-C 起 build-nac182 树，
#   参数同 T007-B evidence）
./build-nac182/unit-tests --run_test='Spec182OfferAdmission/*'
#   rc=0；15/15 全绿（.codex-tmp/t008b-focused.log）
./build-nac182/unit-tests --run_test='!StreamFacade'   # 完整回归
#   rc=0；Running 893 test cases ... *** No errors detected
#   （.codex-tmp/t008b-full-regression.log）
#   注：裸跑仍见环境性 StreamFacade 族 PredictiveProviderExactWireValidation-
#   AndAtomicFlush 段错误 rc=139，与 T006-B/C/D/T007-A/B/T008-A 同族，负结果
#   已记 failure-log 2026-09-07；回归 gate 固定排除该族
```

raw 输出见 `.codex-tmp/t008b-focused.log`、`t008b-full-regression.log`。

## 卡 Steps 对照

- **使用 Core 认证结果检查 policy/有效期/绑定再形成 immutable view**：
  gate 顺序 = provenance 前置 → policy 可用性 → context 绑定 →
  signer==provider → policy containment → wall-clock（第 1 节 1--6），
  与 frozen python verify_ack 顺序与 CD-013 bullet 对齐；验证内容覆盖
  Trust Schema 结果、signer identity/key locator/wire digest、policy 的
  provider/service/key/candidate 绑定、request/model/graph/有效期；Core
  packet auth 与 DI candidate policy 不合并为字段相等（provenance 是
  前置事实，policy 只做 containment）。
- **拒绝 caller trusted=true**：无任何接受调用方声称已验证的槽位——
  trustSchemaValidated 只能由 Core 验证路径置位（默认 false），内容门
  （signer/locator/wire shape、KEY 命名空间）使 caller 无法构造可通过的
  伪证据；CD-013 proof PO-013：测试 harness 不做生产步骤，T016 的
  validated subscription path 是唯一置位者。
- **拒绝 Python verifier callback**：C++ API 无 callback、无 key map、
  无第二 Trust Schema——verify 只消费结构化的已验证据 + immutable
  policy snapshot。
- **真实 Core admission 留 T016**：in-process suite 全部为 U-level
  fixture（trustSchemaValidated 手动置位的已是"持 Core 结果"形状的
  mimic，不代做验证本身）。

## 卡 Verify 对照

- **CPP(Spec182OfferAdmission/*)**：15/15 全绿，gate 在
  `tests/unit-tests/di-native-offer-admission.t.cpp`（tests glob 自动
  拾取，无 wscript；case-manifest T008-B 行登记 suite + 14 个本卡新增
  named cases，1 个既有 case 保留）。
  - **伪 provenance**：case 1（trustSchemaValidated=false，全字段
    合法仍拒——迁移自 di-native-preparation.t.cpp 的裸 case，断言升级
    为 code-string 精确相等 UNAUTHENTICATED）；case 2/3（signer/locator
    空）；case 4（wire digest 空/非 digest/`sha256:zz`/空 digest 四形状）；
    case 5（5 个越界或非 NDN locator：`/other/KEY/1`、`/provider/keys/1`
    （无 /KEY/ 段）、裸 `/provider`、相对名 `provider/KEY/1`、双斜杠
    `/a//b/KEY/1`——UNAUTHENTICATED）。
  - **错身份/策略**：case 6（coherent 外来签名：signer="/other" 且
    policy 的 acceptedSignerIdentities 含 "/other" 仍被
    signer!=provider 拒——REJECTED 而非 UNAUTHENTICATED，mirror python
    该规则）；case 7/8/9（signer/provider/service 各在 policy accepted
    列表之外 → REJECTED）；case 10（requestId/attempt/serviceName/
    modelDigest/graphDigest 五维 context mutation 各自拒）；case 11
    （policy digest 脏/roles 空/backends 空/resourceSequence==0/
    residency 含脏 digest → REJECTED）；case 14（controllerVersion 空、
    offerDigest 空/脏 → REJECTED）。
  - **过期 ACK**：case 12（capturedAtMs>now 伪造时钟声明、expiresAtMs
    ==now 已过期 → REJECTED）；case 13（policy expiresAtMs==now →
    REJECTED）。
  - **无合法 view 就不能进入 strategy**：唯一 accept 路径 case 15——
    15 个 reject cases 全部抛 runtime_error 且不返回 view；case 15 验证
    view 由 ack.provider/offerDigest + policy 六维构造、roles/backends/
    residency/freeBytes/resourceSequence 逐字段相等、
    preparationAccepted/executionAllowed 均 true、`view.validate()` 无抛、
    两次 verify 结果逐字段一致（无内部状态、确定性 immutable）。
- **真实 Core admission 留 T016**：validated ServiceUser subscription
  路径投递 ACK→置位 trustSchemaValidated→构造 NativeAckEvidence 属
  T016 集成；本卡把卡要求的闸门全部钉死并保持切片阶段零真实 Core
  依赖。
- **I/di-native-preparation.t.cpp 未在本卡创建**：T008-A 已给出该
  I-file 的理由（无真实 publication seam、PO-013 禁止 harness 代做
  生产步骤、无 0-case integration seat 先例），T008-B 同理——本卡全部
  Verify 均为 U-level selector，C++ 侧尚无真实 Core subscription/
  admission seam（T016 之前无 T 级集成入口），0-case seat 违反
  "每个 integration .t.cpp 至少 1 case"先例；理由续记于此。

## 关键发现与修正

- T008-A 期间 verify() 以 3-arg 形态（ack, policy, context）存在；
  本卡按 CD-013 executed 4-arg（ack, policy, context, nowMs）定稿——
  wall-clock 由调用侧注入，suite 以固定 nowMs 使过期/未来测试确定性
  可复现。
- 拆出后 NativeRequestPreparation.cpp 的 verify 实现、contains helper
  及随之死亡的 hashBytes（sha256 计算已无人使用）一并移除，暴露
  `-Wunused-function` 警告已清零。
- python oracle 按 (provider, service) entry 键查表 + locator 落在该
  entry keyLocatorPrefix 之下；native 以 flattened
  acceptedProviders/acceptedServices/acceptedSignerIdentities + KEY
  命名空间近似（signer∈accepted 且 locator∈signer/KEY/）；差异的
  含义：policy holder 必须把 provider 与其 signer 同时列入各自列表
  （signer==provider 使二者恒等），近似在 containment gate 上比 python
  entry 键约束更细粒度——case 7 单测该面。
- 回归计数 879 → 893：净 +14 = 新 suite 15 cases − di-native-
  preparation.t.cpp 顶层裸 case（迁移后不在该 TU 计数）；无其它 suite
  数量变化。
- 代码错误串（DI_NATIVE_OFFER_REJECTED_UNAUTHENTICATED /
  DI_NATIVE_OFFER_REJECTED）不进入 contracts（T008-A
  DI_NATIVE_ARTIFACT_BINDING_MISMATCH 先例：family-level code-string
  runtime_error，语义由本 evidence 固化）。

## 文件

- [NativeOfferAdmission.hpp](../../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp)
  （新：NativeAckEvidence（Core 形状 + capturedAtMs）/NativeOfferPolicy-
  Snapshot/NativeOfferBindingContext/NativeOfferAdmission::verify 4-arg；
  类注释固化"无 Trust Schema、无 caller trust flag、无 callback"）
- [NativeOfferAdmission.cpp](../../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.cpp)
  （新：六段 gate + immutable view 构造；digest/ndnName/underKeyNamespace
  helpers）
- [NativeRequestPreparation.hpp](../../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp)
  （移除 4 个 admission types 与 NativePlanSealer include）
- [NativeRequestPreparation.cpp](../../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.cpp)
  （移除 verify 实现、contains、死 hashBytes 与 openssl/sstream include）
- [di-native-offer-admission.t.cpp](../../../tests/unit-tests/di-native-offer-admission.t.cpp)
  （新：Spec182OfferAdmission suite 15 cases）
- [di-native-preparation.t.cpp](../../../tests/unit-tests/di-native-preparation.t.cpp)
  （移除裸 case，suite 主体不变）
- [case-manifest.json](../../../tests/fixtures/spec182/case-manifest.json)
  （T008-B 行 existingSuite + file 落位 + 14 named cases）
- [tasks.md](../tasks.md)（T008-B → DONE，含本 evidence 与 case 分布）

## 残余风险

- per-(provider,service) entry 键绑定 vs flattened 列表的差异如上记录；
  T016 用真实 policy holder/entry 语义核对时若需精确键约束，改的是
  NativeOfferPolicySnapshot 形状而非闸门顺序。
- trustSchemaValidated 的置位只能由 T016 validated subscription path
  证明；在此之前 U-level fixture 手动置位不构成真实验证证据（PO-013
  顺序证明），真实 Core admission 用例全部留 T016。
- I/di-native-preparation.t.cpp 在本卡（及 T008-A）均无 integration
  seat 可落；真实 NFD/Core 集成用例属 T016，届时补该文件或按卡审
  裁撤。
- 完整回归门排除环境性 StreamFacade 段错误族（负结果见
  docs/failure-log.md 2026-09-07）；focused/回归 suites 与全量（排除
  该族）均绿。
