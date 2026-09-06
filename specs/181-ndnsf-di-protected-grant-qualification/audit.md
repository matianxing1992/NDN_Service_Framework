# Spec181 Design-code Convergence Audit

**Date**: 2026-09-06 | **Revision**: 7 | **Task**: T007
**Source identity**: 初审基线 `67194dc2`；当前 G0 完成检查点 `453f6990`，
公共准备与 worker 修复为 `0a3a79c3`、`cf15fa0c`。
local gate 提交/源码身份修复为 `2628e3d2`。
**Layer**: proposed / implemented / wired / executed（限定在各记录的检查范围）。
**Verdict**: **BLOCK**。G0 已完成，T007 的 A05 源/配置闭包尚待核查。

**Scope**：按所有者确认改为本机开发、本地验证与交付；T010/T011
TRANSFERRED，实验机器负责 SIF/Tiger，不再作为本地审计/关闭依赖。
本机 10 个活动任务中 5 个完成；Git 合并留待当前开发完成后另行讨论。
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
| A05 | HIGH | scripts/run_spec181_y_n_matrix_retry.py；R；scripts/spec180_inventory.py；scripts/run_spec180_local_gate.py | 旧重试器删除/挑选结果的问题已修复（118 checks）；后续复现资格范围遗漏、重算摘要后的案例脚本替换和越界 entry ID。维护 inventory/gate 统一注册契约并收集活动测试，21 focused checks PASS，见 evidence/t007-qualification-scope-20260905.md。 | PARTIAL — 本地清单工具单元已闭合；native/candidate 的有效配置、封印输入及剩余生产源闭包仍待审查，正式矩阵归 T005/T008。 |
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
- A05 剩余的是生产入口、构建源清单、有效配置和资格收集入口的同源闭包核查；T005 的正式网络矩阵不移入 T007。
- local gate 的实际 Git 根/提交/index/源码字节及运行后变化校验已修复（`2628e3d2`）；最终 R8 39 focused checks PASS，真实 `1ba99000` 构建 checkout 的只读源码核对 PASS。生成的本地工具/runtime、外部 import 与实际配置摘要仍待绑定，见 [local gate identity](evidence/t007-local-gate-identity-20260906.md#checkpoint-gate-r8)。
- A05 的干净编译核查暴露 framework 配套声明遗漏与 Provider 根元数据丢失；已在隔离源码修复并链接生产库，26 cases / 204 assertions PASS，见 [framework source closure](evidence/t007-framework-source-closure-20260906.md)。本轮关闭该 framework 单元；完整 native/candidate 源和有效配置仍须核查。
- A05 的 native projection 声明缺口已补齐并通过一次明确差异源码的维护 native build；随后修复 COMPONENT_SET 后处理解析和双张量 scope，29 cases / 133 assertions PASS，见 [native plan closure](evidence/t007-native-plan-closure-20260906.md)。后续以该源码提交刷新 native 身份，继续候选与有效配置审查。
- A05 最初的无 Git HEAD/虚构 sourceRevision probe 为历史失败证据，已由上述源码校验关闭；后续运行时/输入及交付身份审查仍 BLOCK，不能晋升正式资格。
- 启动配置单元已关闭：builder/gate 共用实际环境、工作目录、解释器文件与启动策略摘要，并核对运行后身份；最终 62 focused checks PASS（5.80 s），包含真实 child 环境消费及 CLI 联通/配置漂移拒绝。见 [local launch configuration](evidence/t007-local-config-identity-20260906.md)。该摘要不封存 native/import 依赖或外部输入文件字节；这些 A05 平面仍待核查，T007 保持 BLOCK。

## Readiness Scorecard

上述源码单元 checkpoint 为 `6b9bb51c`。R13 最终提交的维护
source guard 与实际 application/native preflight 都 PASS；R12 的
未跟踪 Repo 中间对象已完整移出源码检出保留，不放宽 gate。
19 个 staged Python 文件与 R11 已测源逐字节相同。源码单元
提交核对已完成，下一步按这些证据更新 A05/逐原则整体裁决。

A05 实际应用源码闭包的 R11 已通过：同一隔离源码 45 项公共请求/
签名/目录/规划检查与 12 项候选绑定回归 PASS，真实 publication、
process_specs、native guard 和四个应用入口导入 PASS。新增 native
receipt 已由 a51f87b3 的维护构建生成，Repo 扩展也在同源树编译。
见 [runtime refresh](evidence/t007-local-runtime-refresh-20260906.md)。
R1–R10 暴露的缺失模块/字段/导出均保留；当前仅关闭此源差异单元，
最终提交身份复核和 T007 整体审查仍待完成，正式矩阵没有执行。

配置外部输入的清单/执行稳定性已修复：72 focused checks PASS
（8.40 s），模型、映射及引用 key 变化会阻止资格通过并保留已有
结果；C++/Python discovery 消费同一显式环境。见
[输入身份证据](evidence/t007-local-input-identity-20260906.md)。
实际 import/runtime 核查与新 native receipt 仍待收口，T007 BLOCK。

显式配置根被默认 HOME 路径覆盖的问题已修复，24 focused checks
PASS（0.85 s）；绝对/相对目录实际传入 child，显式缺 key 不回退。
见 [配置选择证据](evidence/t007-explicit-config-root-20260906.md)。
该修复关闭路径选择，不代替外部文件内容的身份核查。

Waf 源/工具选择缺口已在维护 native owner 修复：80 focused checks
PASS（1.75 s），两个真实 checkout 的只读目录对照均 PASS。新
`waf_tool` 字段参与 receipt、复用及前后核对；旧 receipt 必须由
维护构建刷新。[证据与范围](evidence/t007-waf-tool-identity-20260906.md)
保留 RED 和 fixture 修复。T007 继续核查外部 import/模型输入及
runtime 闭包，不以该单元代替实际 native rebuild 或正式验收。

| Principle | Status | Rationale |
|---|---|---|
| 1 Intent fidelity | PASS（修订 7 设计） | 本机开发、本地矩阵、同源交付；实验机器接管 SIF/Tiger，按开发/实验而非模型分工；既有安全延期边界保留。 |
| 2 Necessity and scope | PASS | 复用公共协议和运行时；grant、parity、真实负例及资格阶段各有独立验收目的。 |
| 3 Architecture and ownership | PASS（G0 检查范围） | requester 内逻辑权威、Core 生命周期、DI 授权/装配/worker、模型 adapter 所有权已有调用链及定向证据。 |
| 4 Cross-artifact consistency | PASS（当前映射） | 本机 5/10、2 TRANSFERRED、G1/T007、FR-008/009/015、SC-005/006 与交接/本地关闭依赖同步。 |
| 5 Code reality | BLOCK | A05 的候选源/构建/有效配置完整闭包尚未核查，不能用工作区定向 PASS 替代。 |
| 6 Security and distributed correctness | BLOCK（A05） | G0 的授权、绑定、取消/过期、存储和清理缺口已修复；候选实际配置与同源消费仍须闭包核查。 |
| 7 Task executability | PASS | 10 个本机内聚任务和 2 个外部移交项；保留原 ID、owner 与验收，不把移交改为完成。 |
| 8 Validation design | PASS（设计） | 先定向 RED/GREEN 与审计，再正式矩阵；维护矩阵首个失败即停，保留所有运行。 |
| 9 Evidence integrity | PASS（清单范围） | 每文件层声明或历史缺口明确，旧 PASS 不晋升；清单检查覆盖集合、哈希和头部漂移，不判运行资格。 |
| 10 Migration and rollback | PASS（已提交单元） | 明确临时门吸收条件，冻结文件未改，本地 checkpoint 只纳入已验证单元，其他预存修改保留。 |
| 11 Performance and operations | PASS（功能范围） | 不作性能/扩展性结论；已记录启动、绝对截止、子进程退出、资源清理，后续资格继续绑定实际运行。 |
| 12 Documentation quality | PASS（当前结构） | 15 FR、6 SC、4 stories、10 active tasks、5 complete、2 TRANSFERRED；历史 ID 不重编号，文档健康不是资格证据。 |

## Evidence and Tool Limits

- 本审查使用各修复记录及其原始日志，不把历史不同源的测试数相加成一次同源套件。
- 最近公共准备模型控制是隔离 P-256 Y-B，四 Provider grant 验证、数值匹配和七个子进程退出已收集。随后 worker 修复通过定向检查；`1ba99000` 已在干净 tracked checkout 刷新维护 native build/实际扩展身份（见 native closure R3），仅覆盖该提交，尚未获得正式资格。
- 2026-09-06 重新核对 FR-015 的模型 adapter、生产公共准备与生成 worker guard 调用链，并同步共享说明/plan/traceability/tasks 的旧进度表述；未新增模型或资格范围，T007 继续由 A05 控制。
- 本轮 R003 只审计文档。没有因此启动完整网络矩阵、SIF 或 Tiger，也不重跑未变的模型测试。
- Context Mode project/active health 通过，权威来源明确命中当前 tasks.md；统计中的跨宿主汇总不作为当前项目或资格证明。
- CodeGraph 泛化检索在 20 s 超时；改用精确 `scripts/spec180_inventory.py` 节点成功，再按当前文件核对。没有把检索超时当作运行失败。
- 原工作区的大量预存修改尚未整体封印。后续必须审查候选实际纳入的源/配置，而非将 dirty tree 全部提交或忽略。

## Metrics and Task Cohesion

4 user stories；15 FR；6 SC；10 active T tasks（5 complete）、2 TRANSFERRED，另有 4 个先行 safeguards。
12 个发现（9 HIGH、3 MEDIUM），A05 PARTIAL，其余在表述的限定范围闭合。
T001/T002 保留 Python/native owner 与验收边界；T003 保留跨入口 parity；
T006 构造与生产拒绝，T005 执行同源矩阵。没有新增模型、网络权威服务或撤销任务。

## Next Actions

1. 核对 A05 的实际本地构建依赖、有效配置、开发交付封存与验证结果收集入口，必要时定向修复；SIF/Tiger 特有身份由实验机器后续处理。
2. 按实际变更刷新 source/native identity，更新本审计的逐项证据并作 T007 裁决；不是先跑正式矩阵再补审计。
3. T007 PASS 后执行 T005/T008 同源完整本地资格，再按 T009→T012 交付并关闭；之后另行讨论 Git 合并，T010/T011 按移交契约由实验机器执行。
