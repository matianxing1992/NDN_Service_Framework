# Feature Specification: Spec184 YOLO/Qwen Cross-Host Experiment Closure

**Feature Branch**: `SPEC184Experiments`
**Created**: 2026-09-12
**Status**: Draft
**Input**: User request to turn the Spec184/TigerCluster analysis into a new Spec186 experiment specification and detailed execution ledger.

## Scope And Evidence Boundary

Spec186 将以 `575b43cc93bbed29932303caf3d09974f1585af7` 作为唯一初始源码基线，
建立 `SPEC184Experiments` 实验分支，验证 Spec184 的 YOLO 路径以及本机可运行的
Qwen3-0.6B 路径。当前远端 `Experimental` 的更新提交属于 Spec185，不能作为本
Spec 的隐式基线。

本 Spec 只负责实验、部署可移植性和证据闭合，不重新设计 NDNSF 协议、不把 Python
业务实现移回 Core/DI、不宣称 Qwen3.6-27B 已完成，也不把现有 Spec183 或旧候选
的 PASS 搬到新候选。所有 Tiger 专用脚本、profile、schema、job、launcher、测试
和操作说明必须位于 `Experiments/TigerCluster/`；通用 Core、DI、Repo 源码继续由
原目录负责。

MiniNDN 通过只证明本机多进程路径可运行。TigerCluster 还必须验证 Apptainer
库闭包、节点身份、NFD/TCP 路由、Slurm allocation、GPU 后端、只读应用 bundle、
真实跨节点 Data 依赖和完整清理。失败必须按首次边界分类并保留，不能以启动、
READY、CUDA probe 或部分日志代替最终推理结果。

## User Scenarios & Testing

### User Story 1 - Freeze A Reproducible Candidate (Priority: P1)

操作者从指定的 `575b43c` 建立实验分支，得到一份可审计的候选身份。源码、构建
运行时、harness、profile、模型、输入、oracle、身份和验证契约都能由清单重建。

**Why this priority**: 没有单一候选身份，旧 Spec183、Spec184 和 Spec185 证据会被错误混合。

**Independent Test**: 在没有 SSH、Slurm 或 GPU 的环境中检查提交、文件 hash、profile 和变更失效规则；任何篡改在外部副作用前拒绝。

**Acceptance Scenarios**:

1. **Given** 本地没有该提交对象，**When** 操作者获取对象并验证 SHA、父提交和工作树，**Then** 只有精确的 `575b43c` 可用于创建分支。
2. **Given** candidate 的任一输入、配置或运行时文件被替换，**When** 执行 pre-dispatch gate，**Then** gate 指出最早重跑阶段，并且没有上传、staging、`sbatch` 或远程变更。
3. **Given** 只改变应用层文件，**When** 比较变更平面，**Then** 未改变的基础库 SIF 可复用，但应用、harness 和正式运行证据必须生成新的身份。

### User Story 2 - Verify Native YOLO On MiniNDN (Priority: P1)

操作者在本机以真实 MiniNDN 多进程运行 YOLO，确认 ACK、Selection、角色依赖、
数值 oracle、退出码和清理都来自 C++ native 路径。

**Why this priority**: MiniNDN 是进入 TigerCluster 前最便宜的真实进程边界；它可以先排除应用接线和候选闭包问题。

**Independent Test**: 以新的 candidate/run ID 执行 Y-A、Y-B 和 Y-N，收集协议事件、跨角色 Data、数值结果和清理回执。

**Acceptance Scenarios**:

1. **Given** exact candidate、模型和独立 oracle，**When** 执行正常 Y-A/Y-B，**Then** 产生终端响应，输出 shape 为 `[1,50,6]`，并满足冻结的数值容差。
2. **Given** Y-N 的注册拒绝或 dependency missing 注入，**When** 请求经过 Selection，**Then** 只产生预期的 native failure/observation，不能成功响应、静默重选或留下进程。
3. **Given** MiniNDN 进程启动顺序变化，**When** 使用显式 readiness 和 bounded deadline，**Then** 结果不依赖固定 sleep，并能区分启动失败和业务失败。

### User Story 3 - Run Qwen3-0.6B On Local CPU (Priority: P1)

由于实验机只有 CPU，操作者使用 Qwen3-0.6B 做本机 native MiniNDN 实验，验证多阶段
请求、两轮会话和 checkpoint；该结果作为 CPU 平台验证，不冒充 Spec184 的
Qwen3.6-27B 资格。

**Why this priority**: 它能验证 Qwen 调度、依赖传递和状态连续性是否存在单机隐藏约束，同时成本适合当前主机。

**Independent Test**: 使用 `NDNSF_DI_Qwen06B_Native_Minindn.py` 的 `check → prepare → run/local` 路径，在真实权重和 tokenizer 可用时完成至少一轮冷请求和一轮追加会话。

**Acceptance Scenarios**:

1. **Given** 可核对的 Qwen3-0.6B 模型、tokenizer、stage 清单和 digest，**When** 运行 native MiniNDN，**Then** 记录 token IDs、terminal success、checkpoint、child exit 和 clean cleanup。
2. **Given** 模型仅为 tiny fixture，或权重格式与现有 ONNX CPU runner 不匹配，**When** 执行 preflight，**Then** 结果标为 `SMOKE_ONLY` 或 `WAITING_EXTERNAL_INPUT`，不能升级为实际模型 PASS。
3. **Given** 用户将“Q3”解释为 GGUF/Q3 量化格式，**When** runner 检查模型 backend，**Then** 明确要求兼容 backend 或停止，不静默回退到另一模型格式。

### User Story 4 - Execute The Same Composition On TigerCluster (Priority: P1)

操作者将同一 candidate 的基础库 SIF 和只读应用 bundle 交给 TigerCluster，先做单节点
GPU，再做双节点 YOLO；节点间角色通过 NDN 传递真实依赖，不通过共享文件系统注入中间结果。

**Why this priority**: 双节点真实执行是本 Spec 的部署目标，单机结果只能作为前置证据。

**Independent Test**: 单节点和双节点 run 都记录 allocation、主机、GPU UUID、角色 backend、依赖 Data、数值 oracle、子进程退出和清理。

**Acceptance Scenarios**:

1. **Given** local exact-SIF gate 通过且 profile 未被改写，**When** 提交单节点 GPU run，**Then** 三个模型角色观察到 CUDA，Merge 显式使用 CPU 后处理，并完成 warmup/measured 请求。
2. **Given** 两个真实 compute node，**When** 执行 YOLO graph，**Then** BackboneNeck、DetectShard0、DetectShard1、Merge 分布在声明的节点，跨节点依赖使用真实 NDN Data。
3. **Given** 路由、GPU、身份、容器库或应用权限错误，**When** run 失败，**Then** 保留首失败边界和清理结果，不把 readiness 或 transport PASS 记为推理 PASS。

### User Story 5 - Diagnose And Reuse Safely (Priority: P2)

操作者能用同一正常 profile 完成独立 allocation 复跑，并能看到失败属于代码、候选
闭包、容器、传输、节点资源、harness 还是模型输入，而不是依赖聊天记录猜测。

**Why this priority**: 可复现性要求第二次真实运行和可离线审计的证据。

**Independent Test**: 第一次正常双节点 run 完成后，不改 profile、SIF、应用、模型、oracle 或 harness，启动新的 allocation 并比较 candidate hashes。

**Acceptance Scenarios**:

1. **Given** 完整正常候选，**When** 在新的 allocation 复跑，**Then** 配置和 artifact hash 相同，run、节点、GPU 和身份是新的，结果独立保留。
2. **Given** 提交状态未知或远端目录冲突，**When** 恢复操作，**Then** 先查询和核对原状态，不盲目重复 `sbatch` 或覆盖旧结果。
3. **Given** MiniNDN PASS 而 Tiger 在请求前失败，**When** 生成诊断，**Then** 将问题归为 packaging/profile/transport/loader 边界并提出最小修复。

### Edge Cases

- `575b43c` 不在本地对象库、分支指向错误提交、或工作树包含未声明修改。
- profile 有未知字段、未消费字段、类型错误、路径穿越、符号链接逃逸、重复 run ID 或过期 candidate digest。
- `_ndnsf.so` 能 import 但 `ldd -r` 有未解析符号，或容器内加载了宿主机替代库。
- Qwen3-0.6B 权重缺失、tokenizer 不匹配、ONNX/GGUF backend 不匹配、stage 数量不一致或输入超出容量。
- MiniNDN 使用共享 `/tmp`、共享 HOME、固定 NFD socket 或残留进程；Tiger 使用错误主机名、localhost、GPU ID、Apptainer bind 或 `LD_LIBRARY_PATH`。
- 只有 CUDA probe、Provider READY、ACK 或部分 dependency-withheld 记录，没有 terminal 数值结果。
- 单节点通过、双节点某一节点超时；负例 completion budget 被另一 rank 的冷启动消耗；清理阶段失败。
- 应用层变更、基础 ABI 变更、模型/oracle 变更、harness 变更和主机环境变更同时发生。

## Requirements

### Functional Requirements

- **FR-001**: The experiment MUST be based on the exact source commit `575b43cc93bbed29932303caf3d09974f1585af7`; a branch or remote tip with another tree MUST be rejected.
- **FR-002**: The feature MUST maintain the `SPEC184Experiments` branch intent while storing all new TigerCluster-specific scripts, profiles, schemas, jobs, tests and operating documentation under `Experiments/TigerCluster/`.
- **FR-003**: The candidate MUST bind source, toolchain/dependencies, base runtime, application bundle, harness, effective profile, model, tokenizer, input, oracle, identities and validation contract with hashes.
- **FR-004**: A repository-owned pre-dispatch closure gate MUST reject stale, missing, malformed or cross-candidate inputs before upload, staging, scheduler, remote mutation or campaign calls; mutation tests MUST prove zero such side effects.
- **FR-005**: The system MUST distinguish base-library and application change planes. An application-only change MUST NOT rebuild an unchanged base SIF; a base ABI/toolchain change MUST rebuild affected consumers before qualification.
- **FR-006**: C++ and native extensions MUST be built in a declared matching builder with parallelism no greater than `-j4`; `readelf`, `ldd -r`, RPATH/RUNPATH, SONAME and import/help checks MUST be recorded.
- **FR-007**: The local YOLO path MUST use real MiniNDN process boundaries, explicit per-process identity and NFD endpoints, native C++ business logic, an independent numerical oracle and bounded cleanup.
- **FR-008**: The local Qwen path MUST use Qwen3-0.6B only when actual compatible model artifacts are available; it MUST record model format/backend and MUST label smoke-only or missing-artifact runs separately from model qualification.
- **FR-009**: The Qwen local run MUST exercise one cold request and one bounded follow-up conversation when the model contract is available, including token IDs, terminal status, checkpoint and cleanup evidence.
- **FR-010**: TigerCluster YOLO MUST run first on one real GPU node and then on two real compute nodes, with four independent Provider processes and a declared role-to-node/GPU map.
- **FR-011**: Cross-node intermediate tensors and dependency objects MUST use the existing NDNSF-DI/Repo NDN data path; shared filesystem mounts MUST be limited to immutable deployment artifacts and evidence.
- **FR-012**: A CUDA role MUST provide signed device capacity evidence and observed CUDA execution; visibility, readiness or an empty ACK resource row MUST NOT qualify GPU execution.
- **FR-013**: A formal PASS MUST require protocol completion, numerical oracle, role/backend/GPU evidence, child exit status and clean cleanup; transport, startup, ACK, READY or component-only evidence MUST remain lower-scope evidence.
- **FR-014**: Negative cases MUST be bounded, fail closed, tied to a unique producer/consumer dependency edge and preserve the original failure; no silent retry, reselection or CPU fallback is allowed.
- **FR-015**: Each candidate and gate MUST have at most one active run; unknown scheduler state MUST be queried before retry, and every temporary resource MUST be owned by the run being cleaned.
- **FR-016**: The implementation MUST include a post-implementation design-to-code convergence audit using CodeGraph and effective configuration. Any unresolved semantic, security, production-wiring or evidence discrepancy MUST block full validation.
- **FR-017**: The validation order MUST be focused repair tests → convergence audit → complete unit/integration → MiniNDN → exact-SIF local → Tiger single-node → Tiger two-node → independent reuse.
- **FR-018**: Evidence MUST be stored under `specs/186-spec184-tiger-qwen-experiments/evidence/` and local `Experiments/TigerCluster/results/<run-id>` or declared project storage; secrets, private keys, models, SIFs and large logs MUST remain outside Git.
- **FR-019**: The handoff MUST include exact commands, candidate/profile/artifact hashes, model identity, allocation/job identity, host/GPU observations, first failure if any, exit/cleanup records and offline-verifiable result references.

### Key Entities

- **ExperimentProfile**: 固定 topology、角色、资源、命令、路径、timeouts、模型和证据规则的版本化配置。
- **CandidateManifest**: 将 source、runtime、harness、configuration、external artifacts 和 validation contract 绑定为不可变身份。
- **ModelDescriptor**: 模型格式、stage、tokenizer、输入限制、backend、digest 和 oracle 关系。
- **ResolvedRun**: 一个 run 的 run ID、candidate digest、实际节点/GPU、角色映射、job/allocation 和输出根目录。
- **GateEvidence**: 明确 gate 范围、命令、结果、首失败、退出、清理和可复核 hash 的证据记录。

## Success Criteria

### Measurable Outcomes

- **SC-001**: A clean checkout can verify the exact `575b43c` base and produce one deterministic candidate manifest with no undeclared source or artifact plane.
- **SC-002**: All registered candidate/profile mutation cases are rejected before external side effects, with zero upload, staging, scheduler or remote-mutation calls.
- **SC-003**: Fresh local MiniNDN YOLO Y-A, Y-B and Y-N runs produce terminal, numerical and cleanup evidence for the same candidate; old Spec183 evidence is not reused as proof.
- **SC-004**: When compatible Qwen3-0.6B artifacts are available, local CPU MiniNDN completes one cold request and one follow-up conversation with token/checkpoint/exit/cleanup evidence; otherwise a reproducible `WAITING_EXTERNAL_INPUT` record is produced.
- **SC-005**: One Tiger single-node GPU YOLO run and one Tiger two-node normal YOLO run complete the declared graph with observed CUDA model roles, independent oracle, cross-node dependency evidence and clean cleanup.
- **SC-006**: A registered Tiger dependency-negative run reaches its exact fault boundary within the shared budget, produces no successful response or silent reselection, and leaves no run-owned process.
- **SC-007**: A second independent two-node normal allocation reuses identical profile, base SIF, application, harness, model, input, oracle and graph hashes while producing new run/node/GPU identities.
- **SC-008**: A new operator can reproduce the accepted local and Tiger commands from the Spec186 quickstart and offline evidence without relying on private chat, undocumented paths or hidden host libraries.

## Assumptions

- `q3 0.6B` 默认解释为 Qwen3-0.6B；如果实际资产是 Q3/GGUF 量化格式，必须先提供兼容 native backend，不能伪装成现有 ONNX CPU 路径。
- 本机 MiniNDN 需要 root 或等价 namespace 能力；这是实验 harness 的隔离要求，不应被删除来迁就 Tiger。
- Qwen3.6-27B 的正式资格仍属于外部输入；0.6B 结果不能关闭该行。
- Tiger 物理 GPU、Slurm account/partition、Apptainer 版本、节点名和项目存储位置在 T001 记录，不写死为当前一次 allocation。
- 稳定基础库 SIF 和频繁变化的 DI/UAV 应用 bundle 分层；应用以只读方式挂载，禁止宿主库覆盖容器内基础库。
- 不做性能显著性结论；warmup/measured 只用于正确性和复现证据。

## Out Of Scope

- Spec185 的需求、分支或代码迁移。
- Qwen3.6-27B 外部模型的替代实现或缩小模型后冒充正式结果。
- NDNSF Core wire protocol、权限协议、Targeted API 或通用服务 API 重设计。
- 训练新 YOLO/Qwen 模型、扩大性能矩阵、修改共享基础 SIF 以绕过应用缺陷。
