# T004-A Canonical Plan Sealing — 验收执行证据（2026-09-07）

Status: DONE。Selector：CPP(Spec182PlanSealer/*)，承载 suite
`Spec182PlanSealer`，按 manifest card T004-A 登记于 planned 文件
[tests/unit-tests/di-native-plan-sealer.t.cpp](../../../tests/unit-tests/di-native-plan-sealer.t.cpp)
（existingSuite null、existingCases []、namedCasesPlanned []；layers L1/2/3/6，
L3/L6 executeOwner 为 T016）。reconfigure 后 `--list_content` 确认 suite 注册非空。

## 执行命令与结果

```
./waf -o build-nac182 configure --prefix=<t002a-l0-r2 staging> --with-tests
  --with-examples --toolchain-root=/usr/bin --disable-local-dependency-prefix
  --nac-abe-prefix=/home/tianxing/NDN/nac-abe-integration-182/install
  --ndn-svs-source-tree=/home/tianxing/NDN/ndn-svs
  --ndn-svs-build-tree=/home/tianxing/NDN/ndn-svs/build   # rc=0（6.6s；
  ant_glob 在 configure 期求值，必须重跑以纳入新增 .t.cpp）
./waf -o build-nac182 build --targets=unit-tests -j2      # rc=0（增量 12.3s）
build-nac182/unit-tests --run_test=Spec182PlanSealer      # rc=0，7 cases，*** No errors detected
build-nac182/unit-tests --run_test=Spec182NativePlanning,Spec182PlanSealer  # rc=0 回归，两 suite 全绿
```

运行时 LD_LIBRARY_PATH 前置 nac-abe-integration-182/install/lib（T002-A 工具链边界）。
raw 输出见 `.codex-tmp/t004a-build.log`、`.codex-tmp/t004a-test.log`。

## Verify 对照（7 cases，全部首次运行即通过）

- 固定 wire/签名字节精确一致
  `PlanSealerCanonicalSealingBindsSnapshotAndSingleSourcesProjection`：
  sealCore→grantView→finalizeSecurity→project→encode 一条链在固定
  snapshot/proposal 上输出确定性 coreDigest/planDigest（重复调用逐字节相同）；
  encode 字节与冻结的 7-key canonical 片段布局字面量精确一致（固定键序
  provider→request_id→attempt→plan_digest→plan_core_digest→
  ack_closed_digest→selected_role.role、无空白、字符串 canonical 引号、
  attempt 裸整数）；同一片段经独立 JSON 语法 oracle（Boost property_tree
  json_parser，非 encode 自身）解析后键集恰为 7 键、值逐字段等于 sealed plan
  来源（M22 字段单源）。
- 错误 endpoint 拒绝
  `PlanSealerRejectsProviderOutsidePlanEndpoint`：`project` 对未分配
  provider（含空串）抛 `invalid_argument("provider is not present in sealed
  plan")`；`grantView` 对自洽但未被 plan 分配的 ghost offer 抛
  `invalid_argument("provider is not assigned by the plan")`；拒绝后同一
  sealed plan 的规范 endpoint 仍可 project/encode（首边界语义）。
- 缺 grant 拒绝
  `PlanSealerRejectsIncompleteSubstitutedOrOutsideGrantCover`：protected
  plan（requireProtectedArtifacts=true）零 grant / 外来 provider grant /
  幽灵 role grant / (provider,role) 重复 grant / 空 recipient / 非 digest
  grantDigest 全部在 finalizeSecurity 抛 invalid_argument（cover 不完全、
  outside assignment、binding invalid）；精确完整 cover 后密封成功。
  `PlanSealerPlaintextPolicyNeedsNoGrantCover`：plaintext 策略（对照 Python
  plaintext-v1 epoch）零 grant 合法，projection 单源且 hasGrantBinding=false。
- 错 ACK digest 拒绝
  `PlanSealerRejectsNonCanonicalAckDigestAtSealBoundary`：非 canonical
  格式 ACK digest 在 sealCore 末段 core.validate 被拒
  （"core identity is incomplete"，canonical 字符串无法进入 digest 身份）；
  对已密封 core 篡改 ack 字段为非 digest 串同样拒绝。
  `PlanSealerDigestAndEncodeBytesAreTamperSensitivePerDimension`：语义层
  篡改 —— 换成 well-formed 但不同的 ACK digest / ACK-offer digest /
  policy digest / grantName / grantDigest / recipient 时，受影响的
  coreDigest 或 planDigest 与 encode 字节逐一变化（canonical 覆盖该维度），
  不在 canonical 范围内的维度保持不变（policy 篡改不扰动 coreDigest），
  无静默等价。
- encode 首边界与 canonical 转义
  `PlanSealerEncodeRejectsIncompleteProjectionsAndEscapesCanonically`：
  空 provider/requestId、attempt=0、非 digest planDigest、空 role 均在
  encode 抛 invalid_argument；引号/反斜杠值经 JSON 解析 round-trip 原样
  还原；可选 digest 字段缺失时片段键位不丢失（空串占位，布局固定）。
- 真实 Core commit 与 Provider parser 协作：留 T016（本卡只验证 sealed
  plan 自身一致性与拒绝面；encode 片段被真实 Core/Provider 消费、planDigest
  的密码学再校验、多 role 单 provider 的逐 role 投影迭代均为 T010/T016 义务）。

## 与冻结 Python PlanSealerV3 的语义对照

- seal_core：request/attempt/candidate 绑定、deadline 过期、proposal 引用
  ACK offers 之外 provider 的拒绝 ↔ native sealCore requestId/attempt/
  model/graph/candidateDigest 绑定 + provider-not-in-ACK-offers 拒绝，共享
  "proposal 必须绑定已 ACK snapshot" 语义区间；disposition/residency 细化
  已由 NativeProviderPlanningView::validate 前置扁平化（T003）。
- finalize_security：Python 按 protected epoch 的 provider 集合精确比对
  grant cover（多/缺/替代均拒）↔ native 按 (provider,role) 唯一对精确覆盖
  plan roles；Python grant 为 per-provider 粒度（GrantBindingV1），native
  grant 为 per-role 粒度（NativeGrantBinding 带 role），单 provider 多 role
  时 native 需每 role 一条 —— native 独立细化，授权签发（T005）按此粒度。
- project：Python 逐 role 传 execution_role/assembly/dataflow/device 参数并
  校验 role 归属 ↔ native project 从 sealed plan 单源派生首个 assigned role
  投影且不允许覆盖参数（CD-003），role 不归 provider 即拒绝。
- ack digest 格式：Python seal_core 原样 str 接收；native 在 core 身份校验
  拒绝非 canonical digest（fail-closed 收紧，无可达行为回退）。
- core/plan digest 字节：Python canonical_digest/自 digest ↔ native
  `sha256:` canonical 字符串（request|attempt|model|…|artifacts + policy +
  grant 序列）；T003 已定 native 独立字节契约，本卡将该契约固化为
  determinism + 布局字面量 + 逐维度篡改敏感断言。byte-exact 与 Provider
  parser 的对照（PO-003 独立 oracle 全链路）属 T016。

## 修改

- tests/unit-tests/di-native-plan-sealer.t.cpp（新增，planned）：suite
  `Spec182PlanSealer` 7 cases。
- NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.cpp：quote()
  控制字符 canonical 转义（`\b \f \n \r \t` 短转义、其余 <0x20 用 `\u00XX`）。
  由本卡 escape case 发现：原实现只转义 `"`/`\`，控制字符可原样进入 JSON
  字符串体导致非法片段（"invalid code sequence"）。修复对可达输入（ASCII
  provider/requestId，上游身份校验已排除控制字符）字节零变化，不改变任何
  canonical 字符串或 digest；encode 产出对任意输入均合法。
- fixture tests/fixtures/spec182/native-wire-vectors.json：保持 planned；
  本卡 golden 向量（canonical 布局 + determinism + 篡改矩阵）已固化于 suite
  断言与本证据，真实字节消费点（T016 Provider parser）再行落盘。
