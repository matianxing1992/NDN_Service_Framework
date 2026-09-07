# T008-A Native Input and Artifact Preparation — 验收执行证据（2026-09-07）

Status: DONE。Selector：CPP(Spec182Preparation/*)（12 cases）。
实现：`NativeRequestPreparation` 冻结端口（encodeInput/inspect/decodeResult 经
adapter、prepareInput/inspectModel/ensureArtifacts）在 baseline 切片中已
存在，本卡在其上完成卡要求的**认证 name/digest 绑定强化**——确保 catalog/
publication 端口只会被"本次请求/attempt 对被考察模型的真实 placement"驱动，
并把 requester-side I/O 全部留在 preparation、不移入纯策略：
1. `ensureArtifacts` 在触碰 artifact 端口前校验 proposal.requestId/attempt ==
   control 且 proposal.modelDigest == model.descriptor.contentDigest、
   proposal.graphDigest == model.graph.graphDigest——外来/陈旧 proposal 绝不
   到达 catalog 端口（`DI_NATIVE_ARTIFACT_BINDING_MISMATCH`）；
2. executionPlan.roles 非空且唯一，缺省校验在端口副作用之前；
3. 端口返回后要求 role-cover 精确等于 plan roles（缺一不可、多一不容），
   防 role 缺口导致 provider 不可装配、或多余 role 把 provider 侧装配
   偷渡进 requester preparation；
4. `NativeArtifactBinding::validate` 新增 catalog 源名 NDN-name 形状门
   （绝对名：前导 '/'、非空组件、无控制字符）——绑定里的 publication/
   catalog 源必须是可发布的绝对 NDN 名字。

两模型 input/result mapping 语义（Qwen bytes-identity、YOLO JSON 文档
parse-validation + byte-identity passthrough）由 suite 内冻结 fixture
adapter 镜像 python oracle 语义验证；生产 Qwen/YOLO adapter 类未在本卡
装配（registry 按 adapter ID 注入 Qwen/YOLO 实现属 T009-C wiring/T016，
symbol/code-design 冻结范围之外不做 scope creep），planner 文件
（NativeQwenPlanner/NativeYoloPlanner）零改动（T003-A 先例：实现即切片
已有，本卡未改动生产 planner）。

## 执行命令与结果

```
./waf -o build-nac182 build --targets=unit-tests -j2   # rc=0（0.8s no-op；
#   T008-A 源码变更已在前次 11.1s build 中编入；configure 复用 T006-C 起
#   的 build-nac182 树，参数同 T007-B evidence）
./build-nac182/unit-tests --run_test='Spec182Preparation/*'
#   rc=0；12/12 全绿（.codex-tmp/t008a-focused.log）
./build-nac182/unit-tests --run_test='!StreamFacade'   # 完整回归
#   rc=0；Running 879 test cases ... *** No errors detected
#   （.codex-tmp/t008a-full-regression.log）
#   注：裸跑仍见环境性 StreamFacade 族 PredictiveProviderExactWireValidation-
#   AndAtomicFlush 段错误 rc=139，与 T006-B/C/D/T007-A/T007-B 同族，负结果
#   已记 failure-log 2026-09-07；回归 gate 固定排除该族
```

raw 输出见 `.codex-tmp/t008a-build.log`、`t008a-focused.log`、
`t008a-full-regression.log`。

## 卡 Steps 对照

- **实现 inspect/encodeInput/decodeResult 与 prepareInput/inspectModel/
  ensureArtifacts 的冻结端口**：四端口与 NativePreparedInput/InspectedModel/
  ArtifactBinding validate 门在 baseline 切片中已就位（registry 构造门、
  digest 形状门、deadline fence、canonicalSourceName 生成
  `/NDNSF/DI/MODEL/<contentDigest>`）；本卡补齐端口之前的**身份绑定**
  （第 1 节 1--4）。端口语义不变，运行行为只变严（既有 879 cases 完整回归
  全绿证明无回归）。
- **复用两 adapter**：suite 内 TaskFixtureAdapter（参数化，adapterId/
  version/modelFormat/precision/semanticsLabel/graphLabel + encode/decode
  fn）镜像冻结 python 语义——Qwen BytesGenerationTaskAdapter 形状
  （adapter_name qwen-three-stage-pipeline、bytes identity）与 YOLO
  JsonTaskAdapter 形状（yolo26n-task v1、task object-detection、model
  yolo26n-onnx v1.0.0 formats (onnx,) precisions (float32,)、
  dict→canonical JSON sort_keys/(",",":")/allow_nan=False、decode
  json.loads）；DI C++ 只有 boost property_tree（无 nlohmann），按 M17
  "已有 bytes 不重复编码"，YOLO fixture 取 JSON 文档 parse-validation +
  byte-identity passthrough 语义（不重编码）。生产 adapter 的 registry
  注入按 code-design "registry 按 adapter ID 注入 Qwen/YOLO 实现"留给
  T009-C wiring/T016，本卡不发明中间装配路径。
- **认证 name/digest 绑定，I/O 位于 preparation，不移入纯策略**：
  sourceByRole 的 catalog 源是绝对 NDN 名、artifactDigestByRole 是 sha256
  digest；所有 I/O 只经 m_inspect/m_artifacts 注入端口；NativePlacement
  Proposal/ExecutionPlan（纯策略）未被本卡改动；planner
  （NativeQwenPlanner/NativeYoloPlanner）零改动——按 T003-A 先例引语
  "实现即切片已有，本卡未改动生产 planner"。

## 卡 Verify 对照

- **CPP(Spec182Preparation/*)**：12/12 全绿，gate 在
  `tests/unit-tests/di-native-preparation.t.cpp`（tests glob 自动拾取，
  无 wscript；case-manifest T008-A 行登记 suite + 11 个本卡实现 named
  cases，1 个 baseline 既有 case 保留）。
  - **两模型 input/result mapping**：case 2（Qwen bytes identity——
    pipeline bytes {0x00,0x01,0x7f,0x80,0xff} 经 encodeInput/decodeResult
    恒等，证明无 re-encode）；case 4（YOLO JSON 文档 validate + 恒等
    passthrough，输入故意乱序 key `{"image":...,"meta":...}` 证明不
    re-encode；malformed `{"image":` 在 encode 与 decode 双侧抛
    invalid_argument）。
  - **错 catalog/publication name/digest**：case 8（5 个非 NDN 形状源名
    ——无前导 '/'、`/a//b`、尾部 '/'、控制字符、`\0`——在 binding
    validate 阶段 invalid_argument）；case 5（foreign requestId/attempt/
    modelDigest/graphDigest 四维 mutation 各自在端口副作用前抛
    DI_NATIVE_ARTIFACT_BINDING_MISMATCH 且 artifactCalls==0）；
    case 9（role/manifest/recipe digest 缺形/缺 map → invalid_argument）；
    case 3（3-role plan 的 role-cover 精确成立、逐 role digest 相等）。
  - **身份/角色错配面**：case 6（空/重复 roles 端口前拒，port 未调用）；
    case 7（role gap、多余 role、同尺寸外来 role 集 → post-port
    mismatch）；case 11（inspect 声明外来 adapterId 的 mislabeled
    adapter 在 inspectModel 阶段 DI_NATIVE_MODEL_ADAPTER_IDENTITY_MISMATCH、
    adapterVersion 不匹配 invalid_argument）。
  - **清理边界**：case 12（deadline 过期/cancelAtPort 中途取消后置
    check 抛错且 calls 精确 0/1、nameless requestId 拒、fresh control
    成功——无残留 state 污染后续请求）；case 10（无 graph/artifact
    端口 fail-closed：DI_NATIVE_MODEL_GRAPH_PORT_NOT_CONFIGURED /
    DI_NATIVE_ARTIFACT_PORT_NOT_CONFIGURED）。
- **真实 publication 在 T016**：catalog/Repo publication 端口在 C++ 中
  尚不存在（真实 Core/Repo artifact port 属 T016 集成）；真实
  returned-name==requested-name、payload digest/size、signer binding
  检查本质在端口内部，本卡把端口前的 requester-side 身份/cover 门钉死。
- **I/di-native-preparation.t.cpp 未在本卡创建**：卡 Verify 全部为
  U-level selector；C++ 侧无真实 publication seam（无 canonical
  catalog/Repo）；PO-013 顺序证明禁止测试 harness 代做生产步骤
  （fake-composed ordering 不为真集成）；无 0-case integration seat
  先例（每个 integration .t.cpp 至少 1 case）。I-file 在卡 Write 列表
  中由 T008-B 承接（其 Write 含 I/di-native-preparation.t.cpp），真实
  Core admission/NFD 用例由 T016 落位。

## 关键发现与修正

- baseline 的 `NativeOfferAdmission::verify` 与
  `NativeOfferAdmissionRejectsUnauthenticatedAck` case 仍留在此 TU——
  原样保留为 suite 后裸 case，供 T008-B 移入独立
  `U/di-native-offer-admission.t.cpp` 的 Spec182OfferAdmission suite
  （不在本卡重复验收该 selector）。
- fixture 的 JSON 语义选择（parse-validate + byte-identity，而非
  canonical re-encode）按 M17 落地：DI C++ JSON 仅 boost property_tree，
  若试图 canonical 重编码会破坏已编码 bytes 恒等并引入第二 JSON 方言；
  decode 侧 json.loads 语义由 property_tree parse 覆盖，round-trip 恒等
  经 case 4 钉住。
- 完整回归 879 cases（此前 T007-B 门为 859）增量即本卡 suite：12 个
  Spec182Preparation cases 减去 baseline 已在数的
  NativePreparationBindsAdapterAndGraphPort；无任何其它 suite 数量变化。

## 文件

- [NativeRequestPreparation.cpp](../../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.cpp)
  （ensureArtifacts 绑定强化 + NativeArtifactBinding NDN-name 门 + ndnName
  helper）
- [di-native-preparation.t.cpp](../../../tests/unit-tests/di-native-preparation.t.cpp)
  （Spec182Preparation suite 12 cases + 保留裸 case，T008-B 承接）
- [case-manifest.json](../../../tests/fixtures/spec182/case-manifest.json)
  （T008-A 行 existingSuite + 11 named cases）

## 残余风险

- 生产 Qwen/YOLO adapter 类未在本卡装配——fixture 镜像冻结 python
  语义，registry 按 adapter ID 注入留 T009-C wiring/T016；若届时
  生产 adapter 语义与 fixture 分歧需回归本 suite。
- 真实 catalog/publication 检查（returned-name==requested-name、
  payload digest/size、signer binding）在 Core/Repo artifact 端口内部
  （T016）；本卡只保证端口前的身份绑定与 role-cover 面。
- I/di-native-preparation.t.cpp 按卡归属顺延 T008-B/T016，本卡无
  integration-seam 证据落盘；T008-B 若不能落地该文件需在此记录原因。
- 完整回归门排除了环境性 StreamFacade 段错误族（负结果见
  docs/failure-log.md 2026-09-07）；focused/回归 suites 与全量
  （排除该族）均绿。
