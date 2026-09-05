# Feature Specification: NDNSF-DI Protected-Grant and Qualification Closure

**Feature Branch**: `Experimental`

**Feature Directory**: `181-ndnsf-di-protected-grant-qualification`

**Created**: 2026-09-05

**Status**: `PLANNED`（继承 Spec 180 的未完成实现与资格认证工作；
Spec 180 已按所有者决定关闭，其契约、冻结证据与失效声明保持权威。）

**Input**: Spec 180 修订 125 的迁移清单；Spec 170
`contracts/artifact-assembly-v1.md`（受保护工件授权契约）；Spec 180
`contracts/`（目录信任、runner 契约、信任根注册表，全部原样继承）。

## 中文叙述：本 spec 的目标

Spec 180 的教训是：范围反复扩张、审计循环替代执行、负裁决假 PASS、
seam-only 证据。本 spec 只做一件事——把 180 遗留的实现与资格认证工作
按行为收口，并且给每一个任务强制三层测试标准（单元 + 集成 + MiniNDN
小模型 CPU），其中集成测试必须设计真实用例、走真实生产调用链，禁止
"看起来通过"。

完成目标只有一个：**一个不可变 YOLO 候选（提交哈希封印）在
MiniNDN 本地小模型 CPU 上通过 Y-A/Y-B/Y-N 全矩阵（含真实受保护工件
grant 授权路径），通过设计-代码收敛审计与本地资格认证，随后一个
SIF 通过 exact-SIF replay，并在 Tiger 上完成一次 cold Y-B 请求**。

不改变的技术路线：ACK 驱动规划、V3 选择投影、受保护工件授权契约、
native Provider 运行时边界、Tiger 提交机制全部继承 180 的实现与契约；
本 spec 不引入新协议、新放置策略、新模型或新范围。

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001** — **继承的授权契约**。受保护工件授权沿用 Spec170
  `artifact-assembly-v1` 的 KeyGrantV1 流程（seal core → 签名请求 →
  策略校验 → 接收者加密 grant → finalize → Provider 解包）。已落地的
  规范编码与真实权威（提交 `d36438c2`）是本 spec 的起点；HMAC/`repr`
  脚手架不得作为任何资格输入，`plaintext-v1` 不得充当保护纪元。
- **FR-002** — **权威网络服务端**。授权的策略权威必须通过 NDN 服务
  `/<authority>/NDNSF-DI/KEY-GRANT/v1/...` 规范名下的签名 grant Data，
  并签发撤销状态 Data。身份、公钥摘要与信任规则来自 Spec 180 的
  `contracts/trust-root-registry-v1.json` 的 `artifactPolicyAuthority`
  条目；私钥在 git 外（`~/.config/ndnsf/spec180/`，mode 0600）。
  权威必须校验请求方签名、模型/纪元策略、Provider 身份与封存
  core/grant-view 摘要，失败即拒绝签发。
- **FR-003** — **Provider 双侧解包一致性**。Python Provider 装配路径与
  native `ProtectedRuntime` 必须按规范名精确获取 grant，校验权威签名、
  全部绑定字段与过期，解包内容密钥，注册明文租约并在清理时零化。
  错误收件人、跨请求/attempt/core/model/纪元绑定或过期的 grant 必须
  在认证层失败关闭（Python 与 native 行为一致，用同一跨语言向量
  验证）。撤销校验由所有者的另一分支负责（见 Out of Scope）。
- **FR-004** — **Y-N-E 真实变异**。Y-N-E 子用例必须让一个真实的 grant
  变异（过期、错误收件人或伪造签名）到达已实现的 verifier 并断言
  其在授权边界被拒；合成纪元异常不得充当该证据。被撤销/过期纪元
  的授权拒绝以保护纪元绑定失败表达。
- **FR-005** — **三层测试标准**。每个实现任务必须交付：① 单元测试
  （focused red/green + 变异）；② 集成测试（真实生产调用链，见
  tasks.md 的集成测试设计标准）；③ 适用任务的 MiniNDN 小模型 CPU
  测试。集成测试的每个用例必须命名其覆盖的生产入口、注册的拒绝原因
  与边界位置；无关失败、错误生命周期相位、内部策略异常不得充当
  预期结果。禁止 seam-only、mock 替换被测生产链、标签 PASS 的证据。
- **FR-006** — **本地资格认证**。从同一个源身份（提交哈希）出发，
  MiniNDN 小模型 CPU 必须执行 Y-A、Y-B 与 Y-N 全矩阵（Y-N-C/P/R/I/E/L）
  直至终端 Response 或注册边界拒绝，全量子进程退出状态收集、零泄漏
  明文/密钥能力、清理完整。
- **FR-007** — **设计-代码收敛审计**。正式资格认证前必须通过一次
  code-aware 收敛审计（12 原则、四层证据分离：文档声称/代码实现/
  测试执行/实验测量）；BLOCK 项修复并回归后才允许后续门。
- **FR-008** — **不可变候选与 SIF**。S1 候选封印绑定提交哈希、契约、
  注册表、权威公钥摘要、模型工件与 oracle；S4 的本地 SIF 哈希必须
  通过 exact-SIF Y-B replay（rev-123 就绪修复之后的运行时）。
- **FR-009** — **一次 Tiger 功能提交**。S5 通过 host-NFD node-local
  launcher 提交一次 `yolo-functional`（一节点、一 RTX、四 Provider
  进程、一次 cold Y-B），产出结构化终局证据。
- **FR-010** — **声称边界**。最终报告不得声称 Qwen、多 GPU、吞吐、
  延迟、扩展或性能结论；不得复用任何 Spec 175/180 历史 PASS 作
  本 spec 证据。
- **FR-011** — **就绪边界修复**。Python `start()`/`start_background()`
  的就绪等待必须大于 Core 探针 deadline（建议 15000 ms 对 10 s）；
  `stop()` 在探针进行中不得热转事件循环至 deadline。
- **FR-012** — **装配 parity**。Python `assemble_certified_onnx_model`
  与 native `NativeCanonicalOnnxAssembler` 对同一 canonical ONNX + 同一
  recipe 必须产出相同装配字节摘要；该 parity 用固定向量测试锁定。

### Key Entities

- **Policy authority**：签发 grant 与撤销状态的策略权威（身份/密钥/
  策略来自注册表）。
- **KeyGrantV1**：权威签名、Provider 身份加密的内容密钥授权
  （规范编码见 Spec 180 提交 `d36438c2`）。
- **GrantBindingV1**：进入最终 plan 的非秘密名称/摘要引用。
- **Qualification candidate**：源（提交哈希）、契约、注册表、运行时、
  模型工件、oracle 与证据模式的单一身份。
- **Y-N matrix**：固定七子用例（Y-N-O/C/P/R/I/E/L）的注册语义矩阵。

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**：grant 往返（权威→seam→Provider 解包）正例与全部负例
  （错误请求方/收件人/请求/attempt/core/model/纪元、过期、撤销、
  跨绑定）在单元、集成与 MiniNDN 三层全部通过；Y-N-E 真实变异拒绝。
- **SC-002**：每个集成测试用例绑定生产入口 + 注册拒绝原因 + 边界
  位置；无关失败不得充当预期结果（Spec 180 假 PASS 教训的制度化）。
- **SC-003**：一个源身份下 MiniNDN Y-A/Y-B/Y-N 全矩阵通过，全部子
  进程退出收集，零未收集存活进程。
- **SC-004**：收敛审计 PASS + 本地资格认证清单（local-suite
  inventory）完整可复现。
- **SC-005**：一个 SIF 哈希通过 exact-SIF Y-B replay 与 Tiger Y-B。
- **SC-006**：终局记录在单一候选身份下映射全部 FR 到代码、测试与
  结构化证据，且只发出一个功能裁决。

## Assumptions

- Spec 180 的契约、信任根注册表与冻结证据保持权威；本 spec 只增不改。
- Spec 175 的 `LOCAL_FUNCTIONAL_PASS` 手交仍为正式资格前提（与 180
  相同）；YOLO 实现本身不等待它。
- MiniNDN 小模型 CPU 测试使用 Spec 180 的 canonical YOLO26n 包与
  640×640 注册输入（外部工件，内容寻址）。
- Tiger 可分配一节点一 RTX；模型工件在 SIF 外按摘要验证。

## Out of Scope

- Qwen 执行、跨模型资格、多 GPU、性能/吞吐/延迟声称（延续 180 边界）。
- 新放置策略、新协议、跨 Provider 张量并行。
- 对 Spec 180 历史文档的回译或重写。
- **撤销子系统（延期项）**：`RevocationStateV1` 账本、网络撤销服务与
  grant 撤销校验由所有者在另一台机器的另一分支开发。本分支不实现
  撤销；grant 路径以过期为准，wire 编码保留 `revocationSequence`
  被动字段（固定为 1）以保证未来集成不改变规范字节。集成条件：所有
  者分支的撤销机制落地后，在 Provider 校验路径插入撤销检查并解除该
  延期标记。
