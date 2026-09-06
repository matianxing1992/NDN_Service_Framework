# Tasks: NDNSF-DI Protected-Grant and Qualification Closure

**Input**: [spec.md](spec.md), [plan.md](plan.md)，Spec 180 契约（继承）与
Spec 170 `artifact-assembly-v1` 契约。

**任务内聚规则**：一个任务 = 一个行为、一个 owner、一个验收门；测试
先行（focused failing case → 实现 → passing gate）。禁止机械拆分
"写测试/实现/跑测试"；也不得把两个独立行为（独立 owner/独立验收）
合并进一个任务。

**边界（2026-09-05 所有者决定）**：
- 撤销子系统（账本/网络服务/撤销校验）由另一分支开发；本 spec 不包含
  撤销任务，`revocationSequence` 固定为 1 的被动 wire 字段；
- 独立权威网络服务端（生产形态）为延期项；功能切片内权威运行在
  请求方进程中并复用既有 `ServiceUser.publish_signed_app_data` 发布
  路径（main 分支的明文路径证明"无网络服务"可工作，本切片不新增
  网络角色与前缀）。集成条件见 spec.md Out of Scope。

## Current Checkpoint (revision 6)

**Status**: `IN_PROGRESS / BLOCK`。2026-09-06 按当前提交与证据复核；
T001/T002/T003/T004/T006 的任务验收已闭合并勾选（5/12）。其余任务仍按完整验收判断，
未勾选不抹去已实现的代码与 unit 结果。

| Task | Implemented / executed | Remaining acceptance |
|---|---|---|
| T001 | PASS：真实发布/消费与受保护装配、全部绑定、inline/external 清理及请求/排队生命周期；最终 151 项定向回归、6 个真实进程用例 PASS，见 [请求生命周期与完成审查](evidence/t001-request-lifecycle-20260905.md) | 本任务验收完成；正式 MiniNDN 归 T005/T008 |
| T002 | PASS：真实 native grant 正负链、资源/请求生命周期、公共准备与 adapter、handler 接线均闭合；追加生成接口修复后 48 cases / 366 assertions PASS，见 [完整验收映射](evidence/t002-acceptance-20260905.md) | 本任务验收完成；新源统一 manifest 刷新与正式同源资格归后续门 |
| T003 | PASS：3 项 grant parity 检查消费 9 个向量；8 个装配向量分别走 Python/C++ 生产入口，16 项检查通过，含实际 ORT CPU 结果和 initializer/recipe/ABI 拒绝，见 [装配证据](evidence/t003-assembly-parity-20260905.md) | 本任务定向验收已闭合；native 格式操作共用生产 Python helper，后续同源资格仍归 T005/T008 |
| T004 | PASS：7 项 Python seam、6 项 Core 定向检查；重建后真实 Provider 等待约 83 ms、Controller 12.39 s 就绪、等待中取消约 2.3 ms 且无热转；全部线程/网络清理，见 [生命周期证据](evidence/t004-lifecycle-acceptance-20260905.md) | 本任务定向验收已闭合；后续同源正式资格仍归 T005/T008 |
| T005 | 已停用旧自动重试入口，维护矩阵首个失败即停止并保留原始结果；118 项定向检查 PASS，见 [证据保留修复](evidence/t005-evidence-repair-20260905.md) | T007 PASS 后同源七子用例矩阵，保留所有失败 |
| T006 | PASS：153 项 Python、22 项 C++、3 rebuilt parity checks；r11/r12/r13 三种实际 Provider 拒绝及 r10 受保护正向控制通过；每次清理后 exit 0，见 [生产修复](evidence/t006-production-repair-20260905.md) | 本任务定向验收已闭合；同源正式矩阵仍归 T005/T008 |
| T007 | framework/native projection 源码单元与维护 native 构建已通过；见下方当前检查点 | BLOCK：local gate 实际源码/配置身份及候选闭包待修复和重审 |
| T008 | native 受保护 Y-B 定向正向控制已有证据 | T007 PASS 后执行同源本地资格清单与 Y-A/Y-B/Y-N 正式矩阵 |
| T009--T012 | 继承工具链 | 本 Spec 候选、SIF、Tiger、终局均未闭合 |

**Latest progress (2026-09-06)**：5/12 已完成，G0 已关闭；当前唯一活动门为
G1/T007。local gate 的提交/源码身份单元已通过，当前控制性缺口为实际配置/工具链身份；
不得据此启动 T005/T008 正式矩阵或后续候选/SIF/Tiger 阶段。

| Closed unit | Current evidence |
|---|---|
| T002 acceptance / shared runtime | [完整验收映射](evidence/t002-acceptance-20260905.md)；[共用路径与差异 owner](evidence/shared-runtime-reuse-20260905.md)。共用准备与 adapter 已收口，生成 worker 授权修复为 48 cases / 366 assertions PASS |
| Qualification inventory | [范围与注册契约修复](evidence/t007-qualification-scope-20260905.md)：21 focused checks PASS；YOLO 三案例、活动 Spec 测试发现、案例路径/参数与输出 ID 边界已修复 |
| Evidence inventory | [完整逐文件清单](evidence/t007-evidence-inventory-20260905.md)，由维护脚本检查漂移；历史证据的层与用途保持各自范围 |
| Framework source closure | 提交 `1df718c8`；[26 cases / 204 assertions PASS](evidence/t007-framework-source-closure-20260906.md)，覆盖配套声明和 assignment 根名称传递 |
| Native projection closure | 提交 `1ba99000`；[29 cases / 133 assertions PASS](evidence/t007-native-plan-closure-20260906.md)，覆盖 COMPONENT_SET/后处理解析、根来源与多张量 scope/原授权组 |
| Committed native build | `1ba99000cd6b03705ea96f0b176d6a77fbbe8a1d` 的 tracked tree 无修改，维护 native build 与实际扩展导入/依赖身份 PASS；R3 receipt 见上一行证据。此结论仅限 host-local 构建，不是正式资格 |

**Current blocker**：[local gate identity](evidence/t007-local-gate-identity-20260906.md)
的源码单元 R7 已通过：39 项定向检查（3.44 s），错误 Git/源码
身份在子进程与输出目录创建前拒绝；执行期间源码变化使最终结果
UNQUALIFIED，保留全部子项与清理证据。实际 `1ba99000` 隔离构建
checkout 也通过只读源码校验。R1--R7 的失败和修复历史见上述链接。
下一步核查 `effectiveConfigDigest` 的实际消费、生成构建工具与
运行时/import 路径绑定；这些独立身份平面尚未闭合。T007 仍 BLOCK，
未运行正式矩阵；本单元完成不改变 5/12。
源码 checkpoint 曾被本地 hook 拒绝，已删除产品脚本中的助手目录
特例。R8 重验 39 项 PASS（4.46 s），真实 checkout 校验 PASS；
原 hook 保持启用，配置/工具链身份仍待关闭。

**Execution order**：T007 PASS → T005 七子用例同源矩阵 → T008 完整
本地清单与 Y-A/Y-B/Y-N → T009 候选 → T010 exact-SIF → T011 单次
Tiger Y-B → T012 终局。不得把定向测试或历史 PASS 代入这些未完成门。

**Shared runtime review (2026-09-06)**：已重新核对两种模型的 adapter
构造、生产准备 factory 的公共上下文绑定、生成路径向共同 worker
传递 guard；FR-015 已覆盖复用要求，保持现有模型差异 owner 与 YOLO
资格范围。同步修正共享说明中的旧 manifest/4 complete 表述，见
[当前源码核对](evidence/shared-runtime-reuse-20260905.md#current-review-2026-09-06)。
本轮为文档与调用链复核，完成数仍 5/12；下一步仍是上述 A05 身份修复。
验证：`audit_speckit_structure.py --strict` PASS（15 FR、6 SC、12 tasks、
5 complete、15 FR traced），`spec181_evidence_inventory.py --check` PASS，
`git diff --check` PASS；未重跑未改变的模型或 native 测试。

较早 4/12、6/7、7/7 与 CONDITIONAL PASS 均为历史状态，不覆盖当前
检查点。逐次 RED/GREEN、启动/构建失败、原始 run-id 与退役条件保留在
对应 evidence 及 Git 历史中；以 [audit.md](audit.md) 与
[修正证据](evidence/audit-repair-20260905.md) 为准。
本文件 `cpp/ndnsf-di/` 简写均相对 `NDNSF-DistributedInference/`；
`security/`、`core/` 简写相对其 `ndnsf_distributed_inference/`。

## Validation Standard

每个实现任务的验收必须同时满足适用层，缺一不可：

T001--T004/T006 的完成门为其 unit/定向 integration 验收；这些任务
引用 T005/T008 的 MiniNDN 是后续 FR 资格覆盖，不是反向完成依赖。
只有标记 `[MiniNDN]` 的 T005/T008 由完整网络资格闭合。T007 审查
已完成的开发验收与后续资格设计，不要求未来的 MiniNDN 结果先存在。

1. **单元测试（unit）**：纯函数与编码层的 focused red/green + 变异
   用例（错误输入、边界值、篡改字段）。不得启动 NFD、不得跨进程。
2. **集成测试（integration）**：必须走真实生产调用链，禁止 mock
   替换被测链。每个用例必须声明三要素：
   - **生产入口**：被覆盖的真实入口（如"进程内权威签发经真实
     requester→authority 调用链"）；
   - **预期判据**：成功的判据或注册的拒绝原因（如
     `DI_PROTECTION_EPOCH_REJECTED`）；
   - **边界位置**：断言发生的授权边界（如"在 Provider 装配之前、
     verifier 之内"）。
   负例必须到达真实 verifier 并被其在授权边界拒绝；无关失败、错误
   生命周期相位、内部策略异常不得充当预期结果（Spec 180 假 PASS
   教训的制度化）。
3. **MiniNDN 小模型 CPU 测试**：适用任务（标注 [MiniNDN]）必须在
   MiniNDN 真实 NFD/NDN-SVS 上用小模型 CPU 后端执行，直至终端
   Response 或注册边界拒绝，全量子进程退出收集、零未收集存活进程、
   清理完整。

禁止：标签 PASS、seam-only 证据、以单元测试冒充集成测试、以集成
测试冒充 MiniNDN 资格证据。每个证据文件头部声明证据层（implemented/
wired/executed/measured）。

## Phase 0: Fail-closed Safeguards (Priority: P0)

这些历史修正防止合成拒绝、状态冒充与明文路径冒充保护执行。R 项
保留原 ID 与历史证据，不计入 T001--T012 的完成率。当前缺口由对应
T 任务完成定向修复；在生产验收前保持失败关闭，并禁止晋升诊断结果。

- **R001 Y-N-E Fail-closed Guard**（historical unit evidence；T006 负责生产验收）。在真实 grant 变异（T006 实现）之前，
  runner 的 Y-N-E 子用例必须报告 `UNAVAILABLE`（结构化原因
  `Y-N-E:GRANT_VERIFIER_NOT_IMPLEMENTED`），禁止合成纪元异常充当
  `PROTECTION_EPOCH_REJECTED`；删除合成拒绝路径。文件：
  `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`、
  `tests/python/test_spec181_y_n_e.py`。验收：unit（verifier 缺席时
  Y-N-E 报 UNAVAILABLE 而非 PASS/拒绝原因）；integration（矩阵驱动下
  unavailable 记录不进入 PASS 计数）。吸收关系：T006 落地后删除
  UNAVAILABLE 路径，由真实变异拒绝取代。

- **R002 Native Runtime Fail-closed Guard**（absorbed；T002 验收完成，以下保留先行门原要求）。在真实 grant 获取/
  解包（T002 实现）之前，native Provider 对非 `plaintext-v1` 纪元
  赋值必须失败关闭并给出明确错误（`DI_PROTECTED_GRANT_UNAVAILABLE`），
  禁止仅凭绑定比对进入 `GrantVerified` 状态；现有绑定比对函数明确
  为"绑定一致性校验"（改名或文档化）。文件：
  `cpp/ndnsf-di/ProtectedRuntime.{hpp,cpp}`、
  `cpp/ndnsf-di/NativeProviderHandler.cpp`、
  `tests/integration-tests/ndnsf-di-protected-grant.t.cpp`。验收：unit
  （C++ 负例：保护纪元赋值 → 明确 unavailable 错误，状态不进
  GrantVerified）；integration（native provider 真实保护纪元投影被
  拒）。吸收关系：T002 落地后由真实 grant 验证取代该失败关闭路径。

- **R003 Evidence Invalidation Inventory**（PASS，document inventory only；T007 剩余 A05 未闭合）。审计 Spec 180/181 全部
  证据文件：任何声称 PASS 但被后续修订失效的文件必须带失效横幅
  （t016/s1 已确认有；核查其余）；每个证据文件头部必须声明证据层
  （implemented/wired/executed/measured）。验收：完整清单 + 每文件
  层声明；当前修订不修改 Spec180 冻结文件，在本 Spec 的完整清单中
  记录其失效范围与替代证据。原 R003 记录的历史修改不在本轮重做。

- **R004 Protected-case Admission Guard**（absorbed；T001/T002 生产验收已完成，以下保留先行门原要求）。runner 的 Y-B 保护纪元子用例
  在 grant 接线（T001/T002）完成前必须失败关闭
  （`DI_PROTECTED_GRANT_UNAVAILABLE`），不得以明文路径冒充保护纪元
  执行、不得产出 PASS 记录。文件：
  `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`、
  `tests/python/test_spec181_runner_guard.py`。验收：unit（门禁存在）；
  integration（未接线时保护纪元用例被拒且原因明确）。吸收关系：
  T001/T002 落地后门禁转为正常执行。

## Phase 1: Protected Artifact Execution (Priority: P1)

- [x] T001 [US1] **Python Grant Publication and Consumption**。`provider.py`
  装配入口（`_assemble_certified_role_execution` 之前）在
  `protection_epoch != "plaintext-v1"` 时：按规范名**精确获取** grant
  Data（使用既有精确名 Data 获取原语，禁止 `ValidatorNull`）→
  `verify_and_unwrap_grant`（权威签名、绑定、过期）→
  `PlaintextLeaseRegistry` 注册内容密钥 → 装配 → 清理零化；任何校验
  失败以 `DI_PROTECTED_GRANT_REJECTED` 失败关闭（与 native 错误码
  家族统一）。请求方侧：`AuthorityBackedGrantProvider`（已实现，
  Spec 180 提交 `d36438c2`）进程内签发后，grant Data 经既有
  `ServiceUser.publish_signed_app_data` 路径发布，Provider 按同一
  规范名获取。权威私钥经注册表 `artifactPolicyAuthority` 条目加载
  （`~/.config/ndnsf/spec180/`，mode 0600）。**内容密钥真实消费
  （FR-013）**：装配产物按 `DISK_CIPHERTEXT_ASSEMBLED` 语义用派生密钥
  （`K_bundle = HKDF(content_key, ...)` → `K_entry = HKDF(K_bundle,
  entryKind)`，AES-256-GCM）加密暂存于工作目录；加载路径用解包出的
  内容密钥解密，明文分配注册进 `PlaintextLeaseRegistry` 并在清理/
  失败时零化。文件：
  `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`、
  `security/grant_provider.py`（复用）、
  `core/protected_artifacts.py`（复用；如需 AEAD 派生辅助在此新增）、
  `tests/python/test_spec181_provider_grant.py`。验收：unit（绑定/过期/
  跨绑定负例，复用 Spec 180 的 29 个编码测试为回归基线；AEAD 派生
  与加密/解密往返）；integration（真实 requester 进程内签发并经真实
  发布路径发布 →
  真实 Provider 进程精确名获取与解包：正确 grant 装配成功且密文
  暂存/解密加载/零化完整；错误收件人 grant 在 verifier 内被拒且无
  明文落地；**错误内容密钥或篡改密文在 AEAD 认证层以
  `DI_PROTECTED_GRANT_REJECTED` 拒绝**）；MiniNDN 由 T008 的
  Y-B 保护纪元子用例覆盖。证据
  `evidence/t001-python-provider-grant-current.md`。当前新增验收：授权先于
  `_assemble_certified_role_execution` 的明文/ORT 加载；签名有效但摘要
  不等于 Selection grant 引用时拒绝（定向回归已关闭）；Merge 模型
  预期值来自独立认证输入；所有 `.weights` 以 `EXTERNAL_DATA` 加密并
  登记租约，取消/失败时同样清理；核对 issuer 身份与注册表一致。
  必须新增真实进程测试 `tests/python/test_spec181_provider_grant_integration.py`，
  现有注入 fetch 的测试不满足 integration。

- [x] T002 [US1] **Native Provider Grant Runtime**。
  `NativeProviderHandler`/`ProtectedRuntime` 增加规范名精确获取、
  权威签名/绑定/过期校验与 KeyChain 解包（信封算法按收件人密钥
  类型：Ed25519 → X25519 转换、EC → ECDH-P256，与 Python 信封
  `alg` 字段一致）；绑定失败维持现有
  `DI_PROTECTED_RUNTIME_BINDING_MISMATCH` 语义并新增
  `DI_PROTECTED_GRANT_REJECTED`。pybind 暴露获取/解包所需的最小面。
  文件：`cpp/ndnsf-di/NativeProviderHandler.cpp`、
  `cpp/ndnsf-di/ProtectedRuntime.{hpp,cpp}`、
  `pythonWrapper/src/ndnsf/_ndnsf.cpp`、
  `tests/integration-tests/ndnsf-di-protected-grant.t.cpp`。验收：unit
  （C++ 校验负例：错误权威/收件人/绑定/过期）；integration
  （Python 端到端 native provider 真实解包，grant 由真实请求方进程
  发布）。证据 `evidence/t002-native-provider-grant-current.md`。
  FR-015 追加验收：复用现有公共准备/执行路径；native factory 的
  证据和资源初始化只保留一个 owner，模型分支只构造 runner spec；
  native YOLO 后处理计算归 adapter，并保持现有公开调用兼容。
  定向回归覆盖公共边界与受影响的已有生成接口，不能用复制 Qwen
  Provider 或新建平行运行时实现。见
  `evidence/shared-runtime-reuse-20260905.md`。

- [x] T003 [US1] **Grant and Assembly Parity Vectors**。固定向量文件
  `tests/fixtures/spec181/grant-vectors-v1.json`：同一 grant 字节
  （规范 JSON）分别由 Python 与 native 解包，必须得到同一内容密钥；
  向量含正例与全部负例（错误收件人、跨请求/attempt/core/model/纪元、
  过期、伪造签名）。文件：
  `tests/python/test_spec181_native_grant_parity.py`（消费 T002 的
  pybind 面）。验收：integration（双侧一致断言；向量文件随任何编码
  变更必须同步更新并重新双侧验证）。证据
  `evidence/t003-grant-parity-current.md`。FR-012 另需固定
  `tests/fixtures/spec181/assembly-vectors-v1.json` 与
  `tests/python/test_spec181_assembly_parity.py`：同一 canonical ONNX、
  recipe、external-data 与 backend ABI，分别消费 Python
  `assemble_certified_onnx_model`、native `NativeCanonicalOnnxAssembler`，
  逐字节及摘要一致；变异 recipe/initializer 必须拒绝。grant 的 9 个
  向量不能关闭该装配验收。
  定向验收已通过：Waf `spec181-assembly-parity` 调用真实 C++ 入口
  与正常 subprocess helper；8 个装配向量双侧验证 + 3 项 grant 检查
  共 19 PASS，见 [装配验收](evidence/t003-assembly-parity-20260905.md)。

- [x] T004 [US1] **Runtime Readiness and Cancellation**。`pythonWrapper/ndnsf/service.py`
  的 `start()`/`start_background()` 就绪等待改为 15000 ms（对 Core
  10 s 探针留余量）；`ServiceController.cpp` 的探针循环在
  `stop()`/取消后不得热转（取消检查 + io 停止后立即退出）。文件：
  `pythonWrapper/ndnsf/service.py`、
  `pythonWrapper/src/ndnsf/_ndnsf.cpp`、
  `ndn-service-framework/ServiceController.cpp`、
  `tests/python/test_spec181_controller_readiness.py`、
  `tests/fixtures/spec181/controller-lifecycle.py`、
  `tests/standalone/run-spec181-controller-lifecycle.py`。验收：unit（取消/
  停止路径）；integration（真实 controller 进程：超时余量下边界成功
  不被误报；取消路径及时退出且无热转）。证据
  `evidence/t004-readiness-boundary-current.md` 与
  `evidence/t004-lifecycle-acceptance-20260905.md`。

## Phase 2: Registered Negative Outcomes (Priority: P1)

- [ ] T005 [US2] **Y-N Matrix Qualification [MiniNDN]**。在 MiniNDN 小模型
  CPU 上按注册语义重跑七子用例：Y-N-O（目录序无关）、Y-N-C（双候选
  不可行）、Y-N-P（ACK 签名/来源/绑定篡改）、Y-N-R（组件角色非法
  区间）、Y-N-I（非 ingress 获取，`DI_INPUT_FETCH_ROLE_MISMATCH`）、
  Y-N-E（真实 grant 变异，由 T006 构造）、Y-N-L（明文字段注入，
  redaction 违规检测）。每个子用例断言注册拒绝原因 + 边界位置；
  全量子进程退出与清理；无关失败不得充当预期结果。文件：
  `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`（语义判定已在 Spec
  180 修复，本任务消费并重跑）、
  `tests/python/test_spec181_y_n_matrix.py`（矩阵参数化 + 原因断言）。
  验收：[MiniNDN] 七子用例全部预期结果、零未收集存活进程；证据
  `evidence/t005-y-n-matrix-current.md` 记录每子用例的拒绝原因与
  边界。Y-N-O 是 terminal control，不要求负拒绝码。
  `scripts/run_spec181_y_n_matrix_retry.py` 已停用：旧入口 exit 2，
  不删除目录、不启动进程、不生成结果。没有已验证的启动前故障
  分类器，不保留自动重试。唯一矩阵入口为维护 runner，首个失败
  立即停止、保留原始证据；诊断与 failure index 更新后才可使用新
  run-id 重跑。正式矩阵需保留全部失败与源/构建/配置摘要，在
  T007 PASS 后执行；旧自有 retry schema 不具备资格效力。

- [x] T006 [US2] **Production Grant Mutation Rejection**。构造三种真实 grant
  变异（过期、错误收件人、伪造权威签名）供 T005 的 Y-N-E 子用例
  使用，断言已实现 verifier 在授权边界以
  `DI_PROTECTED_GRANT_REJECTED` 拒绝；删除合成纪元异常路径。
  撤销仍是另一分支的延期项，不以过期或跨纪元拒绝冒充撤销。文件：
  `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` 的 Y-N-E 子用例、
  `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`、
  `security/grant_mutations.py`、`cpp/ndnsf-di/ProtectedRuntime.cpp`、
  `tests/python/test_spec181_y_n_e.py`、
  `tests/python/test_spec181_production_grant_mutations.py`。
  验收：unit（三种变异构造）；
  integration（每种变异经真实发布/获取到达选定 Provider verifier，
  在装配前拒绝；绑定 request/attempt/provider，正向控制必须成功；
  无关 ValueError、未配置、超时或错误生命周期不得算预期拒绝）。证据
  `evidence/t006-y-n-e-grant-mutation-current.md` 与
  `evidence/t006-production-repair-20260905.md`。

## Phase 3: Convergence and Local Qualification (Priority: P2)

- [ ] T007 [US3] **Design-code Convergence Audit**。按 12 审计原则对真实生产链
  （进程内权威、grant 解包双侧、装配、runner、候选工具链）做
  code-aware 审计，四层证据分离；BLOCK 项修复 + focused 回归 +
  重新审计至 PASS。文件：本目录 `audit.md`、`traceability.md`、
  `evidence/post-implementation-audit.md`。验收：审计 PASS 且每个
  发现附 file:line 证据与关闭回归；四层声明在每个证据文件头部。
  T007 不等待 T005/T008 的正式资格结果；其审计 PASS 是这些执行的
  前置条件。R003 的完整证据清单与临时诊断路径删除条件由本任务核查。
  FR-015：附 YOLO/Qwen 共用路径与差异 owner 映射，核查普通角色、
  缓存和可选生成 epoch 的公共授权/截止/清理边界；共享修复附
  受影响的 stateless/stateful 接口定向回归。Qwen 模型资格仍在范围外。

- [ ] T008 [US3] **Local Qualification [MiniNDN]**。从 T007 PASS 的同一源
  身份（提交哈希）执行：单元/集成选择器清单（local-suite inventory，
  逐项子进程监督）+ MiniNDN Y-A（原子候选、1 Provider）、Y-B
  （共享骨架、4 Provider、含 T001/T002 的保护纪元 grant 往返）、
  Y-N 全矩阵（T005 同源重跑）；CPU 后端证据记录，不得呈现为 GPU
  证据。文件：`scripts/spec180_inventory.py`、`scripts/run_spec180_local_gate.py`（路径沿用，所有权
  移交本任务）、`evidence/local-qualification.md`。验收：清单完整、
  Y-A/Y-B/Y-N 全通过、零未收集存活进程、CPU 后端声明。

## Phase 4: Candidate, SIF and Tiger (Priority: P3)

- [ ] T009 [US4] **S1 Candidate Seal**。以提交哈希封印候选：源、契约、
  注册表（含 `artifactPolicyAuthority` 公钥摘要）、模型工件摘要、
  oracle、runner、测试清单与 parity 向量文件摘要。文件：
  `scripts/spec180_candidate.py`（沿用）。验收：unit（脏树/跨候选
  证据拒绝）；封印记录含提交哈希与全部平面摘要。证据
  `evidence/t009-candidate-seal-current.md`。

- [ ] T010 [US4] **S4 Exact-SIF Y-B Replay**。在 T008
  通过后的同一源上构建本地 SIF（含 rev-123 就绪修复后的运行时），
  执行 exact-SIF Y-B replay：无源码/包覆盖、NFD 与全部子进程在镜像
  内、终端结果与 oracle 一致。验收：integration（SIF 边界验证 +
  replay 结果摘要）；证据 `evidence/t010-exact-sif-replay-current.md`。

- [ ] T011 [US4] **S5 Single Tiger Y-B Submission**。通过 host-NFD node-local
  launcher（Spec 180 修订 122 暴露的设计缺口：Tiger 节点无 MiniNDN
  基底）提交一次 `yolo-functional`：一节点、一 RTX、四 Provider
  进程、一次 cold Y-B、三模型角色 CUDA 证据 + Merge CPU；无参数
  漂移的 byte-identical 重提仅限记录的 Slurm/host 入口前失败一次。
  文件：Spec 180 的 `packaging/ndnsf-di-container/jobs/spec180/*` 与
  `scripts/` 工具链（路径沿用）。验收：一次提交的完整结构化证据
  （协议/数值/设备/子进程退出/清理 oracle 全通过）；证据
  `evidence/t011-tiger-submission-current.md`。

- [ ] T012 [US4] **Final Closure Record**。在单一候选身份下映射全部 FR 到代码、
  三层测试与结构化证据，发出唯一功能裁决；声称边界语言审计（无
  Qwen/多 GPU/性能声称）。文件：
  `evidence/closure-record.md`（唯一裁决来源）。验收：closure 记录
  中每个 FR 有映射行、每个 oracle 有摘要与 schema、无历史 PASS
  复用。

## Dependencies & Execution Order

```text
R001/R002/R003/R004（历史 safeguard；对应任务持续定向回归）
T001 -> T002 -> T003 -> T006
T004 为独立修复，但同样是 T007 的前提
T001/T002/T003/T004/T006 -> T007 PASS -> T005 -> T008
T008 -> T009 -> T010 -> T011 -> T012
```

R0 的失败关闭语义在定向修复期间保持；不得因 helper 存在或常量翻转
声称生产完成。只有第一个未关门是活动门；T007 未 PASS 前不运行完整
矩阵或资格套件。G3/G4 昂贵动作在 G0--G2 全通过前禁止。
任何行为影响面变更使下游证据失效并回到最早失效门。
