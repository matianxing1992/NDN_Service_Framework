# Implementation Plan: Spec184 YOLO/Qwen Cross-Host Experiment Closure

**Branch**: `SPEC184Experiments` | **Date**: 2026-09-12 | **Spec**: [spec.md](spec.md)
**Status**: IN_PROGRESS (direct MiniNDN/Tiger qualification remains open)

## Summary

本计划把 `575b43cc93bbed29932303caf3d09974f1585af7` 固定为 Spec186 的初始源码入口；
静态修复后的 candidate 使用该基线的可验证后代 commit，并固定对应 source seal，
把 Spec184 的 YOLO/Qwen 本地实验和 TigerCluster 运行拆成可复核的证据门。实现只
增加实验层适配：profile、candidate closure、lifecycle/transport launcher、
collector、作业模板和文档；业务推理继续由现有 C++ native DI 路径拥有。

Qwen 先在本机 CPU 验证 Qwen3-0.6B。实际权重若与现有 ONNX CPU runner 不匹配，
则停止在模型输入/后端边界并记录 `WAITING_EXTERNAL_INPUT`，不修改协议或静默换
模型。Tiger 使用稳定基础库 SIF 加只读应用 bundle 的分层组合；只改变应用时不
重建未变化的基础 SIF。SIF 默认在本地实验 host 用 Apptainer 1.5.3 构建、做
exact-SIF smoke 并封存 SHA，再上传同一字节到 Tiger；compute 1.5.3 只核对同一
SHA 并运行。若本地 helper、文件系统或权限阻断 materialization/build，才记录例外
并在 compute 1.5.3 以新的 source-sealed candidate 构建，登录节点 1.3.4 永不参与。

## Direct Qualification Path

本计划的主结果是 Spec184 YOLO 在真实 MiniNDN 和 TigerCluster 上完成可复核运行。
依赖图中的静态审计、完整构建、ABI/SIF closure 和资源 preflight 是必要的门，
不是替代实验的交付结果。唯一的主路径是：

```text
T005 convergence
  → T006 native + base/app package closure
  → T007 MiniNDN YOLO Y-A/Y-B/Y-N
  → T006.d local-first SIF promotion or recorded compute-build exception
  → T009 Tiger single-node GPU YOLO
  → T010 Tiger two-node normal + dependency-negative YOLO
  → T012 independent two-node reuse
```

T001–T005、T006.a/b 和 T013 提供可审计的前置或离线收口；T006.c 提供 base+app
分层包，T006.d 在 MiniNDN 后提供进入 Tiger 运行所需的 exact composition。T008 和 T011 仅在 Qwen3-0.6B 的真实
模型/backend/resource tuple 到位时作为辅助路径。任何静态
`PASS`、fixture、READY、CUDA probe、transport receipt 或历史 Spec183 结果都不
能跳过 T007、T009、T010 或 T012。

## Technical Context

**Language/Version**: C++17/NDN runtime、Python 3.x experiment orchestration、bash/Slurm/Apptainer；以基线锁文件和实际 host receipt 为准。

**Primary Dependencies**: ndn-cxx、NFD、ndn-svs、NDNSD、NAC-ABE/OpenABE、NDNSF-DI native runtime、ONNX Runtime CPU/CUDA、MiniNDN、Apptainer、Slurm。

**Storage**: Git 只存规范、脚本、profile、collector 和小型 fixtures；模型、tokenizer、SIF、private keys 与大日志存于声明的本地或 project storage；运行证据位于 `Experiments/TigerCluster/results/<run-id>` 和 Spec186 evidence。

**Testing**: focused unit/mutation tests → design-code convergence audit → cheap source/definition/base-SIF preflight (including builder-consumer and base-capability checks) → complete unit/integration → MiniNDN → local exact-SIF build/import/CPU smoke → immutable upload and same-SHA compute verification → Tiger single-node → Tiger two-node → reuse。后续 loader/runtime 门失败时，在 candidate identity 不变的前提下使用 `--verify-existing`，不重复编译。各门结果不得跨 candidate 晋级。

**Target Platform**: 本地 Linux CPU/MiniNDN 与本地 Apptainer 1.5.3 SIF builder；TigerCluster Linux compute nodes、Slurm 和 Apptainer 1.5.3。实验主机只保留 `/usr/local/bin/apptainer` 1.5.3；Tiger 登录节点的 1.3.4 仅是元数据入口，不参与 SIF 构建/执行，也不是本机回退版本。物理 GPU、节点和 account 仍记录在 allocation receipt。

**Project Type**: experiment/deployment qualification layer over a native distributed-inference application。

**Performance Goals**: correctness and reproducibility only；固定 warmup/measured 请求数和 bounded deadlines，不作吞吐或性能优势声明。

**Constraints**: 构建并行度最高 `-j4`；同一构建树不并发；运行前 fail-closed；无宿主库覆盖；每 candidate/gate 单一活动 run；本机 Qwen 仅 CPU 0.6B 范围。

## Constitution Check

| Principle / gate | Plan response | Status |
| --- | --- | --- |
| Canonical Dynamic Runtime | 不增加旧 generated API；YOLO/Qwen 业务路径继续使用现有 native C++ runtime。 | PASS |
| Security Is Part Of The Data Path | 保留 Controller 授权、角色身份、signed Data、digest、replay/cleanup 边界；缺证明 fail-closed。 | PASS |
| CodeGraph First | T002 和 T005 明确使用 CodeGraph 检查生产入口、调用者、effective config 和证据路径。 | PASS |
| Spec-Driven Durable Work | 本 Spec、plan、tasks、contracts、validation matrix 和 evidence 目录共同作为实施依据。 | PASS |
| Verify With The Right Scope | 将 component、local CPU、single-node GPU、two-node 和 reuse 分开，不跨 candidate 复用 PASS。 | PASS |
| Cohesive Tasks | 每个父任务绑定一个可审查的行为结果；focused test、实现和该 gate 证据保持在同一任务。 | PASS |
| Immutable Promotion | candidate tuple、变更失效矩阵、无副作用 closure gate、真实路径 readiness 和 terminal acceptance 均在计划中。 | PASS |
| Design-Code Convergence | T005 在完整 unit/integration、MiniNDN 和 Tiger 之前执行；未闭合 gap 阻断后续门。 | PASS |

## Architecture And Ownership

| Concern | Owner | Spec186 change |
| --- | --- | --- |
| Request/ACK/Selection/security/wire | `ndn-service-framework/` | 不改；只检查实际调用路径。 |
| Model plan, stage graph, dependency Data, native runners | `NDNSF-DistributedInference/` | 不复制业务逻辑；只提供现有入口的 candidate-bound configuration。 |
| Qwen/YOLO app orchestration | `Experiments/` and native app binaries | 仅新增或调整实验入口、参数和 evidence adapter。 |
| Tiger scripts, profiles, jobs, collector, operations | `Experiments/TigerCluster/` | Spec186 的唯一 Tiger owner；建议新增 `jobs/spec184/` 和 `profiles/spec184-*.json`。 |
| Durable requirements/evidence | `specs/186-spec184-tiger-qwen-experiments/` | 记录 candidate、gate、run、失败和重跑关系。 |

Python 只负责 topology、identities、文件、子进程生命周期和收集；C++ native
组件拥有模型执行、权限、依赖传递、流/会话状态和最终业务结果。MiniNDN 与
Tiger 使用同一业务 binary/config contract，差异只在节点、容器、资源和网络适配。

## Candidate Identity And Change Invalidation

Candidate 是不可变有序组合：

```text
source seal
→ built runtime / ABI closure
→ test and replay harness
→ base SIF + application bundle
→ effective profile and transport layout
→ model/tokenizer/input/oracle
→ identities/permissions
→ validation contract and evidence collector
```

| Changed plane | Earliest restart gate | Reuse rule |
| --- | --- | --- |
| 文档、run ID、证据物理位置 | profile/schema check | 不重编译；产生新 run identity。 |
| Core/Repo/DI ABI、toolchain、dependency lock | T005 → T006 | 干净重建受影响消费者和 base runtime；旧执行 PASS 失效。 |
| DI/YOLO/Qwen app C++ 或自有 extension | T005 → T006 | base SIF 可保持不变；重建并验证 application bundle。 |
| Python launcher、collector、identity/routing、effective config | T005 | 运行证据全部重跑；SIF 只有在 ABI 未变时可复用。 |
| SIF bytes 或 builder definition | T006 | 新 base identity；匹配 app 必须重新闭包。 |
| 本地封存 SIF、上传字节或接收 SHA 不一致 | T006.d | 拒绝 Tiger 运行；保留原 receipt，重新生成 source-sealed candidate。 |
| 模型、tokenizer、输入、预处理、oracle、容差 | T001 → T007/T008 | 不必重建 SIF；所有相关本地/Tiger数值门重跑。 |
| host/GPU/driver/Apptainer/scratch | T009 allocation preflight | candidate bytes 可复用；环境证据和实际 run 必须刷新。 |

任何变更都不能覆盖旧失败或把不同 digest 的结果拼成一个 PASS。应用-only 改动
不得强制重建未变化的 base SIF；基础 ABI 变化则必须重建所有受影响消费者。

## Design-Code Convergence Gate

**Design authority**: [spec.md](spec.md)、[candidate contract](contracts/candidate-identity.md)、[profile contract](contracts/experiment-profile.md)、[validation matrix](validation-matrix.md)、NDNSF architecture docs。

**Production paths to inspect**: `NDNSF_DI_YoloAckDriven_Minindn.py`、`NDNSF_DI_Qwen06B_Native_Minindn.py`、`Experiments/TigerCluster/jobs/spec184/` launcher/submit/collector、native provider/requester binaries、effective Apptainer/Slurm/NFD configuration、model/backend selection、evidence writer。

**Required audit artifact**: `specs/186-spec184-tiger-qwen-experiments/evidence/design-code-convergence.md`，包含 CodeGraph call path、字段 consumer、配置实值、requirements-to-code traceability、severity、owner 和 closing regression。

**Closure rule**: 任何 semantic、architecture、security、production-wiring 或 evidence-validity gap 都是 `BLOCK`；修复必须带 focused regression；修复后重新审计直到 `PASS`。

**Formal validation boundary**: T006 完整 build/unit/integration、T007/T008 MiniNDN、T009–T012 SIF/Tiger/reuse 都依赖 T005 `PASS`。

**Re-audit triggers**: native source/ABI、launcher/collector、profile、identity/route、model/backend、SIF、harness、oracle、dependency 或 evidence contract 的行为变化。

## Execution Gates

1. **G0 / T001** — exact baseline、分支意图、host/model/resource inventory；无外部副作用。
2. **G1 / T002–T004** — portability audit、candidate/profile closure 和 lifecycle/transport adapter；focused tests 可在此阶段执行。
3. **G2 / T005** — design-code convergence `PASS`；否则停止。
4. **G3-pre / T006** — first compare the retained successful GPU tuple in `Experiments/TigerCluster/docs/successful-tiger-gpu-template.md` and record candidate-only differences in `evidence/successful-template-comparison-20260915.md`; then run `prepare-development-handoff.py render`, shell/Python/boundary checks and `preflight-development-sif.py` against the rendered definition, sealed workspace/wheels and exact base SIF. Template drift or preflight failure stops before native compilation and receives a failure-log entry.
5. **G3 / T006** — 完整 build/unit/integration 和 runtime/ABI closure；完成 native/app closure，并在 exact replay entrypoint 下检查 venv 原生 binding 未被源码 wrapper shadow，但不把它升级为 SIF 或 GPU 结果。
6. **G4 / T007–T008** — fresh MiniNDN YOLO 和 Qwen3-0.6B local CPU；分别标记 `LOCAL_CPU_PASS`、`SMOKE_ONLY` 或 `WAITING_EXTERNAL_INPUT`。
7. **G4-promote / T006.d** — 在 MiniNDN 后默认用本地 1.5.3 完成 exact-SIF build/import/CPU smoke 并封存 SIF SHA；上传后只在登录节点做元数据检查，在 compute 1.5.3 核对同一 SHA，不允许隐式远端重建。若本地 builder 有已记录阻断，compute 构建必须使用新的 candidate 和例外 receipt。
8. **G5 / T009** — exact base+app composition、single-node GPU YOLO。
9. **G6 / T010** — first two-node normal YOLO 和 dependency-negative boundary；失败保留，负例必须先闭合。
10. **G7 / T011** — conditional Tiger Qwen experiment；只有模型/backend、资源和 profile 完整时才运行。
11. **G8 / T012–T013** — independent reuse、离线重算、handoff 和 Spec186 closure；不得掩盖未完成的 external Qwen row。

## Project Structure

```text
specs/186-spec184-tiger-qwen-experiments/
├── spec.md
├── plan.md
├── tasks.md
├── validation-matrix.md
├── quickstart.md
├── contracts/
│   ├── candidate-identity.md
│   └── experiment-profile.md
├── checklists/
│   └── requirements.md
└── evidence/
    ├── design-code-convergence.md
    ├── baseline-inventory.md
    ├── minindn-yolo-*.md
    ├── minindn-qwen06b-*.md
    └── tiger-*.md

Experiments/TigerCluster/
├── jobs/spec184/                  # submit/run/collect entrypoints
├── profiles/spec184-*.json        # effective profile definitions
├── adapters/slurm-apptainer/      # reuse canonical transport/runtime primitives
├── apps/                          # only experiment adapters, no Core business logic
├── docs/spec184-*.md              # operator and diagnosis guidance
└── results/<run-id>/              # local ignored evidence, never Git
```

## Migration And Rollback

不要把当前 `TigerClusterExperiments` 的 Spec183 v56 closure 或远端 `Experimental`
Spec185 文件整体合并到 Spec186。只迁移已确认的通用 transport、identity、cleanup、
Apptainer 和 collector primitive，保留 Spec184 的 source seal、model contract 和
新 candidate identity。失败 candidate 不能替换已知稳定 SIF；回滚只能选择一个
完整匹配的旧 candidate，不拼接旧 base 与新 app。

## Complexity Tracking

无 constitution violation。新增复杂度来自两个真实边界：Qwen3-0.6B 的本机 CPU
模型输入，以及 MiniNDN 到 Tiger 的跨主机容器/网络闭包。两者均有独立证据和
停止条件，不新增通用调度平台、协议或数据库。
