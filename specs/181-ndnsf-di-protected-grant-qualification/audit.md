# Spec181 Design-code Convergence Audit

**Date**: 2026-09-05 | **Revision**: 6 | **Task**: T007
**Source identity**: 初审基线 `67194dc2`；当前公共准备/worker 检查点
`0a3a79c3`、`cf15fa0c`。历史运行仅对应各自证据中的源/构建。
**Layer**: proposed（设计）+ implemented（源码核查）+ executed（定向 unit / live control）。
**Verdict**: **BLOCK**。本审查取代旧 `CONDITIONAL PASS / no HIGH` 结论。

## Findings

路径缩写：P = `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`；
N = `NDNSF-DistributedInference/cpp/ndnsf-di/`；
U = `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`；
R = `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`。行号对应本轮源码。

| ID | Severity | Location | Finding and evidence boundary | Disposition / closing action |
|---|---|---|---|---|
| A01 | HIGH | N/ProtectedRuntime.cpp；N/NativeProviderHandler.cpp；R；evidence/t002-acceptance-20260905.md | 初审发现 runtime 无条件拒绝、缺 factory、保护 Y-B 使用 Python Provider。native Ed25519/P-256 正负链、helper 生命周期、进程集成和 ASAN 已有证据；worker 授权、公共准备/adapter 与 handler 接线已收口，统一构建及新隔离 P-256 控制 PASS。 | CLOSED — T002 unit/integration 与 FR-015 验收完成；正式同源资格仍归 T005/T008，generation worker 修复见 A11。 |
| A02 | HIGH | P；evidence/t001-request-lifecycle-20260905.md | 授权顺序、独立模型/策略绑定、实际磁盘读取与两类明文清理已修复。12 项失败回归定位请求取消/截止漏检，4 项失败回归定位策略快照替换；修复覆盖准备及实际 worker 排队后的重新校验。 | CLOSED — T001 逐项验收 PASS：最终 151 项定向回归、6 个真实 Python 进程用例；inline/external、成功/异常/取消/过期均有检查。 |
| A03 | HIGH | P:1445；tests/python/test_spec181_provider_grant.py:445 | 原实现未核对 Selection grant 摘要，权威另签的同上下文 grant 可替换选中密钥。新回归实测 `ProtectedGrantRejected not raised`。 | CLOSED — `ff7b5c3b` 增加封印摘要比对；RED 1 failed，GREEN 34 passed，见专项证据。 |
| A04 | HIGH | U；R；ProtectedRuntime.cpp；evidence/t006-production-repair-20260905.md | 初审发现 User 内部 probe 冒充 Provider 拒绝。现已移除该成功判据，记录并核对实际发布、Provider verifier、请求/attempt/Provider 与封印计划；三种真实变异和有效 grant 控制通过。 | CLOSED — T006 定向生产验收 PASS；正式同源矩阵仍归 T005/T008。 |
| A05 | HIGH | scripts/run_spec181_y_n_matrix_retry.py；R；evidence/t005-evidence-repair-20260905.md | 旧 driver 删除 attempt、重试任意异常并取首个 PASS，已停用；维护矩阵改为首个失败即停止，保留原始结果。新增 5 项失败回归，修复后相关 118 项检查 PASS。 | PARTIAL — 证据覆盖/跨运行挑选入口已移除；完整源/构建/配置闭包审查仍归 T007，正式同源矩阵待 T005/T008。 |
| A06 | HIGH | plan.md revision 4 Summary / Ownership / Gate order；spec.md SC-001 | 计划同时指定 user 与 Controller 权威，已延期撤销仍写入成功条件；审计依赖后续资格，而资格又依赖审计。 | CLOSED（设计）— revision 5 统一进程内权威、保留延期边界，T007 PASS 先于 T005/T008；不扩大范围。 |
| A07 | HIGH | tests/python/test_spec181_assembly_parity.py；tests/fixtures/spec181/assembly-vectors-v1.json；evidence/t003-assembly-parity-20260905.md | 初审发现 grant parity 冒充装配覆盖。现已添加 8 个固定装配向量，分别经过 Python 直接入口和真实 C++ 入口/正常 helper，验证字节、摘要、ORT CPU 结果与变异拒绝。 | CLOSED — 16 项装配检查 + 3 项 grant parity PASS；格式算法共用 Python 实现，不声称独立 C++ 算法或网络资格。 |
| A08 | HIGH | U；security/registry_keys.py；evidence/t001-request-lifecycle-20260905.md | 注册表算法、公钥摘要、模型/epoch、独立逻辑身份与最终 root 允许列表已接线；实际 user/native 控制和维护 Python 消费链有证据，本轮注册表与 seam 回归通过。 | CLOSED — T001 任务验收完成；冻结注册表保持不变，正式同源资格仍归后续任务。 |
| A09 | MEDIUM | tasks.md revision 4；T001/T002/T004/T006 证据；旧 audit.md | 五项任务打勾但缺其约定生产 integration；T004 测试明确使用 fake native；声称的 provider integration 文件不存在。任务加粗语法还导致扫描器解析为 0 tasks。 | CLOSED（进度/设计）— 恢复未完成标记并列出已有实现，规范 12 个 T 任务、4 个故事与三层证据。真实验收仍归所属任务。 |
| A10 | MEDIUM | active-context health；failure index | 活动 feature 已为 181，但托管 plan 链接仍指 180；failure index 未指向当前 181 失败。 | CLOSED — 链接、索引已修复；project/active health 均 exit 0，当前文件来源 fresh。 |

## Traceability Gaps

**A11 / HIGH / CLOSED (focused repair) — Shared Generation Worker Authority**：
`NativeEpochCoordinator` 到 registered runtime/worker 未传递公共 guard；
排队取消/截止两个实际回归进入模型 1 次（预期 0），1/3 cases PASS。
修复沿原执行/状态回滚 owner 传递授权，重建后 48 cases /
366 assertions PASS；源码行号、完整 RED/GREEN 与资格边界见
[generation worker authority](evidence/t007-generation-worker-20260905.md)。

- FR-015：用户要求共用 YOLO/Qwen 已有执行机制。当前共用接口已存在，
  native 准备分支仍重复证据初始化、YOLO 算法位于通用 runtime 目录；
  多轮授权与清理一致性仍待核对。T002/T007 按
  [共享路径核对](evidence/shared-runtime-reuse-20260905.md) 收口；维持 BLOCK。

- FR-002/003/013：T001 的任务验收已完成；T002 已有真实 grant 接线、维护进程集成和定向清理证据，native handler 源码闭包及剩余边界核对仍开放。
- FR-004：T006 实际 Provider 拒绝网络证据已关闭 A04；正式同源矩阵由 T005/T008 验收。
- FR-012：T003 固定装配向量已完成双入口字节/摘要与 ORT CPU 检查，A07 CLOSED。
- FR-005/011：T004 真实启动/取消验收已完成，依赖图保留；后续源变更需按影响重新验证。
- SC-003--006：没有本 Spec 同源完整矩阵、local-suite inventory、SIF/Tiger
  或唯一 closure；未来 evidence 路径均明确标记 planned。
- R003 的清单记录旧文件“无层声明”却给 PASS，不能解释为逐文件要求已
  全满足。本轮保护 Spec180，历史覆盖缺口由 T007 的当前清单承接。
- 未发现需新增模型、网络服务或撤销任务的依据；现有 T 任务可承接修复。

## Readiness Scorecard

| Principle | Status | Rationale |
|---|---|---|
| 1 Intent fidelity | PASS（设计范围） | 保留 YOLO 功能闭环及既定延期，不扩展性能结论。 |
| 2 Necessity and Occam | PASS（修复规划） | 复用既有机制，不新增协议；grant 与装配各自有验收价值。 |
| 3 Architecture and ownership | BLOCK | native 生产链与注册表消费已接线并有定向证据，剩余 handler 源码闭包和完整异常验收未闭合。 |
| 4 Cross-document consistency | PASS（修订后设计） | 统一权威、撤销边界、七子用例、验收依赖与映射。 |
| 5 Code fact verification | BLOCK | A02 Python 和 A07 装配 parity 已验收；A01 native 的完整源码闭包仍未完成。 |
| 6 Security and distributed correctness | BLOCK | 摘要替换已修复；前置授权、存储、清理、策略仍有缺口。 |
| 7 Task executability | PASS（修订后计划） | 12 个内聚任务；定向修复→审计→资格，包含 T004。 |
| 8 Validation design | BLOCK（当前实现） | Y-N-E production oracle 已验收；旧重试器已停用，维护矩阵首个失败即停止；其余生产覆盖与同源闭包仍待闭合。 |
| 9 Evidence integrity | BLOCK（资格） | 旧 PASS 降为诊断，34 项回归严格为 unit；没有新矩阵。 |
| 10 Frozen evidence protection | PASS（本轮变更边界） | 未修改 Spec180 冻结证据；不认可重试器覆盖行为。 |
| 11 Migration and rollback | CONDITIONAL PASS | 延期与临时路径 owner/删除条件明确，删除验收仍待执行。 |
| 12 Verdict gate | BLOCK | A01/A05 的当前源/构建/配置闭包未闭合，禁止资格晋升。 |

## Metrics and Task Cohesion

4 user stories；14 FR；6 SC；12 T tasks，4 个历史 R safeguards 独立记录。
本次 10 个发现：8 HIGH、2 MEDIUM；4 个文档/代码/上下文发现已关闭，
其他按表中状态执行。T001/T002 因 Python/native owner 与验收边界不同
保留；T003 负责跨语言等价性；T006 构造与生产拒绝，T005 同源矩阵。
未发现需要机械拆分“测试/实现/跑测试”的理由。

## Evidence Limits

- 文档声称：revision 5 规定待完成行为；不是实现 PASS。
- 代码实现：CodeGraph 首查后用工作区精确路径核对；其初次结果混入
  临时历史副本，已排除，不能用索引健康代替生产事实。
- 测试执行：本轮仅重跑 grant 引用修复的 34 项 unit/seam 回归；旧
  2682、143、9-vector 等记录未在本轮重新执行，不作当前通过声明。
- 实验测量：本轮未启动 MiniNDN、SIF 或 Tiger。当前原始
  `/tmp/spec181-y-n-run/yb39.log` 六次 `control` startup 失败，
  包装脚本 `EXIT=0` 不是 Y-B PASS，也不足以归因 OOM/transport 竞态。
- 工作区起始 775 条预存变更，不是已封印候选；提交只包含本轮明确路径。

## Next Actions

**Implementation checkpoint (2026-09-05)**：A02 的前置授权、独立模型名
绑定、落盘密文读取与已登记明文清理已有定向修复；70 项 unit 回归
通过，见 `evidence/t001-provider-repair-20260905.md`。这不是重新审计
PASS。后续 A08 注册表公钥/策略、逻辑身份和最终 root 允许列表已接线，
100 项定向 unit 通过，见 `evidence/t001-registry-repair-20260905.md`。
后续维护真实进程集成与请求生命周期验收已完成 T001，见
[T001 完成审查](evidence/t001-request-lifecycle-20260905.md)。其他控制性缺口仍开放。
T002 的 `ProtectedRuntime` 已接入真实 verifier 和受管内容密钥，
18 项定向 C++ 用例通过，见 `evidence/t002-runtime-repair-20260905.md`；
后续 native store 与私有目录清理已有实现，20 项 C++ 和 55 项 Python/
跨语言存储检查通过，见 `evidence/t002-storage-repair-20260905.md`。
后续真实 native Y-B 与 grant 负例已有定向证据；helper 的超时、
并发取消、过期、输出限制及取消后目录重建问题已修复，27 项检查
通过，见 [helper 生命周期](evidence/t002-helper-lifecycle-20260905.md)。
factory 的 P-256 凭据入口缺口已由实际失败回归确认并修复，8 项
定向检查通过；factory/header 纳入 `35e1c6d5`，见
[凭据入口](evidence/t002-recipient-credentials-20260905.md)。后续
`835f20f9` 修复 Python loader、recipient map 与实际 EC 信封创建，
134 项检查及重建后的四收件人 P-256 Y-B 控制 PASS，见
[P-256 生产链](evidence/t002-p256-production-20260905.md)。任务指定的
维护 integration 与 P-256 ASAN 已通过，见 [维护进程集成](evidence/t001-t002-process-integration-20260905.md)。
worker 另发现准备后或计算期间取消/过期仍成功返回；4 项失败回归
修复后，加上缓存正例和流事件拒绝，共 51 cases / 280 assertions PASS，
见 [worker 授权](evidence/t002-worker-authority-20260905.md)。
剩余 native handler 源码闭包与边界核对仍未完成，A01 未整体关闭；
这些更新不改变本审计 BLOCK 裁决。

1. T001 已完成；后续相关源变更按影响复验已有注册 handler 与真实
   进程测试，不把当前完成状态当作未来候选的资格证据。
2. T002：native Ed25519/P-256 正向链与 T006 负例已有证据，继续
   核对剩余边界并完成 handler 源码提交。T003 已完成 grant 与装配两组 parity。
   T004 已补当前 build 的真实就绪/取消/无热转证据并完成定向验收，
   见 [生命周期验收](evidence/t004-lifecycle-acceptance-20260905.md)；
   此项关闭不改变整体 BLOCK 裁决。
3. T006 已关闭 User probe 资格漏洞并完成真实变异验收；T005 旧
   重试入口已停用，维护矩阵首个失败即停止；正式矩阵仍等待 T007。
4. T007 对同一源/构建/有效配置重新审计；只有新 PASS 才执行 T005/T008。
5. 本地资格通过后按 T009→T010→T011→T012 晋升；历史结果不得拼接。
