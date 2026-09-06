# Spec181 Design-code Convergence Audit

**Date**: 2026-09-06 | **Revision**: 7 | **Task**: T007
**Source identity**: 初审基线 `67194dc2`；当前 G0 完成检查点 `453f6990`，
公共准备与 worker 修复为 `0a3a79c3`、`cf15fa0c`。
local gate 提交/源码身份修复为 `2628e3d2`。最终审计对象为
`6b9bb51c43a597bf36ca62f304fee5f71c17fa58` 的隔离源码和 R13 runtime；
`8c231493` 仅补记证据，主工作区其余预存改动不在该审计对象内。
**Layer**: proposed / implemented / wired / executed（限定在各记录的检查范围）。
**Verdict**: **PASS**（T007 convergence audit；sudo source boundary re-audited）。
首次正式 sudo 启动发现 _source_git 丢失 SUDO_UID，已按实际 owner
限定保留并通过真实 sudo 正负例及既有 gate 的 51 项回归（8.68 s）；
不接受 Git 配置/index/replacement 覆盖。R1/R2 失败均未联网，
见 [正式矩阵与修复](evidence/t005-formal-matrix-20260906.md)。A05 的源码、有效配置、
输入、构建工具与实际应用依赖核查已闭合；允许执行 T005/T008。
本裁决不是本地资格、开发交付或最终关闭 PASS。

T005 R8 暴露启动异常在矩阵包装后丢失底层位置。追加只含类型与
frame 的失败记录，93 项 runner/matrix 回归 PASS；phase 回滚、
首次失败停止与裁决保持不变，未记录异常文本或 locals。此定向
证据改进通过受影响项复审；真实 spawn 根因仍由下一次执行定位。

**Scope**：按所有者确认改为本机开发、本地验证与交付；T010/T011
TRANSFERRED，实验机器负责 SIF/Tiger，不再作为本地审计/关闭依赖。
本机 10 个活动任务中 6 个完成；Git 合并留待当前开发完成后另行讨论。
原实验验收和反馈契约见 [handoff-contract.md](handoff-contract.md)。

## Findings

路径缩写：P = `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`；
N = `NDNSF-DistributedInference/cpp/ndnsf-di/`；
U = `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`；
R = `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`。
初审位置用于保留发现来源；当前修复位置与测试边界见对应 evidence 链接。

| ID | Severity | Location | Finding and evidence boundary | Disposition / closing action |
|---|---|---|---|---|
| A01 | HIGH | N/ProtectedRuntime.cpp；N/NativeProviderHandler.cpp；R；evidence/t002-acceptance-20260905.md | 初审发现 runtime 无条件拒绝、缺 factory、保护 Y-B 使用 Python Provider。native Ed25519/P-256 正负链、helper 生命周期、进程集成和 ASAN 已有证据；worker 授权、公共准备/adapter 与 handler 接线已收口，统一构建及新隔离 P-256 控制 PASS。 | CLOSED — T002 unit/integration 与 FR-015 验收完成；正式同源资格仍归 T005/T008，generation worker 修复见 A11。 |
| A02 | HIGH | P；evidence/t001-request-lifecycle-20260905.md | 授权顺序、独立模型/策略绑定、实际磁盘读取与两类明文清理已修复。12 项失败回归定位请求取消/截止漏检，4 项失败回归定位策略快照替换；修复覆盖准备及实际 worker 排队后的重新校验。 | CLOSED — T001 逐项验收 PASS：最终 151 项定向回归、6 个真实 Python 进程用例；inline/external、成功/异常/取消/过期均有检查。 |
| A03 | HIGH | P:1445；tests/python/test_spec181_provider_grant.py:445 | 原实现未核对 Selection grant 摘要，权威另签的同上下文 grant 可替换选中密钥。新回归实测 `ProtectedGrantRejected not raised`。 | CLOSED — `ff7b5c3b` 增加封印摘要比对；RED 1 failed，GREEN 34 passed，见专项证据。 |
| A04 | HIGH | U；R；ProtectedRuntime.cpp；evidence/t006-production-repair-20260905.md | 初审发现 User 内部 probe 冒充 Provider 拒绝。现已移除该成功判据，记录并核对实际发布、Provider verifier、请求/attempt/Provider 与封印计划；三种真实变异和有效 grant 控制通过。 | CLOSED — T006 定向生产验收 PASS；正式同源矩阵仍归 T005/T008。 |
| A05 | HIGH | scripts/run_spec181_y_n_matrix_retry.py；R；scripts/spec180_inventory.py；scripts/run_spec180_local_gate.py | 旧重试器删除/挑选结果的问题已修复（118 checks）；后续复现资格范围遗漏、重算摘要后的案例脚本替换和越界 entry ID。维护 inventory/gate 统一注册契约并收集活动测试，21 focused checks PASS，见 evidence/t007-qualification-scope-20260905.md。 | CLOSED — 下方 A05 Closure Matrix 将每个身份平面对应到生产入口、定向回归及最终提交检查；R13 source/native/application 均 PASS。正式矩阵归 T005/T008，最终交付归 T009。 |
| A06 | HIGH | plan.md revision 4 Summary / Ownership / Gate order；spec.md SC-001 | 计划同时指定 user 与 Controller 权威，已延期撤销仍写入成功条件；审计依赖后续资格，而资格又依赖审计。 | CLOSED（设计）— revision 5 统一进程内权威、保留延期边界，T007 PASS 先于 T005/T008；不扩大范围。 |
| A07 | HIGH | tests/python/test_spec181_assembly_parity.py；tests/fixtures/spec181/assembly-vectors-v1.json；evidence/t003-assembly-parity-20260905.md | 初审发现 grant parity 冒充装配覆盖。现已添加 8 个固定装配向量，分别经过 Python 直接入口和真实 C++ 入口/正常 helper，验证字节、摘要、ORT CPU 结果与变异拒绝。 | CLOSED — 16 项装配检查 + 3 项 grant parity PASS；格式算法共用 Python 实现，不声称独立 C++ 算法或网络资格。 |
| A08 | HIGH | U；security/registry_keys.py；evidence/t001-request-lifecycle-20260905.md | 注册表算法、公钥摘要、模型/epoch、独立逻辑身份与最终 root 允许列表已接线；实际 user/native 控制和维护 Python 消费链有证据，本轮注册表与 seam 回归通过。 | CLOSED — T001 任务验收完成；冻结注册表保持不变，正式同源资格仍归后续任务。 |
| A09 | MEDIUM | tasks.md revision 4；T001/T002/T004/T006 证据；旧 audit.md | 五项任务打勾但缺其约定生产 integration；T004 测试明确使用 fake native；声称的 provider integration 文件不存在。任务加粗语法还导致扫描器解析为 0 tasks。 | CLOSED（进度/设计）— 恢复未完成标记并列出已有实现，规范 12 个 T 任务、4 个故事与三层证据。真实验收仍归所属任务。 |
| A10 | MEDIUM | active-context health；failure index | 活动 feature 已为 181，但托管 plan 链接仍指 180；failure index 未指向当前 181 失败。 | CLOSED — 链接、索引已修复；project/active health 均 exit 0，当前文件来源 fresh。 |
| A11 | HIGH | N/NativeEpochCoordinator.cpp；N/NativeProviderRuntime.cpp；N/ProviderRoleWorker.cpp；evidence/t007-generation-worker-20260905.md | 生成协调器的 stopCheck 未传入排队 worker；两个 RED 用例在取消/截止后仍调用模型。 | CLOSED — `cf15fa0c` 沿同一 guard/回滚 owner 修复，重建 48 cases / 366 assertions PASS；不声称 Qwen 模型资格。 |
| A12 | MEDIUM | evidence/r003-evidence-banner-audit-current.md；scripts/spec181_evidence_inventory.py | 旧清单省略记录、称 Spec181 无证据文件；实际已有 37 个，其中 4 个缺头部层声明；旧正文仍列出已关闭的 T002 缺口。 | CLOSED（文档覆盖）— 补层声明、完整逐文件哈希/范围清单及漂移检查；Spec180 冻结原文未改，见 `evidence/t007-evidence-inventory-20260905.md`。 |

## Traceability and Current Boundaries

- FR-001/002/003/013：T001/T002 的真实 grant、内容密钥消费、清理及请求生命周期验收已完成；[T002 acceptance](evidence/t002-acceptance-20260905.md) 汇总具体入口与证据。
- FR-004：T006 三种实际 Provider grant 拒绝和正向控制已通过；T005 正式同源七子用例矩阵仍待 T007 PASS。
- FR-005/011：T004 真实进程启动、取消及无热转检查已通过；后续行为变更按影响复验。
- FR-012：T003 grant 与装配向量分别验收，装配经过 Python/C++ 生产入口与 ORT CPU，未以 grant parity 替代。
- FR-014：R001/R002/R004 的未接线路径已由对应生产验收吸收，缺失授权仍失败关闭。R003 的当前完整清单显式限制每个历史文件的使用范围，不改冻结原文。
- FR-015：公共准备只保留一个 context owner，YOLO 算法已归 adapter；生成和普通 worker 共用授权边界。[共享路径核对](evidence/shared-runtime-reuse-20260905.md) 列出差异 owner 和定向回归。
- FR-008/009、SC-003--006：同源完整矩阵、local-suite inventory、开发交付清单与本地 closure 尚未完成；T009 核对交付/移交材料，T012 发出 LOCAL_DEVELOPMENT_PASS。原 SIF/Tiger 验收保留为外部 TRANSFERRED，既不冒充完成也不形成新的本地关闭依赖。
## A05 Closure Matrix

| Boundary | Current source owner | Closing evidence |
|---|---|---|
| Exact checkout / selectors | `scripts/run_spec180_local_gate.py:399`、`scripts/spec180_inventory.py:521` | [源码门](evidence/t007-local-gate-identity-20260906.md)：39 checks；[R13](evidence/t007-local-runtime-refresh-20260906.md#committed-source-and-runtime-r13) 的实际最终提交 gate PASS。错误提交/index/源字节先于执行拒绝。 |
| Actual launch configuration | `scripts/spec180_inventory.py:81`、`scripts/run_spec180_local_gate.py:543` | [62 checks](evidence/t007-local-config-identity-20260906.md)：实际 cwd/env/解释器/超时参与摘要，前后核对，真实 child/CLI 覆盖。 |
| External inputs and explicit key root | `scripts/spec180_inventory.py:124`、`scripts/run_spec180_local_gate.py:550` | [72 checks](evidence/t007-local-input-identity-20260906.md) 绑定实际模型/配置/映射/引用 key 字节；[24 checks](evidence/t007-explicit-config-root-20260906.md) 覆盖显式根与缺 key 拒绝。 |
| Native build and Waf selection | `scripts/spec180_native_build.py`、`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:1492` | [80 checks 与两检出对照](evidence/t007-waf-tool-identity-20260906.md)；a51f87b3 维护 native build 及同源 Repo 扩展构建成功；R13 在最终提交的实际 child 环境验证 native receipt。 |
| Public application dependency closure | `adapters/yolo/`、`splitter.py:341`、`app_sdk/contracts.py:692`、`app_sdk/provider.py:92` | [runtime refresh](evidence/t007-local-runtime-refresh-20260906.md)：45+12 定向检查，19 个 staged Python 文件与已测源码逐字节相同；最终提交真实 publication/process_specs/native guard/四应用入口导入 PASS。 |
| Supervision / evidence preservation | `scripts/run_spec180_local_gate.py:517`、`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:3190` | [118 checks](evidence/t005-evidence-repair-20260905.md) 与 [21 checks](evidence/t007-qualification-scope-20260905.md)；所有执行结果保留，身份漂移使总结果 UNQUALIFIED，缺注册案例不能晋升。 |

这里的构建/依赖核查证明当前明确环境可从所选源码构建并运行；
不声称整个操作系统、第三方 Python distribution 或瞬间变更后恢复
均被持续证明。T008 记录实际 runtime/native/input 身份，T009 封存
同一环境与证据；变更来源、配置、依赖或模型输入须重新核查受影响门。
最终开发交付工具与材料归 T009，不把它们尚未执行误作 T007 的循环前提。

## Readiness Scorecard

| Principle | Status | Rationale |
|---|---|---|
| 1 Intent fidelity | PASS | 本机开发/本地验证/交付；实验机器负责 SIF/Tiger；按工作性质分工，保留安全延期边界。 |
| 2 Necessity and scope | PASS | 沿用公共协议/runtime/Repo；没有增加模型资格、网络权威或撤销任务。 |
| 3 Architecture and ownership | PASS | G0 公共 grant/装配/生命周期与 adapter 差异 owner 已有生产链和回归；A05 补齐同一公共契约的提交依赖。 |
| 4 Cross-artifact consistency | PASS | 6/10、2 TRANSFERRED、T007 PASS 后进入 T005/T008；FR/SC 与交接条件同步。 |
| 5 Code reality | PASS | A05 Closure Matrix 的提交源码、真实构建、应用入口与行为回归对应；不以主工作区混合源码代替。 |
| 6 Security and distributed correctness | PASS | G0 授权/绑定/取消/清理生产验收与 A05 实际配置/输入前后校验已覆盖；正式网络负例仍归 T005/T008。 |
| 7 Task executability | PASS | 10 个内聚本机任务与 2 个移交项；T005/T008 可按既有 runner 执行，失败保留原始目录。 |
| 8 Validation design | PASS | RED/GREEN 与审计先行，正式矩阵后行；注册全覆盖、实际 Provider 拒绝、终端/清理共同判定。 |
| 9 Evidence integrity | PASS | 完整逐文件 inventory 与漂移检查；历史 PASS 保留限定，失败不删除、不择优复用。 |
| 10 Migration and rollback | PASS | 独立本地 checkpoint、未提交改动隔离；临时诊断由生产证据吸收，冻结历史保留。 |
| 11 Performance and operations | PASS | 功能范围；绝对截止/启动/子进程清理有定向证据，后续矩阵不产生性能或 GPU 声称。 |
| 12 Documentation quality | PASS | 15 FR、6 SC、4 stories、10 active tasks（6 complete）、2 TRANSFERRED；本裁决与资格/交付分离。 |

R003 逐文件层声明/失效范围以维护 inventory 为准。临时 preflight
脚本和失败 raw runs 留在 ignored 工作目录，未成为生产执行分支；
其排查用途由维护 runner/native guard/source gate 吸收。原始失败
证据继续保留，不能为清理目录而删除尚需复现的失败；交付只收
维护入口及摘要/复现说明，不携带临时脚本或私钥。

## Evidence and Tool Limits

- 本审查使用各修复记录及其原始日志，不把历史不同源的测试数相加成一次同源套件。
- 最近公共准备模型控制是隔离 P-256 Y-B，四 Provider grant 验证、数值匹配和七个子进程退出已收集。随后 worker 修复通过定向检查；`1ba99000` 已在干净 tracked checkout 刷新维护 native build/实际扩展身份（见 native closure R3），仅覆盖该提交，尚未获得正式资格。
- 2026-09-06 重新核对 FR-015 的模型 adapter、生产公共准备与生成 worker guard 调用链，并同步共享说明/plan/traceability/tasks 的旧进度表述；未新增模型或资格范围，A05 已按上表关闭，正式网络结果仍由 T005/T008 验收。
- 本轮 R003 只审计文档。没有因此启动完整网络矩阵、SIF 或 Tiger，也不重跑未变的模型测试。
- Context Mode project/active health 通过，权威来源明确命中当前 tasks.md；统计中的跨宿主汇总不作为当前项目或资格证明。
- CodeGraph 泛化检索在 20 s 超时；改用精确 `scripts/spec180_inventory.py` 节点成功，再按当前文件核对。没有把检索超时当作运行失败。
- 原工作区的大量预存修改尚未整体封印。后续必须审查候选实际纳入的源/配置，而非将 dirty tree 全部提交或忽略。

## Metrics and Task Cohesion

4 user stories；15 FR；6 SC；10 active T tasks（6 complete）、2 TRANSFERRED，另有 4 个先行 safeguards。
12 个发现（9 HIGH、3 MEDIUM），A01–A12 均在表述的限定范围闭合。
T001/T002 保留 Python/native owner 与验收边界；T003 保留跨入口 parity；
T006 构造与生产拒绝，T005 执行同源矩阵。没有新增模型、网络权威服务或撤销任务。

## Next Actions

1. 以通过审计的相同代码和明确输入执行 T005 七子用例矩阵，再执行 T008 的完整本地清单与 Y-A/Y-B/Y-N。
2. 失败保留新 run、首个边界与全部退出结果；行为/源/配置变更后定向修复并复审受影响项，不复用失效 PASS。
3. T009 封存同源开发交付与实验交接材料，T012 作 LOCAL_DEVELOPMENT_PASS 裁决；之后再讨论 Git 合并。T010/T011 由实验机器执行。
