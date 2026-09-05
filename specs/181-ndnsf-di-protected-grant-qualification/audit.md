# Spec181 Design-code Convergence Audit

**Date**: 2026-09-05 | **Revision**: 5 | **Task**: T007
**Source identity**: 基线 `67194dc2`；定向修复 `ff7b5c3b`；本修订文档差异。
**Layer**: proposed（设计）+ implemented（源码核查）+ executed（定向 unit）。
**Verdict**: **BLOCK**。本审查取代旧 `CONDITIONAL PASS / no HIGH` 结论。

## Findings

路径缩写：P = `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`；
N = `NDNSF-DistributedInference/cpp/ndnsf-di/`；
U = `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`；
R = `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`。行号对应本轮源码。

| ID | Severity | Location | Finding and evidence boundary | Disposition / closing action |
|---|---|---|---|---|
| A01 | HIGH | N/ProtectedRuntime.cpp；N/NativeProviderHandler.cpp；R；evidence/t002-native-live-repair-20260905.md | 初审发现 runtime 无条件拒绝、缺 factory、保护 Y-B 使用 Python Provider。修复 native 选择、group 摘要、grant 路由、装配 basename 和就绪等待竞态后，live-r7 真实受保护 native Y-B 与正常清理 exit 0。 | PARTIAL — 定向正例及文件清理已通过；T002 仍需生产负例、取消/过期、资源上界及工作区源码闭包。 |
| A02 | HIGH | P:1369、1445、1474、1485、1507、2559、2695、2734 | 先装配后授权；Merge 用 grant 自述 manifest 作预期值；落盘密文后直接解密内存对象；external weights 明文复制且未登记租约；准备阶段后续异常提前 return 可绕过 handler 的 finally。 | OPEN — T001 重排授权、绑定独立输入并覆盖全部加载/清理边界；错误密钥、磁盘密文变异、准备失败与取消测试。 |
| A03 | HIGH | P:1445；tests/python/test_spec181_provider_grant.py:445 | 原实现未核对 Selection grant 摘要，权威另签的同上下文 grant 可替换选中密钥。新回归实测 `ProtectedGrantRejected not raised`。 | CLOSED — `ff7b5c3b` 增加封印摘要比对；RED 1 failed，GREEN 34 passed，见专项证据。 |
| A04 | HIGH | U；R；ProtectedRuntime.cpp；evidence/t006-production-repair-20260905.md | 初审发现 User 内部 probe 冒充 Provider 拒绝。现已移除该成功判据，记录并核对实际发布、Provider verifier、请求/attempt/Provider 与封印计划；三种真实变异和有效 grant 控制通过。 | CLOSED — T006 定向生产验收 PASS；正式同源矩阵仍归 T005/T008。 |
| A05 | HIGH | scripts/run_spec181_y_n_matrix_retry.py:48--73、84--94；evidence/t005-y-n-matrix-current.md | driver 删除已有 attempt 目录、重试任意异常且取首个 PASS；产物是自有 retry schema。旧记录混合不同提交的 6/7、7/7 与 NOT PROVEN，不能形成同源矩阵。 | OPEN — T005 改为保留每次失败、唯一 run-id、受限启动重试与同源身份；本轮已纠正文档状态。 |
| A06 | HIGH | plan.md revision 4 Summary / Ownership / Gate order；spec.md SC-001 | 计划同时指定 user 与 Controller 权威，已延期撤销仍写入成功条件；审计依赖后续资格，而资格又依赖审计。 | CLOSED（设计）— revision 5 统一进程内权威、保留延期边界，T007 PASS 先于 T005/T008；不扩大范围。 |
| A07 | HIGH | tests/python/test_spec181_native_grant_parity.py:23、41、62；旧 traceability FR-012 | 9 个固定 grant 向量只检查解包；没有 ONNX canonical+recipe 双侧装配字节比较。已有映射将 grant parity 误作 FR-012 完成证据。 | OPEN（实现）— T003 已补独立装配向量/验收路径；现有 grant 向量覆盖保留。 |
| A08 | HIGH | U；security/registry_keys.py；evidence/t001-registry-repair-20260905.md | 初审发现硬编码发布身份和空模型白名单。当前接线校验注册表算法、公钥摘要、模型/epoch，区分 requester 与 authority，并绑定最终发布 root；100 项定向 unit 通过。 | PARTIAL — 注册表与逻辑身份修复已接线；T006 三种生产变异已验收，T001 策略与发布/消费完整验收仍开放；冻结注册表保持不变。 |
| A09 | MEDIUM | tasks.md revision 4；T001/T002/T004/T006 证据；旧 audit.md | 五项任务打勾但缺其约定生产 integration；T004 测试明确使用 fake native；声称的 provider integration 文件不存在。任务加粗语法还导致扫描器解析为 0 tasks。 | CLOSED（进度/设计）— 恢复未完成标记并列出已有实现，规范 12 个 T 任务、4 个故事与三层证据。真实验收仍归所属任务。 |
| A10 | MEDIUM | active-context health；failure index | 活动 feature 已为 181，但托管 plan 链接仍指 180；failure index 未指向当前 181 失败。 | CLOSED — 链接、索引已修复；project/active health 均 exit 0，当前文件来源 fresh。 |

## Traceability Gaps

- FR-002/003/013：T001/T002 仍缺完整生产接线与错误路径清理证据。
- FR-004：T006 的真实密码学 helper 不是 Provider 授权拒绝网络证据。
- FR-012：T003 原装配验收缺失已写回任务，尚未执行或证明一致。
- FR-005/011：现有 unit 不代替真实进程 integration；T004 不应被依赖图漏掉。
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
| 3 Architecture and ownership | BLOCK | 设计归属已澄清；native 生产路径与注册表消费未闭合。 |
| 4 Cross-document consistency | PASS（修订后设计） | 统一权威、撤销边界、七子用例、验收依赖与映射。 |
| 5 Code fact verification | BLOCK | A01/A02/A07 的源码距离明确且保留未完成状态。 |
| 6 Security and distributed correctness | BLOCK | 摘要替换已修复；前置授权、存储、清理、策略仍有缺口。 |
| 7 Task executability | PASS（修订后计划） | 12 个内聚任务；定向修复→审计→资格，包含 T004。 |
| 8 Validation design | BLOCK（当前实现） | Y-N-E production oracle 已修复并完成定向验收；重试器与其他生产覆盖仍待闭合。 |
| 9 Evidence integrity | BLOCK（资格） | 旧 PASS 降为诊断，34 项回归严格为 unit；没有新矩阵。 |
| 10 Frozen evidence protection | PASS（本轮变更边界） | 未修改 Spec180 冻结证据；不认可重试器覆盖行为。 |
| 11 Migration and rollback | CONDITIONAL PASS | 延期与临时路径 owner/删除条件明确，删除验收仍待执行。 |
| 12 Verdict gate | BLOCK | A01/A02/A05/A07/A08 未闭合，禁止资格晋升。 |

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
真实网络验收及其他控制性缺口仍开放。
T002 的 `ProtectedRuntime` 已接入真实 verifier 和受管内容密钥，
18 项定向 C++ 用例通过，见 `evidence/t002-runtime-repair-20260905.md`；
后续 native store 与私有目录清理已有实现，20 项 C++ 和 55 项 Python/
跨语言存储检查通过，见 `evidence/t002-storage-repair-20260905.md`。
生产 factory/装配接线仍在工作区验证中，真实网络与 ORT 生命周期
仍未闭合，A01 未整体关闭；这些更新不改变本审计 BLOCK 裁决。

1. T001：在装配前完成独立绑定与授权；补注册表消费、模型/weights 密文
   读取及全错误路径清理，建立真实发布/获取的定向进程测试。
2. T002/T003：接入 native 授权 runtime，并完成 grant 与装配两组 parity。
   T004 已补当前 build 的真实就绪/取消/无热转证据并完成定向验收，
   见 [生命周期验收](evidence/t004-lifecycle-acceptance-20260905.md)；
   此项关闭不改变整体 BLOCK 裁决。
3. T006 已关闭 User probe 资格漏洞并完成真实变异验收；T005 继续
   修复重试器证据边界，保留所有失败，不运行正式矩阵。
4. T007 对同一源/构建/有效配置重新审计；只有新 PASS 才执行 T005/T008。
5. 本地资格通过后按 T009→T010→T011→T012 晋升；历史结果不得拼接。
