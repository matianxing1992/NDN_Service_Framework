# T005-B Requester Grant Publication — 验收执行证据（2026-09-07）

Status: DONE。Selector：CPP(Spec182GrantClient/*)，承载 suite
`Spec182GrantClient` 于 manifest 登记的既有文件
[tests/unit-tests/di-native-grant-client.t.cpp](../../../tests/unit-tests/di-native-grant-client.t.cpp)
（existingSuite null、existingCases 2 —— 既有 2 个 module-level cases 原样迁入
suite，case 名保持不变；layers L1/2/6，L6 executeOwner 为 T016，真实
publication/fetch/Provider 消费由 T016 运行）。新集成 suite
`Spec182GrantClientFlow` 建于
[tests/integration-tests/di-native-requester-grant.t.cpp](../../../tests/integration-tests/di-native-requester-grant.t.cpp)
并注册进 integration-tests 目标（本次即运行通过）。

## 执行命令与结果

```
./waf -o build-nac182 build --targets=unit-tests -j2        # rc=0（12.0s）
build-nac182/unit-tests --run_test=Spec182GrantClient       # rc=0，8 cases
build-nac182/unit-tests --run_test=Spec182GrantAuthority,Spec182GrantClient,
  Spec182PlanSealer,Spec182NativePlanning                  # rc=0 回归
./waf -o build-nac182 build --targets=integration-tests -j2 # rc=0（link 增量）
build-nac182/integration-tests --run_test=Spec182GrantClientFlow  # rc=0，1 case
```

运行时 LD_LIBRARY_PATH 前置 nac-abe-integration-182/install/lib。raw 输出见
`.codex-tmp/t005b-unit.log`、`.codex-tmp/t005b-regression.log`、
`.codex-tmp/t005b-integration.log`。

## 切片现状与卡路径差异（registry 基线接管）

- execution-units Write 预期 U/di-native-grant.t.cpp；manifest card 登记文件为
  tests/unit-tests/di-native-grant-client.t.cpp（T001-era 切片既有 2 个
  module-level cases）→ 按 manifest 落 suite，execution-units 的 U 路径留档为
  差异记录（与 T005-A 同一 registry 约定）。
- acquire() 生产实现为既有 T001 切片（本卡无生产改动需求）：同步直线式
  request 构造 → authority.issue() → 二次 deadline 复检 → canonical
  exact name → Face publication port → binding；客户端无成员状态（除身份/
  authority/port 外无可变字段）。卡 Steps 的“工作 executor 等待且可取消”语义
  属 T010 executor 层（本卡 Read 的 runtime-boundaries Cancellation and
  Observer Contract：cancel 后晚到事件只消费/忽略、不复活成功）——可单测面由
  deadline 门（任何副作用前 fence）+ 每次 acquire 恰好一次 issue + 一次
  publication + 失败后无 pending 状态可复活覆盖，executor 编排留 T010。
- crypto（requester 签名/recipient-key envelope/authority 签名）继续由注入
  IssuePort 承载（CD-004 不引入网络 authority、不自写密码算法）；verifier
  envelope 与真实 Provider 消费在 T016。

## Verify 对照（Spec182GrantClient 8 cases + 集成 1 case）

- 既有 case 迁入
  `NativeGrantClientBindsAndPublishesExactName`/`NativeGrantClientRejectsWrongRequester`
  （原样，未改名 —— existingCases 记账不变）。
- 构造门
  `GrantClientConstructionRequiresIdentityAuthorityAndPublishPort`：空 requester
  identity/null authority/null publish port 均在构造期拒绝。
- view 完整性门（先于任何端口副作用）
  `GrantClientRejectsIncompleteViewBeforeAnyPortSideEffect`：requestId 空/
  attempt=0/epoch 空/expiry=0/非 canonical digest/错误长度 digest 逐项 →
  invalid_argument，issue 与 publish 计数全程为 0；恢复完整向量后恰好 1 次
  issue + 1 次 publish。
- timeout/cancel 门（runtime-boundaries deadline gate）
  `GrantClientRejectsExpiredDeadlineBeforeAnySideEffect`：已过 deadline → 原因码族
  `DI_PROTECTED_GRANT_REJECTED:`，issue=0 且 publish=0（过期等待不复活为工作）。
- 过期 grant 边界
  `GrantClientRejectsExpiredGrantThroughAuthorityBeforePublication`：deadline 尚在
  未来、grant 已过期 → 原因码族拒绝（只能来自 authority expiry 边界——该检查先于
  policy port，T005-A 顺序），issue port 与 publish 均未运行。
- exact-name 与 binding 契约
  `GrantClientPublishesCanonicalGrantNameFromViewAndIssuedGrant`：发布名精确等于
  canonical KEY-GRANT/v1 布局（publication identity 前缀 + 固定向量下的字面
  REQ/ATTEMPT/PLAN-CORE/MODEL/EPOCH/GRANT 组件次序，/request/%2F 转义为字面常量，
  不做编码再实现）；MODEL 组件取 model-manifest digest、为空时回落 model digest
  （与 request 构造回落一致）而非 plan-core；同向量重复 acquire 名字逐字节一致
  （issue=2/publish=2）；binding 各字段与发布 grant/过期时间一一对应。PROVIDER
  组件 64-hex（Provider identity 摘要）为 verifier 切片契约，由该切片自身套件覆盖。
- 晚到/错名不复活
  `GrantClientMismatchedPublicationIsConsumedOnceAndNeverRevives`：publication port
  回报错名 → 原因码族拒绝且恰好消费一次（publish=1）；随后重试为完整全新尝试
  （issue=2/publish=2），binding 只指认自己这次发布的名字 —— 失败无 pending
  状态，晚到回调不可能恢复成功（客户端同步、无成员状态 + 行为双证）。
- 真实 publication/fetch（集成层，fixture 真实传输/keyChain/ServiceUser）
  `Spec182GrantClientFlow/RequesterAcquirePublishesSignedExactNameDataConsumedByProviderFetch`：
  NativeGrantClient::acquire 的 Face publication port 接真实
  ServiceUser::publishSignedAppData（name 处于 /<identity>/NDNSF-DI 下），Data 出现
  于 User face；Provider 端点对 exact name 表达 fetch Interest（DummyClientFace 不
  保留 unsolicited Data，按 request-scoped 既有机制 replay）并校验：名字 canonical
  KEY-GRANT 布局、KeyLocator == requester 默认证书、content 逐字节等于 issued
  grant wire、binding 字段映射。authority crypto envelope 的真验证与完整 Provider
  消费在 T016（executeOwner 标注）。
- 时间确定性：unit 层全部无 wall-clock 敏感断言（deadline 用过去时间点/未来窗口、
  expiry 用相对 nowMs 偏移）；acquire 内 issue 后二次 deadline 复检存在但单线程
  unit 无法确定性触发（无时钟注入），边界留档 T016/T010 executor 层覆盖。

## 修改

- tests/unit-tests/di-native-grant-client.t.cpp：既有 2 个 module-level cases
  迁入 suite `Spec182GrantClient`（case 名不变），新增 6 个 cases（见上）。
- tests/integration-tests/di-native-requester-grant.t.cpp（新增）：suite
  `Spec182GrantClientFlow`，真实 ServiceUser publication + exact-name fetch case。
- tests/wscript：integration-tests 目标注册新 .t.cpp；di_integration_sources 补充
  NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantClient.cpp（client 符号进集成
  二进制）。
- 生产代码（NativeGrantClient.hpp/.cpp）：本卡无改动 —— acquire 语义经 8 个
  unit cases + 集成 case 验证与既有实现一致（测试未暴露生产缺口）。
- planned fixtures（native-wire-vectors.json 等）：保持 planned。
