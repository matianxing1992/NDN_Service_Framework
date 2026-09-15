# Feature Specification: YOLO MiniNDN SIF+APP Fast Path

<!-- 新 Spec 按 constitution 1.5.0 使用分层双语：叙述性正文用中文，结构标题、ID、状态、路径和命令保持英文。 -->

**Feature Branch**: 187-yolo-minindn-sif-app

**Created**: 2026-09-15

**Status**: Draft

**Input**: User description: 先完成一个最快可运行的 YOLO MiniNDN 实验，复用已有 Experiments/TigerCluster SIF 构建脚本，消除隐藏依赖，先本机验证再交付 TigerCluster；QWEN 暂放 to-do。

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Build and Run YOLO Locally (Priority: P1)

维护者使用现有 TigerCluster 构建入口，把已封存的 NDNSF-DI 源码、稳定基础 SIF 和 YOLO APP 组合成一个可追踪的 SIF+APP candidate，并在本机 MiniNDN 中完成一次真实 YOLO 请求。命令不要求操作者临时设置宿主源码目录、宿主 Python 环境、宿主 .so 或未记录的 /usr/local/lib 路径。

**Why this priority**: 这是最短的可观察闭环。它先证明候选镜像和 APP 能承载真实 NDNSF-DI 请求，再消耗 TigerCluster 资源。

**Independent Test**: 从一个干净的输出目录执行现有 handoff、SIF build、APP materialization 和 pair runner 入口；随后运行维护中的 Experiments/NDNSF_DI_YoloAckDriven_Minindn.py 的 YOLO case。C++ 生产 selector 记录 ACK、Selection、Provider execution 和 terminal Response；Python 只负责 MiniNDN/NFD、身份和子进程编排。

**Acceptance Scenarios**:

1. **Given** 一个 regular base SIF、sealed source bundle、锁定的 build definition 和有效 host-gate manifest，**When** 维护者执行标准构建序列，**Then** 产生不可覆盖的 candidate SIF、APP tree 和 pair manifest，且每个摘要与输入身份一致。
2. **Given** 已发布的 pair candidate，**When** 本机 MiniNDN 启动 YOLO case，**Then** 请求真实经过 ACK admission、Selection、Provider execution 和 terminal Response，输出包含 C++ oracle 与所有子进程终态。
3. **Given** 删除或篡改任一宿主 .so、Python extension、源码目录、旧 /usr/local/lib 路径或 APP 文件，**When** 运行 preflight 或 pair runner，**Then** 在 MiniNDN/NFD 启动前拒绝，并产生可定位的 WAITING_EXTERNAL_INPUT 或输入校验失败，不伪造协议结果。

### User Story 2 - Promote the Same Candidate to TigerCluster (Priority: P2)

维护者把已经通过本机门禁的同一 base SIF、APP、manifest、profile 和运行输入复制到 TigerCluster，直接运行同一 pair runner；集群阶段不重新编译，也不更换未记录的基础库或模型。

**Why this priority**: 本机通过后复用同一 pair identity 可以把集群工作限定为环境差异和资源验证，避免把 TigerCluster 当作配置调试器。

**Independent Test**: 核对本机 candidate identity 与集群提交 bundle 的 digest 和 manifest 完全一致，在一个有界 Slurm 作业中运行 YOLO MiniNDN case；C++ selector 和终态证据沿用本机判据，外部调度失败单独分类。

**Acceptance Scenarios**:

1. **Given** 本机 candidate 已获得 LOCAL_PASS，**When** 复制到 TigerCluster 并提交受限作业，**Then** 作业只挂载该 pair 和明确的模型、身份、NFD 输入，不能在节点上回退到宿主库或重新构建。
2. **Given** 集群上的 base、APP 或 manifest digest 与本机记录不同，**When** 作业启动，**Then** 在请求前拒绝并保留首个身份边界；不得把不同 candidate 的日志合并为一次资格结果。
3. **Given** TigerCluster 调度、Apptainer、NFD 或身份设施失败，**When** 作业结束，**Then** 结果标为 UNQUALIFIED 或 WAITING_EXTERNAL_INPUT，保留原始日志，不升级为 YOLO 协议 PASS。

### User Story 3 - Defer QWEN Without Expanding the Fast Path (Priority: P3)

维护者可以看到 QWEN 的依赖和未完成项已经登记，但 YOLO 本机/集群闭环不等待 QWEN tokenizer、streaming conversation、模型 staging 或多角色资源资格。

**Why this priority**: QWEN 的模型和流式状态更复杂；把它从首个闭环中隔离可以减少批次扩张和重复构建。

**Independent Test**: 检查 Spec187 的 scope、tasks 和 evidence 明确把 QWEN 标为 TODO，并验证 YOLO candidate、构建和验收不读取 QWEN 文件或配置。

**Acceptance Scenarios**:

1. **Given** QWEN 相关模型或配置缺失，**When** 执行 Spec187 YOLO path，**Then** YOLO path 仍按自身输入完成或按自身边界失败，不回退到 QWEN。
2. **Given** 需要开始 QWEN 工作，**When** 新建后续任务，**Then** 使用新的 candidate identity、动态矩阵和独立 evidence，不复用 YOLO 的协议 PASS。

## Acceptance Evidence Contract *(required for code-backed stories)*

| Story / FR | Production entry / callers | Observable outcome | Independent oracle / C++ selector | Negative / recovery boundary | Dynamic profile / invariant | Evidence owner / path | Batch / Coverage reference |
| --- | --- | --- | --- | --- | --- | --- | --- |
| US1 / FR-001..FR-006 | Experiments/TigerCluster/adapters/slurm-apptainer/scripts/prepare-development-handoff.py, build-local-sif.sh, build-sif-app.py, run-sif-app.sh; Experiments/NDNSF_DI_YoloAckDriven_Minindn.py | Local candidate SIF+APP is immutable, self-contained and reaches one terminal YOLO response | Registered C++ selector `Spec187YoloMiniNdn` (tests/wscript) owns correlated request/ACK/Selection/Provider/Response assertions; MiniNDN Python runner only owns external processes and cleanup | Missing/changed base, APP, ABI, Python extension, NFD, identity or model input fails before network side effects; child exit and cleanup are checked | asan-ubsan; invariant: one candidate identity, authenticated selection, terminal response, zero hidden host library paths | specs/187-yolo-minindn-sif-app/evidence/local-yolo-<run-id>.md | B187-LOCAL; five-lane matrix in plan.md/tasks.md |
| US2 / FR-007..FR-009 | Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-sif-app.sh and existing Slurm job wrapper | Same pair identity runs on TigerCluster without rebuild or fallback | Same C++ Spec187YoloMiniNdn selector and retained terminal oracle; scheduler/Apptainer records are external evidence | Digest mismatch, scheduler failure, mount failure and NFD/KeyChain startup failure remain UNQUALIFIED or WAITING_EXTERNAL_INPUT | none for first bounded cluster smoke; invariant: candidate digest and profile unchanged | specs/187-yolo-minindn-sif-app/evidence/tiger-yolo-<run-id>.md | B187-TIGER; qualification matrix only after local PASS |
| US3 / FR-010 | Documentation and task registry only | QWEN is visible as deferred work and does not enter YOLO candidate | N/A; no native behavior is claimed | Missing QWEN inputs do not block or alter YOLO; later QWEN work gets a new candidate | none; invariant: no QWEN input read by YOLO path | specs/187-yolo-minindn-sif-app/tasks.md and evidence/qwen-deferred.md | B187-DEFERRED; coverage N/A by scope |

## Edge Cases

- 基础 SIF 路径是 dangling symlink、非 regular file、摘要变化或缺少 /opt/ndn-base/lib、/opt/onnxruntime/lib、/opt/venv/bin/python 时，构建和运行在副作用前停止。
- APP 中的 ELF、Python extension 或脚本依赖宿主源码、构建目录、/usr/local/lib 或不匹配的 libndn-cxx 时，验证器拒绝；不能由 MiniNDN 的单机文件系统掩盖。
- candidate、APP、profile、model、identity、NFD socket 或 output root 在 preflight 后被替换时，运行器重新核对 inode/digest 并拒绝 TOCTOU 变化。
- 本机磁盘空间不足、Apptainer 版本不是 1.5.3、C++ selector 未链接或外部模型缺失时，记录首个边界并保持 PARTIAL，不重试成假 PASS。
- YOLO C++ 请求返回协议失败、超时、取消或非零子进程时，保留原始输出并区分 UNQUALIFIED 与输入/调度失败。
- QWEN 文件存在但未被本 Spec 的 profile 声明时，仍不得被隐式读取或混入 candidate。

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST reuse the existing Experiments/TigerCluster handoff, local SIF build, APP materialization and pair-runner entrypoints; a new parallel SIF workflow is out of scope.
- **FR-002**: System MUST require one immutable candidate identity binding source seal, definition, base SIF digest, APP digest, build record, Apptainer 1.5.3 identity, profile, model inputs and output manifest.
- **FR-003**: System MUST prove that every native ELF and Python extension used by the YOLO APP is built in the container-native boundary and resolves only to declared APP or base paths; host source, host build output, host virtualenv and unrecorded /usr/local/lib are forbidden.
- **FR-004**: System MUST provide a single documented local sequence that validates inputs before side effects, builds or reuses the sealed candidate, materializes APP, and runs the maintained YOLO MiniNDN harness.
- **FR-005**: The local YOLO acceptance MUST execute a real C++ production selector through MiniNDN and assert authenticated ACK admission, Selection, Provider execution, terminal Response, child exit status and bounded cleanup; Python may orchestrate only external facilities and process lifecycle. The selector correlates every stage record by `requestId`, `attemptEpoch` and `planDigest`, and checks monotonic `epochMs` ordering. The `NDNSF_DI_NATIVE_ACK_CLOSED` record is the one pre-plan exception and therefore carries the authenticated `ackDigest` instead of a plan digest.
- **FR-006**: System MUST classify missing, changed or incompatible inputs before MiniNDN startup and preserve the first failure boundary; a startup/preflight result MUST NOT be reported as a protocol or model result.
- **FR-007**: System MUST permit TigerCluster execution only after a fresh local candidate and local YOLO gate pass; the cluster job MUST reuse the exact pair identity and MUST NOT rebuild or substitute libraries.
- **FR-008**: System MUST revalidate pair digests, runtime paths, NFD/KeyChain mounts, profile and model identity at cluster startup, with no fallback to host libraries or stale image links.
- **FR-009**: System MUST keep local, cluster, scheduler, Apptainer, MiniNDN, C++ selector and cleanup outcomes in separate evidence records with explicit status values (LOCAL_PASS, TIGER_PASS, UNQUALIFIED, WAITING_EXTERNAL_INPUT, PARTIAL).
- **FR-010**: QWEN streaming/model staging/conversation work MUST remain explicitly deferred and MUST NOT be required by, or silently included in, the YOLO candidate or its acceptance.
- **FR-011**: Before any local SIF build, this feature MUST pass the design-to-code convergence audit for the actual handoff/build/materialize/run call chain; later behavior-affecting changes invalidate the audit and require re-audit.
- **FR-012**: Each logical batch MUST use the shared five-lane coverage matrix, immutable review snapshot, read-only review-agent gate, batch composition review and one batch-level dynamic gate card; static PASS alone MUST NOT close a behavior task.

### Key Entities

- **Base SIF**: 提供稳定操作系统、NDN-CXX/NFD、ONNX Runtime、Python 和明确基础库路径的 regular Apptainer image；不包含本次版本化 DI APP。
- **YOLO APP**: 由容器内候选构建产生的无 symlink 应用树，包含 NDNSF-DI、Core/SVS/NDNSD/NAC-ABE/OpenABE/Relic 闭包和 YOLO application entrypoint。
- **Pair Manifest**: 绑定 base、APP、source/build/profile/model/identity/Apptainer 摘要及挂载契约的不可变组合记录。
- **Local Run Record**: 本机 MiniNDN 的 C++ selector、Python orchestration、子进程、终态、清理和首个失败边界记录。
- **Tiger Run Record**: 集群作业的同一 candidate identity、调度/Apptainer/NFD 输入和独立结果记录。

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 在一个已具备 regular base SIF 和 host-gate manifest 的工作区中，维护者只按一套文档化入口即可从 sealed source 生成并验证 YOLO pair；不需要手工修改宿主 LD_LIBRARY_PATH、PYTHONPATH 或复制宿主 .so。
- **SC-002**: 本机 YOLO MiniNDN run 在同一 candidate identity 下连续完成 2 次，C++ selector 每次都观察到 ACK、Selection、Provider execution、terminal Response 和所有子进程正常退出；任何一次失败都保留独立原始记录。
- **SC-003**: candidate 的 ELF/Python closure 检查发现 0 个宿主源码、构建目录、宿主 virtualenv、未声明 /usr/local/lib 或不匹配 libndn-cxx 解析；故意删除或替换任一输入时在 MiniNDN 启动前拒绝。
- **SC-004**: 本机 LOCAL_PASS 后，TigerCluster 使用完全相同的 base digest、APP digest、pair manifest、profile 和模型身份运行一次有界 YOLO job；若任一身份不一致，作业在请求前拒绝且不产生协议 PASS。
- **SC-005**: 本 Spec 的实现批次、静态审查、构建、C++ selector、MiniNDN run 和集群 run 均有可追踪 evidence；未执行阶段保持 PARTIAL 或 WAITING_EXTERNAL_INPUT。
- **SC-006**: QWEN 相关任务在 Spec187 完成记录中保持 TODO，不增加 YOLO 构建或本机/集群验收的前置依赖。

## Scope and Dependencies

本 Spec 只建立 YOLO 的最短垂直切片，复用以下 existing 入口：

1. Experiments/TigerCluster/adapters/slurm-apptainer/scripts/prepare-development-handoff.py
2. Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-local-sif.sh
3. Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-sif-app.py
4. Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-sif-app.sh
5. Experiments/NDNSF_DI_YoloAckDriven_Minindn.py

当前已知的 external dependency 是一个真实 regular base SIF；本机现有 spec180-runtime-r119.sif 断链，不能作为 base。取得 base 前不得把 pair 构建标为 PASS。 本机 SIF 构建统一使用 Apptainer 1.5.3；本机磁盘空间和 host-gate manifest 也是构建前置条件。

QWEN 的 tokenizer、streaming conversation、模型 staging、长时间生成和多角色 cluster qualification 不在本 Spec 范围内，统一登记为 TODO，不得从 YOLO 的结果推导完成。

## Assumptions

- 接收机器能够提供与 profile 匹配的 regular base SIF，或由后续独立任务按当前 base contract 产生；本 Spec 不把 dangling historical link 当作有效输入。
- 旧 TigerCluster 脚本和 YOLO MiniNDN harness 的行为保持向后兼容；若实际调用链需要修改，先更新本 Spec 的 plan/tasks 和 evidence owner。
- 本地 MiniNDN 只证明候选的真实请求调用链和本机设施；它不替代 TigerCluster 的调度、节点驱动或 GPU 资格。
- C++ selector、fixture、oracle 和构建注册由 Spec187 实现批次补齐；Python 只承担 MiniNDN、NFD、身份、挂载和进程生命周期编排。
- 本 Spec 不自动上传、提交 Slurm、启动 TigerCluster 或 staging QWEN 模型。

## Revision History

- 2026-09-15：创建 Spec187，范围收窄为 YOLO MiniNDN 本机优先、同一 candidate 交付 TigerCluster；复用现有 TigerCluster SIF 脚本，QWEN 明确延期。
