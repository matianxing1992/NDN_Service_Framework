# Feature Specification: Reusable TigerCluster YOLO Distributed Inference

**Feature Branch**: `TigerClusterExperiments`
**Created**: 2026-09-06
**Status**: IN_PROGRESS / Tiger runtime qualification OPEN
**Input**: 固定可复用的配置文件与实验脚本，在 TigerCluster 验证 NDNSF-DI + YOLO 分布式推理；Tiger 专用脚本和配置集中于 `Experiments/TigerCluster`。

## Scope And Evidence Boundary

交付一个人能直接使用、机器能验证的入口：选择一份配置，检查、准备、运行、收集；同一合格配置可在新 allocation 中重复使用。目标是正确性和复用，不是新推理算法、整个 DI 的 C++ 迁移或性能优势。Spec182 保持 NOT_STARTED。

交付入口 `81e251a4` 只表示 SOURCE_READY；四库运行源码由 `Experiments/TigerCluster/development-handoff.lock.json` 固定，其中 NDNSF 为 `447f7584`。旧 r119/base SIF、Local R8 FAIL、B003 未完成均不代表新组合通过。2026-09-10 更新：APP v33 + v22 base 已取得 exact-SIF Y-B/Y-N、host gate 和远端 local-cpu `NORMAL_EXPERIMENT_PASS`；真实 Tiger job `210316` 已通过 SIF/容量/socket/CUDA probe 和四 Provider startup，但 User 在 placement 前因 native V3 ACK 的 `resources:[]` 失败，尚无 Tiger YOLO 数值 PASS。native offer 已改为签名的 provider-owned CUDA `cudaMemGetInfo` 快照，APP-only 修复不重建未变化的 base SIF；新的 APP/gate/allocation 尚待刷新。

本 Spec 的资格判定仍按完整边界执行：transport、CUDA probe 或 Provider `READY` 只能作为组件证据；GPU Provider 的 ACK 还必须包含与其 `cuda:*` topology 对应的、签名且可用的 `free_memory_mb` 资源行。只有 User warmup/measured、独立 oracle、每角色 backend/GPU、退出码和清理全部闭合，才能记录 `SINGLE_NODE_GPU_PASS`。今天的逐 run 证据和固定执行顺序见 [Tiger deployment diagnosis](evidence/tiger-deployment-diagnosis-v69.md)。

最终必须使用两个真实 Tiger 计算节点，不同节点 Provider 计算同一次 YOLO 请求的不同阶段，通过 NDN 交换中间数据。多个节点各跑完整模型、只交换 echo、只出现 READY 或本机模拟节点，均不满足最终目标。

## User Scenarios & Testing *(mandatory)*

### User Story 1 - One Reusable Experiment Profile (Priority: P1)

操作者只选择一份配置；镜像、依赖、模型、身份规则、角色放置、参数和判定标准有唯一来源，不临时拼接 shell 环境。

**Why this priority**: 先在运行前发现环境不一致。
**Independent Test**: 无 SSH/Slurm 的单测环境生成确定性配置和执行计划；错误输入有准确原因和零外部副作用。
**Acceptance Scenarios**:
1. **Given** 固定输入，**When** 从仓库外 cwd 检查，**Then** 解析同样的路径/hash，列出实际 argv、挂载、资源和未完成门槛。
2. **Given** 错 hash、未知字段或旧 ABI 证据，**When** 提交，**Then** 在上传、staging、sbatch 前拒绝。
3. **Given** 配置变化，**When** 再检查，**Then** 指出必须重跑的最早阶段，拒绝复用失效 PASS。

### User Story 2 - Verified Local Runtime And Real Model (Priority: P1)

在实验机器完成当前源码 unit/integration/MiniNDN 验证，构建或复用基础库 SIF，独立构建外部应用包，确认真实 YOLO、应用入口和库加载一致。

**Why this priority**: Python import 或单个 native 目标通过不足以证明真实路径。
**Independent Test**: 小型真实 YOLO ONNX CPU 多进程执行请求、ACK、Selection、中间数据和最终数值输出；最终 SIF 再执行同一 CPU 诊断路径。
**Acceptance Scenarios**:
1. **Given** 固定四库源码，**When** 干净构建验证，**Then** 记录实际加载身份，两个 Python 扩展及应用入口可用。
2. **Given** 真实模型及独立完整模型 oracle，**When** 本机多 Provider 执行，**Then** 全阶段及数值标准通过，仅标为 LOCAL_CPU_PASS。
3. **Given** 中间 Data 缺失或错误权限，**When** 负例执行，**Then** 准确失败、无假成功、有限时间退出并清理。

### User Story 3 - Cross-Node GPU Inference (Priority: P1)

使用同一本地构建 SIF，在两台计算节点以四个独立 Provider 完成一次 YOLO 图的分布式执行。

**Why this priority**: 核心交付，前面检查为其前置条件。
**Independent Test**: 关联同一 request/plan 的角色、主机/GPU、跨节点依赖 Data 和最终数值响应，所有进程及清理结果一致。
**Acceptance Scenarios**:
1. **Given** 本地资格与 allocation 检查通过，**When** 发请求，**Then** BackboneNeck、DetectShard0、DetectShard1、Merge 完成，跨节点计算依赖真实交付，oracle 通过。
2. **Given** 缺 CUDA、错放置、路由不通或进程早退，**When** 执行，**Then** 拒绝或有界失败，不无声 CPU fallback、重放置或延长 deadline。

### User Story 4 - Repeat And Diagnose Without Reconfiguration (Priority: P2)

其他操作者只换 run ID 和合规输出位置就能复跑；失败记录指出首边界，不依赖聊天或个人插件。

**Why this priority**: 一次成功不能证明复用。
**Independent Test**: 同一配置、SIF、模型及脚本完成两个独立 allocation，第二次不改参数/脚本。
**Acceptance Scenarios**:
1. **Given** 合格配置，**When** 新 allocation 运行，**Then** 产生独立证据、全新角色私钥；大模型/SIF 无按时间戳重复拷贝。
2. **Given** 失败或提交状态未知，**When** 恢复，**Then** 保留原终态并查询 job，不盲目重复提交。

### Edge Cases

未知/未消费字段、布尔类型混淆、路径穿越/链接逃逸、重复 run ID；检查后篡改；旧 ABI、重复库和 C++/Python ORT 不一致；cwd、绑定、scratch、PIB、端口冲突；权限过期、角色丢失、激活缺失/篡改/迟到；GPU 不可见、同一主机冒充两节点；部分启动、signal、写盘失败、遗留进程。

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: **Canonical ownership.** Tiger 专用脚本、配置、schema、测试工具及操作文档 MUST 位于 `Experiments/TigerCluster`，工作分支 `TigerClusterExperiments`；复用 Core/DI/Repo 和现有 runtime，不复制业务实现。
- **FR-002**: **Single profile.** 版本化 profile MUST 固定全部行为选择并引用有 hash 的输入；启动仅允许 run ID、输出位置和经核对的物理路径。未知/未消费字段 MUST 拒绝。
- **FR-003**: **Immutable candidate.** MUST 绑定四库源码、工具链/依赖、SIF、脚本/harness、有效配置、模型/输入/oracle、安全规则和验证契约；区分交付文档与构建源码，按变更平面失效证据。
- **FR-004**: **Fail-before-side-effects.** MUST 提供分阶段可执行检查和零外部调用 mutation tests；坏输入在昂贵构建/上传/staging/sbatch 前拒绝。构建前不要求尚不存在的 SIF，提交前必须验证最终 SIF。
- **FR-005**: **Dependency closure.** MUST 验证基础库及外置应用的 C++/Python 闭包；最多 `-j4`、同一树只允许一次构建。应用原生产物由本地匹配 base 的容器/SDK 构建，允许清单内应用专属 `.so`，禁止携带宿主 venv/替代基础库。构建键不变时增量编译受影响 app 目标，基础 ABI/工具链改变时干净重建受影响消费者。
- **FR-006**: **Local SIF route.** MUST 在本地实验机器用匹配目标 compute 的 Apptainer 构建或复用合格基础运行库 SIF，NDNSF-DI/YOLO 与以后 UAV 应用以独立不可变只读包部署；Tiger 仅校验并执行同一 base+app 组合。应用变化不得强制重建未变的 SIF；旧镜像存在或小例子通过不算新 YOLO 组合合格。基础/应用归属与变更失效规则见 [runtime layers](../../Experiments/TigerCluster/docs/runtime-app-layers.md)。
- **FR-007**: **Real-path security and readiness.** MUST 验证独立角色身份、Controller 签名状态/权限、真实 socket/route 和服务路径；禁止 auth bypass 或固定 sleep 替代 readiness。
- **FR-008**: **Distributed graph.** MUST 执行 BackboneNeck → DetectShard0/DetectShard1 → Merge；四个独立 Provider 放在两真实节点，模型阶段有 CUDA 执行证据。Merge 显式 CPU 后处理不等于模型 CPU fallback。
- **FR-009**: **NDN data path.** MUST 用现有 NDNSF-DI/Repo 命名、安全及依赖规则传递输入/激活/结果；共享文件系统只用于部署 artifact 和证据，不能替代跨节点激活传输或向 Provider 注入 oracle。
- **FR-010**: **Independent numerical oracle.** MUST 使用同模型/输入/预处理的独立完整模型参考，冻结 hash、shape/class 和容差；不能根据分布式输出放宽标准或只判非空。
- **FR-011**: **Terminal agreement.** PASS MUST 同时满足协议、各请求角色/依赖、CUDA、数值、worker/子进程及清理；marker/READY/exit0 单独不足。缺证据为 FAIL/INCOMPLETE。
- **FR-012**: **Bounded ownership.** 每 candidate/gate MUST 仅一个活动运行；提交状态不明先查询、禁止盲重试；所有等待和清理有期限，只回收本 run 进程/私有目录。
- **FR-013**: **Reuse.** MUST 从同一正常 profile 完成两个独立双节点 allocation，每次 1 warmup + 3 measured 请求；保留失败，不据此宣称性能显著性。
- **FR-014**: **Validation order.** 实现/聚焦回归后 MUST 通过生产接线审计，再 unit → integration → MiniNDN → exact-SIF local → Tiger single-node → Tiger two-node；不能用旧依赖证据替代新组合。
- **FR-015**: **Negative coverage.** MUST 覆盖候选篡改、库/入口错误、cwd/身份错误、Provider 失联、激活丢失/篡改、假 PASS、部分启动/清理失败，注册检测阶段和独立 oracle。
- **FR-016**: **Evidence and recovery.** MUST 保留 candidate/config、精确命令、request/阶段事件、job/node/GPU、首失败、退出/清理、数值/耗时及 raw 索引/hash；大日志/秘密不入 Git。
- **FR-017**: **Capacity and cache.** MUST 按实际峰值和余量检查容量，内容寻址缓存 SIF/模型；不固定通用 20GB 拒绝阈值，不每 30 秒全盘扫描。scratch 不是唯一证据存储。
- **FR-018**: **Human operation.** MUST 提供一个 profile/一个入口的 check/prepare/local/submit/collect、字段说明、成功例和诊断指引；换机器不依赖聊天、私有插件或热修。
- **FR-019**: **GPU offer capacity.** Native V3 ACKs for `cuda:*` topology MUST carry a signed per-device resource snapshot with `free_memory_mb` sufficient for the selected role. Provider readiness or CUDA visibility without this row MUST remain infeasible; measurement failure MUST fail closed.

### Key Entities

- ExperimentProfile：唯一行为配置，引用 release 和 workload。
- Candidate：不可变源码/运行时/工具/输入/规则组合。
- ResolvedRun：run ID、实分配节点/GPU/路径和角色映射。
- GateEvidence / RunResult：绑定 candidate/gate 的真实执行与终态，非人工勾选。

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: **Reject invalid launches.** 全部注册 preflight mutation 在外部副作用前拒绝，并指出字段/文件/阶段。
- **SC-002**: **Verify local path.** 当前依赖 unit、integration、MiniNDN、exact-SIF 本地真实 YOLO 及对应安全/故障负例通过。
- **SC-003**: **Demonstrate distributed execution.** 两个正常 allocation 共 8 请求（2 warmup、6 measured）全 DAG/跨节点/数值通过，模型零 CPU fallback、全部受控清理。
- **SC-004**: **Reuse unchanged configuration.** 两正常 allocation 的 profile/SIF/harness/model/oracle hash 一致，未改脚本，保留新的 node/GPU/run 身份。
- **SC-005**: **Diagnose bounded failures.** 注册负例在 deadline + cleanup 内准确失败，无假 PASS、重复提交、无主进程。
- **SC-006**: **Deliver reproducible handoff.** 干净 checkout 与锁定 artifact 按 quickstart 完成流程，保存证据可离线重算，不修改原记录。

## Assumptions

使用交付 YOLO26n 固定小样本，不训练/扩大模型、不扩展 Qwen/性能矩阵。四 Provider 是四个进程身份，正式双节点每节点一张 GPU，同节点角色可显式共享 GPU；不声称四 GPU 并行。
沿用正常 ACK 驱动协作，不改 Targeted、不增加重试掩盖错误。GPU 型号、partition/account、内存及 Apptainer 实值在 T001 登记；未获得的物理输入保留WAITING_EXTERNAL_INPUT，allocation相关值按T007后的substrate例外核实。用户已授权完成全部任务，当前只有接收清点，不代表模型或集群运行资格。
