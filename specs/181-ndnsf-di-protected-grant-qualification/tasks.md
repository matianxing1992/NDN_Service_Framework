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

## 三层测试标准（本 spec 的规范性要求）

每个实现任务的验收必须同时满足适用层，缺一不可：

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

## Phase 1: User Story 1 — 受保护工件授权正路径（Priority: P1）

- [ ] **T001 [US1] Python Provider 解包接入与 grant 发布**。`provider.py`
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
  `DI_PROTECTED_GRANT_REJECTED` 拒绝**）；MiniNDN 由 T005 的
  Y-B 保护纪元子用例覆盖。证据
  `evidence/t001-python-provider-grant-current.md`。

- [ ] **T002 [US1] native Provider 解包**。
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

- [ ] **T003 [US1] 跨语言 parity 向量锁定**。固定向量文件
  `tests/fixtures/spec181/grant-vectors-v1.json`：同一 grant 字节
  （规范 JSON）分别由 Python 与 native 解包，必须得到同一内容密钥；
  向量含正例与全部负例（错误收件人、跨请求/attempt/core/model/纪元、
  过期、伪造签名）。文件：
  `tests/python/test_spec181_native_grant_parity.py`（消费 T002 的
  pybind 面）。验收：integration（双侧一致断言；向量文件随任何编码
  变更必须同步更新并重新双侧验证）。证据
  `evidence/t003-grant-parity-current.md`。

- [ ] **T004 [US1] 运行时就绪边界修复**。`pythonWrapper/ndnsf/service.py`
  的 `start()`/`start_background()` 就绪等待改为 15000 ms（对 Core
  10 s 探针留余量）；`ServiceController.cpp` 的探针循环在
  `stop()`/取消后不得热转（取消检查 + io 停止后立即退出）。文件：
  `pythonWrapper/ndnsf/service.py`、
  `ndn-service-framework/ServiceController.cpp`、
  `tests/python/test_spec181_controller_readiness.py`。验收：unit（取消/
  停止路径）；integration（真实 controller 进程：超时余量下边界成功
  不被误报；取消路径及时退出且无热转）。证据
  `evidence/t004-readiness-boundary-current.md`。

## Phase 2: User Story 2 — 负面矩阵语义化（Priority: P1）

- [ ] **T005 [US2] [MiniNDN] Y-N 全矩阵语义重跑**。在 MiniNDN 小模型
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
  边界。

- [ ] **T006 [US2] Y-N-E 真实 grant 变异构造**。构造三种真实 grant
  变异（过期、错误收件人、伪造权威签名）供 T005 的 Y-N-E 子用例
  使用，断言已实现 verifier 在授权边界以
  `DI_PROTECTED_GRANT_REJECTED` 拒绝；删除合成纪元异常路径。被撤销
  纪元的拒绝以保护纪元绑定失败表达（撤销子系统属另一分支，见
  spec.md Out of Scope）。文件：
  `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` 的 Y-N-E 子用例、
  `tests/python/test_spec181_y_n_e.py`。验收：unit（三种变异构造）；
  integration（每种变异到达真实 verifier 并在装配前被拒）。证据
  `evidence/t006-y-n-e-grant-mutation-current.md`。

## Phase 3: User Story 3 — 收敛与本地资格（Priority: P2）

- [ ] **T007 [US3] 设计-代码收敛审计**。按 12 审计原则对真实生产链
  （进程内权威、grant 解包双侧、装配、runner、候选工具链）做
  code-aware 审计，四层证据分离；BLOCK 项修复 + focused 回归 +
  重新审计至 PASS。文件：本目录 `audit.md`、`traceability.md`、
  `evidence/post-implementation-audit.md`。验收：审计 PASS 且每个
  发现附 file:line 证据与关闭回归；四层声明在每个证据文件头部。

- [ ] **T008 [US3] [MiniNDN] 本地资格认证**。从 T007 PASS 的同一源
  身份（提交哈希）执行：单元/集成选择器清单（local-suite inventory，
  逐项子进程监督）+ MiniNDN Y-A（原子候选、1 Provider）、Y-B
  （共享骨架、4 Provider、含 T001/T002 的保护纪元 grant 往返）、
  Y-N 全矩阵（T005 同源重跑）；CPU 后端证据记录，不得呈现为 GPU
  证据。文件：`scripts/run_spec180_local_gate.py`（路径沿用，所有权
  移交本任务）、`evidence/local-qualification.md`。验收：清单完整、
  Y-A/Y-B/Y-N 全通过、零未收集存活进程、CPU 后端声明。

## Phase 4: User Story 4 — 候选、SIF 与 Tiger（Priority: P3）

- [ ] **T009 [US4] S1 候选封印**。以提交哈希封印候选：源、契约、
  注册表（含 `artifactPolicyAuthority` 公钥摘要）、模型工件摘要、
  oracle、runner、测试清单与 parity 向量文件摘要。文件：
  `scripts/spec180_candidate.py`（沿用）。验收：unit（脏树/跨候选
  证据拒绝）；封印记录含提交哈希与全部平面摘要。证据
  `evidence/t009-candidate-seal-current.md`。

- [ ] **T010 [US4] S4 本地 SIF + exact-SIF Y-B replay**。在 T008
  通过后的同一源上构建本地 SIF（含 rev-123 就绪修复后的运行时），
  执行 exact-SIF Y-B replay：无源码/包覆盖、NFD 与全部子进程在镜像
  内、终端结果与 oracle 一致。验收：integration（SIF 边界验证 +
  replay 结果摘要）；证据 `evidence/t010-exact-sif-replay-current.md`。

- [ ] **T011 [US4] S5 一次 Tiger Y-B 提交**。通过 host-NFD node-local
  launcher（Spec 180 修订 122 暴露的设计缺口：Tiger 节点无 MiniNDN
  基底）提交一次 `yolo-functional`：一节点、一 RTX、四 Provider
  进程、一次 cold Y-B、三模型角色 CUDA 证据 + Merge CPU；无参数
  漂移的 byte-identical 重提仅限记录的 Slurm/host 入口前失败一次。
  文件：Spec 180 的 `packaging/ndnsf-di-container/jobs/spec180/*` 与
  `scripts/` 工具链（路径沿用）。验收：一次提交的完整结构化证据
  （协议/数值/设备/子进程退出/清理 oracle 全通过）；证据
  `evidence/t011-tiger-submission-current.md`。

- [ ] **T012 [US4] 终局记录**。在单一候选身份下映射全部 FR 到代码、
  三层测试与结构化证据，发出唯一功能裁决；声称边界语言审计（无
  Qwen/多 GPU/性能声称）。文件：
  `evidence/closure-record.md`（唯一裁决来源）。验收：closure 记录
  中每个 FR 有映射行、每个 oracle 有摘要与 schema、无历史 PASS
  复用。

## Dependencies & Execution Order

```text
T001 -> T002 -> T003 -> T006 -> T005
T004 可与 T001 并行 [P]
T001/T002/T003/T005/T006 -> T007 -> T008
T008 -> T009 -> T010 -> T011 -> T012
```

只有第一个未关门是活动门；G3/G4 的昂贵动作在 G0--G2 全通过前禁止。
任何行为影响面变更使下游证据失效并回到最早失效门。
