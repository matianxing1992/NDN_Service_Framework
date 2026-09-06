# Feature Specification: NDNSF-DI Protected-Grant Local Development and Delivery

**Feature Branch**: `Experimental`

**Feature Directory**: `181-ndnsf-di-protected-grant-qualification`

**Created**: 2026-09-05

**Status**: `IN_PROGRESS`（修订 7：本机开发、本地验证与版本交付；SIF/Tiger 移交实验机器。G0 与 G1/T007 已完成，当前进入 G2/T005；
Spec 180 已按所有者决定关闭，其契约、冻结证据与失效声明保持权威。）

**Input**: Spec 180 修订 125 的迁移清单；Spec 170
`contracts/artifact-assembly-v1.md`（受保护工件授权契约）；Spec 180
`contracts/`（目录信任、runner 契约、信任根注册表，全部原样继承）。

## Goal

R18 共享精确传输修复采用 [Exact Tensor Wire Representation](contracts/exact-tensor-wire.md)
的紧凑 wire 契约；逻辑签名、绑定、内容及旧格式验证保持权威。

Spec 180 的教训是：范围反复扩张、审计循环替代执行、负裁决假 PASS、
seam-only 证据。本 spec 只做一件事——把 180 遗留的实现与本地验证工作
按行为收口，并且给每一个任务强制三层测试标准（单元 + 集成 + MiniNDN
小模型 CPU），其中集成测试必须设计真实用例、走真实生产调用链，禁止
"看起来通过"。

完成目标只有一个：**一个明确提交哈希的开发版本在
MiniNDN 本地小模型 CPU 上通过 Y-A/Y-B/Y-N 全矩阵（含真实受保护工件
grant 授权路径），通过设计-代码收敛审计与本地资格认证，交付可复现的
源码、配置/依赖身份、验证证据与实验交接说明**。

2026-09-06 所有者明确采用长期开发/实验分工：本机负责代码开发、
单元/集成验证及本地 MiniNDN；另一台机器接收明确 commit，负责 SIF
构建、TigerCluster 脚本/配置、实验执行与反馈。Qwen/YOLO 不构成长
期责任边界。原 T010/T011 标为 TRANSFERRED，保留外部验收责任，
不计本机完成率、不作为本 Spec 关闭条件。交接契约见
[Development and Experiment Handoff](handoff-contract.md)。

Git 合并按所有者最新要求留到当前开发完成后另行讨论；本 Spec 不
执行分支合并，也不以合并或实验机器接收回执作为本地关闭前置条件。

不改变的技术路线：ACK 驱动规划、V3 选择投影、受保护工件授权契约、
native Provider 运行时边界全部继承 180 的实现与契约；实验机器后续
继承原有 SIF/Tiger 机制与约束，冻结的 Spec180 文档保持不变。
本 spec 不引入新协议、新放置策略、新模型或新范围。

### Shared Runtime Boundary

YOLO 与 Qwen 案例使用同一套服务、规划/Selection、授权、装配、
依赖传输、角色调度、执行证据和清理机制。已有 Qwen 实现中的公共
机制须直接复用；图/切分规则、任务编码、模型计算及状态表示由
现有 adapter/runner 接口承载。生成的多轮调度是可选能力，无状态
YOLO 不承担 tokenizer、KV 或会话保留职责。详见
[共享路径核对](evidence/shared-runtime-reuse-20260905.md)。

## User Scenarios & Testing

### User Story 1 - Protected Artifact Execution (Priority: P1)

请求方获得签名且面向选定 Provider 加密的 grant，Python 与 native
Provider 在装配/加载前完成授权，并在成功、取消及失败时清理全部秘密。
独立验收：真实发布与精确名获取、正负授权及装配字节 parity；对应 T001--T004。

### User Story 2 - Registered Negative Outcomes (Priority: P1)

操作者能将真实授权拒绝与启动、网络、收集器故障区分。Y-N-E 变异必须
到达请求选中的 Provider；进程内 verifier probe 仅为 unit 证据。
独立验收：T006 定向生产链变异与 T005 同源七子用例矩阵。

### User Story 3 - Auditable Local Qualification (Priority: P2)

操作者先获得当前源与有效配置的收敛审计 PASS，再执行受监督的本地
资格套件。审计验收不依赖随后产生的资格结果；对应 T007、T008。

### User Story 4 - Reproducible Development Delivery (Priority: P3)

开发者交付同一源、模型、配置与本地验证身份的明确 commit，提供
实验机器可使用的构建/复现说明与反馈契约；最后只发出本地开发裁决。
对应 T009/T012；SIF/replay/Tiger 结果由移交的 T010/T011 单独负责。

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001** — **Inherited Authorization Contract**。受保护工件授权沿用 Spec170
  `artifact-assembly-v1` 的 KeyGrantV1 流程（seal core → 签名请求 →
  策略校验 → 接收者加密 grant → finalize → Provider 解包）。已落地的
  规范编码与真实权威（提交 `d36438c2`）是本 spec 的起点；HMAC/`repr`
  脚手架不得作为任何资格输入，`plaintext-v1` 不得充当保护纪元。
- **FR-002** — **In-process Authority and Publication**。功能切片内授权的策略
  权威运行在请求方（user）进程中：加载注册表
  `artifactPolicyAuthority` 条目的私钥（git 外，mode 0600），校验
  请求方签名、模型/纪元策略、Provider 身份与封存 core/grant-view
  摘要，签发规范名 grant Data 并经既有
  `ServiceUser.publish_signed_app_data` 路径发布，供 Provider 按规范名
  精确获取。本切片不新增网络服务前缀；独立权威服务（生产形态的信任
  域分离）是延期项（见 Out of Scope）。
- **FR-003** — **Provider Grant Verification Parity**。Python Provider 装配路径与
  native `ProtectedRuntime` 必须按规范名精确获取 grant，校验权威签名、
  全部绑定字段与过期，解包内容密钥，注册明文租约并在清理时零化。
  错误收件人、跨请求/attempt/core/model/纪元绑定或过期的 grant 必须
  在认证层失败关闭（Python 与 native 行为一致，用同一跨语言向量
  验证）。撤销校验由所有者的另一分支负责（见 Out of Scope）。
  grant payload 摘要必须匹配 Selection 的 `GrantBindingV1.grant_digest`；
  grant 自述字段不得替代独立封印的模型、策略或身份预期值。
- **FR-004** — **Production Grant Mutation**。Y-N-E 子用例必须让一个真实的 grant
  变异（过期、错误收件人或伪造签名）到达已实现的 verifier 并断言
  其在选定 Provider 的授权边界被拒；合成纪元异常或 User 进程内
  verifier probe 不得充当该证据。过期 grant 与跨纪元绑定是独立负例，
  均不代表已实现撤销。
- **FR-005** — **Three-layer Validation**。每个实现任务必须交付：① 单元测试
  （focused red/green + 变异）；② 集成测试（真实生产调用链，见
  tasks.md 的集成测试设计标准）；③ 适用任务的 MiniNDN 小模型 CPU
  测试。集成测试的每个用例必须命名其覆盖的生产入口、注册的拒绝原因
  与边界位置；无关失败、错误生命周期相位、内部策略异常不得充当
  预期结果。禁止 seam-only、mock 替换被测生产链、标签 PASS 的证据。
- **FR-006** — **Local Qualification**。从同一个源身份（提交哈希）出发，
  MiniNDN 小模型 CPU 必须执行 Y-A、Y-B 与 Y-N 全矩阵（Y-N-O/C/P/R/I/E/L）
  直至终端 Response 或注册边界拒绝，全量子进程退出状态收集、零泄漏
  明文/密钥能力、清理完整。
- **FR-007** — **Design-code Convergence Audit**。正式资格认证前必须通过一次
  code-aware 收敛审计（12 原则、四层证据分离：文档声称/代码实现/
  测试执行/实验测量）；BLOCK 项修复并回归后才允许后续门。
- **FR-008** — **Immutable Development Delivery**。交付清单绑定已验证的
  提交哈希、契约、注册表、权威公钥摘要、模型工件、oracle、测试清单、
  本地依赖/构建/有效配置与证据摘要。拒绝未记录的源码漂移和跨版本
  PASS；SIF 构建输入/镜像身份由实验机器后续封印，不在本机伪造。
- **FR-009** — **Experiment Handoff and Feedback**。交接记录明确实验机器
  负责 SIF、exact-SIF replay、TigerCluster 脚本/配置与实验；提供
  源 commit、复现命令、工件获取方式、已知限制与原 T010/T011 的
  验收要求。反馈须携带实际 commit、配置、run-id、日志/结果及复现
  条件；实验脚本变更以提交返回同一代码库。本 Spec 只验收交接材料
  可用，不要求远端运行、接收回执或实验 PASS。
- **FR-010** — **Claim Boundary**。最终报告不得声称 Qwen、多 GPU、吞吐、
  延迟、扩展或性能结论；不得复用任何 Spec 175/180 历史 PASS 作
  本 spec 证据。`LOCAL_DEVELOPMENT_PASS` 仅为本地开发/验证交付裁决，
  不代表 SIF、Tiger 或 GPU 资格；TRANSFERRED 不等于 PASS。
- **FR-011** — **Readiness Boundary**。Python `start()`/`start_background()`
  的就绪等待必须大于 Core 探针 deadline（建议 15000 ms 对 10 s）；
  `stop()` 在探针进行中不得热转事件循环至 deadline。
- **FR-012** — **Assembly Byte Parity**。Python `assemble_certified_onnx_model`
  与 native `NativeCanonicalOnnxAssembler` 对同一 canonical ONNX + 同一
  recipe 必须产出相同装配字节摘要；该 parity 用固定向量测试锁定。
- **FR-014** — **Fail-closed Evidence Boundary**。修正任务（tasks.md Phase 0，R001--R004）
  确立的约束在实现与定向修复期间持续有效：真实机制接线前，相关路径必须失败关闭或
  报告 `UNAVAILABLE`，禁止合成拒绝充当注册拒绝原因、禁止状态冒充
  （如仅凭绑定比对进入 `GRANT_VERIFIED`）、禁止明文路径冒充保护纪元
  执行。对应生产验收闭合后才吸收临时门；允许为已命名缺口进行定向修复。
- **FR-013** — **Content-key Consumption and Cleanup**。解包出的内容密钥必须被真实的
  密码学操作消费，禁止"验证授权但密钥闲置"的授权剧场。功能切片内
  的最小真实消费：装配产物按 Spec170 契约的 `DISK_CIPHERTEXT_ASSEMBLED`
  语义 AEAD 加密暂存于 Provider 工作目录——
  `K_bundle = HKDF(epochContentKey, "NDNSF-DI/assembled/v1" || modelManifestDigest || roleAssemblySpecDigest || storageProfileDigest)`，
  `K_entry = HKDF(K_bundle, entryKind)`——加载路径用解包出的内容密钥
  解密（AES-256-GCM），明文分配注册进 `PlaintextLeaseRegistry` 并在
  清理/失败时零化。错误内容密钥或篡改的密文在 AEAD 认证层失败并
  以 `DI_PROTECTED_GRANT_REJECTED` 关闭。本切片内保护纪元固定不
  轮换（单一 `spec180-yolo-protected-v1`）；纪元轮换机制是生产延期
  项（见 Out of Scope）。
  保护范围包含 `MODEL_PROTO` 与全部 ONNX external-data 工件；未加密、
  未登记租约的 `.weights` 副本不能作为受保护加载结果。授权校验必须
  先于装配器的明文加载/ORT 检查；清理不得删除共享 canonical 源。

- **FR-015** — **Shared Runtime and Adapter Reuse**。不得因 YOLO/Qwen
  案例不同而复制 Provider 主循环、grant/装配/传输协议或清理逻辑。
  native 公共准备与执行边界由同一 owner 维护，模型分支只提供
  adapter/runner spec；模型专属计算归 adapter。公共变更须以受影响
  的既有单次/生成接口定向回归证明兼容，实际模型资格仍限本 Spec
  的 YOLO 切片。T002 完成公共准备与 adapter 收口，T007 以调用链和
  边界回归验收，不能用目录迁移或空接口作为复用证明。

### Key Entities

- **Policy authority**：本切片签发 grant 的策略权威（身份/密钥/
  策略来自注册表）；撤销状态签发与检查属于延期子系统。
- **KeyGrantV1**：权威签名、Provider 身份加密的内容密钥授权
  （规范编码见 Spec 180 提交 `d36438c2`）。
- **GrantBindingV1**：进入最终 plan 的非秘密名称/摘要引用。
- **Development delivery**：源（提交哈希）、契约、注册表、本地运行时/
  配置、模型工件、oracle 与证据的单一交付身份；后续实验候选引用它。
- **Y-N matrix**：固定七子用例（Y-N-O/C/P/R/I/E/L）的注册语义矩阵。

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001** — **Protected Grant Round Trip**：grant 往返（权威→seam→Provider 解包）正例与全部负例
  （错误请求方/收件人/请求/attempt/core/model/纪元、过期、伪造签名、
  跨绑定）在单元、集成与 MiniNDN 三层全部通过；Y-N-E 真实变异拒绝。
- **SC-002** — **Production Boundary Assertions**：每个集成测试用例绑定生产入口 + 注册拒绝原因 + 边界
  位置；无关失败不得充当预期结果（Spec 180 假 PASS 教训的制度化）。
- **SC-003** — **Same-source Local Matrix**：一个源身份下 MiniNDN Y-A/Y-B/Y-N 全矩阵通过，全部子
  进程退出收集，零未收集存活进程。
- **SC-004** — **Audit and Inventory**：收敛审计 PASS + 本地资格认证清单（local-suite
  inventory）完整可复现。
- **SC-005** — **Reproducible Development Handoff**：一个交付清单绑定已验证
  commit 与所有本地输入/证据摘要，复现与交接材料通过本地完整性检查，
  明确实验 owner、移交验收和反馈字段；不依赖 SIF/Tiger 结果。
- **SC-006** — **Single Local Development Verdict**：终局记录在同一交付身份
  下映射全部活动 FR 到代码、测试或交接材料，发出唯一
  `LOCAL_DEVELOPMENT_PASS`；原 T010/T011 保持 TRANSFERRED。

## Assumptions

- Spec 180 的契约、信任根注册表与冻结证据保持权威；本 spec 只增不改。
- Spec 175 的 `LOCAL_FUNCTIONAL_PASS` 手交仍为正式资格前提（与 180
  相同）；YOLO 实现本身不等待它。
- MiniNDN 小模型 CPU 测试使用 Spec 180 的 canonical YOLO26n 包与
  640×640 注册输入（外部工件，内容寻址）。
- 实验机器负责后续平台资源与镜像构建；其资源可用性不阻塞本地开发
  关闭，模型工件的获取说明与摘要必须包含在交付中。

## Out of Scope

- **SIF/Tiger execution (TRANSFERRED)**：原 T010/T011 的构建、replay、
  部署脚本/配置与集群执行归实验机器；具体责任及原验收见交接契约。
  不执行远端操作，不用本地 PASS 代替实验结果。
- **Branch merge**：当前开发完成后另行讨论双方分支及未提交工作，
  不在本 Spec 执行 `UAV-Experimental` 合并。未来合并改变源码身份，
  必须按影响范围验证合并后的版本，不能直接继承旧提交资格。
- Qwen 模型执行资格、跨模型资格、多 GPU、性能/吞吐/延迟声称
  （延续 180 边界）。复用已有 Qwen 公共机制及其受影响接口的定向
  兼容性回归属于源码收口，不是新增模型资格。
- 新放置策略、新协议、跨 Provider 张量并行。
- 对 Spec 180 历史文档的回译或重写。
- **撤销子系统（延期项）**：`RevocationStateV1` 账本、网络撤销服务与
  grant 撤销校验由所有者在另一台机器的另一分支开发。本分支不实现
  撤销；grant 路径以过期为准，wire 编码保留 `revocationSequence`
  被动字段（固定为 1）以保证未来集成不改变规范字节。集成条件：所有
  者分支的撤销机制落地后，在 Provider 校验路径插入撤销检查并解除该
  延期标记。
- **独立权威服务（延期项）**：生产形态的策略权威必须与请求方进程
  分离（独立网络服务端、独立信任域）。本功能切片为 experiment-only
  信任集（Spec 180 修订 112：无生产 PKI 声称），权威与请求方同进程
  可接受。集成条件：任何生产部署前将权威拆分为独立服务并恢复
  FR-002 原网络服务端形态。
- **纪元轮换（延期项）**：本切片保护纪元固定为
  `spec180-yolo-protected-v1` 且不轮换。纪元轮换（rotation）、旧纪元
  拒收与缓存身份跨纪元失效的完整机制是生产延期项；本切片的绑定与
  缓存身份已携带纪元字段，轮换机制接入时不改变线编码。

## Revision History

- **7 (2026-09-06)**：所有者确认本机开发与本地验证、实验机器负责
  SIF/Tiger 的长期分工；改写 Goal、US4、FR-008/009/010、SC-005/006，
  T009/T012 负责交付与本地闭合，T010/T011 移交且不记为完成。
  本地 MiniNDN 与授权/清理/同源验证要求保留；Git 合并留待开发结束。
