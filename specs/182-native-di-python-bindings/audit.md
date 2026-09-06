# Spec182 Design Audit

**Revision**: 4 | **Mode**: design-workflow revision / pre-implementation
**Verdict**: DRAFT / BLOCK for implementation
**Evidence layer**: source/design review；runtime NOT_RUN
**Reviewed source**: merged-source-baseline-r3.json；未提交merge及修复快照，不仅HEAD。
**Evidence**: [revision 4 review](evidence/static-review-gate-revision4.md)；[revision 3 history](evidence/skill-and-design-revision3.md)
本轮按用户要求将静态读码审查前置到unit/integration/MiniNDN之前；仅修改技能和文档，不对尚未实现的182签发产品审查PASS。

## Findings

| ID | Severity / status | Finding / evidence | Disposition |
| --- | --- | --- | --- |
| A182-01 | HIGH / OPEN | 合并代码/依赖仍有修复，revision 1/2 source snapshot不再是当前基线 | O-001；T001读取合并closure与181承接，不要求先完成181旧完整资格 |
| A182-02 | HIGH / OPEN | native ONNX extraction/checker/protobuf 逐字节复现和依赖锁未证明 | O-002；T006 前锁定算法/API/版本；不编造原生库结论 |
| A182-03 | HIGH / OPEN | native tokenizer encode/decode ABI/线程/资源契约尚未冻结 | O-003；T007 前以固定向量决定依赖 |
| A182-04 | HIGH / OPEN | 公开字段/默认值、完整 caller/admin/journal 兼容清单未关闭 | O-004；T001 完成，CD-013/014 只补已可证实的职责缺口 |
| A182-05 | MEDIUM / OPEN | 无 Python 隔离工具/权限/服务实现白名单未冻结 | O-005 设计由 T001 关闭；T014 实现和反例验证；不是 T014 自行猜隔离后称 READY |
| A182-06 | MEDIUM / OPEN | T003/004/006/008/010/011 仍为较大设计批次，实际 selectors/叶子边界未冻结 | T001 细化内聚原子单元；当前不宣称可直接交大包编码 |
| A182-09 | HIGH / FIXED_IN_DESIGN | revision 1 FLOW 把认证 model/graph/offers 当现成输入，缺原生准备和 offer policy owner；源码有 GraphAdapter/TaskAdapter、CanonicalCatalogEnsurer、verify_ack | 新 CD-013/T008/PO-013，端口、时序、错误、清理、反例、FR 映射齐备 |
| A182-10 | HIGH / FIXED_IN_DESIGN | revision 1 CD-008 写 Provider 转发，却没有可供绑定的 native host；现有接线留在 executable | 新 CD-014/T009/PO-014；复用 NativeProviderHandlerConfig/runtime，绑定和 CLI 消费同一宿主 |
| A182-11 | HIGH / FIXED_IN_DESIGN | 原 FLOW-002 将本地取消直接连到 Provider executionGuard；ServiceUser.cpp:11150 的方法没有 remote abort | 本地 fence、stream cancel、远端 deadline/control 与 cleanup 分开；不许推导立即停止远端 |
| A182-12 | HIGH / FIXED_IN_DESIGN | 原 T014 正式验收任务才创建 MiniNDN harness；原 T013 先审计，无法检查尚未存在的真实 collector | revision 2 T014 构建/自检 harness → T015 审计 → T016 只执行冻结验收 |
| A182-13 | HIGH / FIXED_IN_DESIGN | 原 migration “删除无消费者”没有退出 owner/计数/数据回退和 mixed-version 规则；callable 兼容与原生目标冲突未分层 | runtime migration 契约：T013 去向/删除记录、legacy=0、原生行为替代、journal 备份/原子转换/禁止未知降级 |
| A182-14 | MEDIUM / FIXED_IN_DESIGN | O-005 由 T014 解决，但 T014 AllowedDecisions 又要求其已冻结；T001 ZERO 却允许依赖选择 | O-005 设计前置 T001，T001 改 BOUNDED_DESIGN；T014 只实现已定方案 |
| A182-15 | MEDIUM / FIXED_IN_DESIGN | 原 T002 未有 requester 实现却用完整 PO-001 关闭；审计/交付批次复制了“测试80--300行” | T002 仅 existing runtime 安装链接与 PO-001 L0；完整调用 T010；纯审计/交付0生产/测试源码 |
| A182-16 | MEDIUM / FIXED_IN_DESIGN | 原“最新 R18”已过时；当前 failure index 为 exact-tensor R6 focused PASS | 保留 revision 1 历史快照，新证据记录 R4/R6 首边界与最新源码身份，不升级为正式资格 |
| A182-17 | MEDIUM / FIXED_IN_DESIGN | observer exception 不改结果，但队列 overflow 又可能被解释为业务终态失败 | observation DELIVERY_OVERFLOW 与可靠 stream gap 的业务失败分开，PO-007 两种反例 |

FIXED_IN_DESIGN 仅表示规范缺陷已修正，不表示对应 native 行为已实现或验证。
原 A182-07/08 的目标与分工判断保留于下列 scorecard，不能抵消 OPEN。

## Revision 3 Findings

| ID | Severity / status | Finding | Disposition |
| --- | --- | --- | --- |
| A182-18 | HIGH / FIXED_IN_DESIGN | 旧基线与181完整关闭前置不符合用户最新合并/承接顺序 | 新快照记录HEAD+MERGE_HEAD+每文件hash；FR-015/T001改为合并稳定及承接 |
| A182-19 | HIGH / FIXED_IN_DESIGN | 将全部撤销笼统列为延期会丢失合并Core已实现机制 | 保留RequestConfidentiality/ControllerVersion/撤销刷新/RuntimeStatusStore；DI工件grant扩展单列 |
| A182-20 | MEDIUM / FIXED_IN_DESIGN | CandidateBudget被写成maxRoles/maxNodes/deadline，实际是三个不同预算字段 | 对照core/ports.py修正max_candidates/max_policy_ms/max_reentries及单位/default |
| A182-21 | HIGH / PARTIAL | 只列类名/签名不足以指导迁移，注释和变量责任没有逐项契约 | FR-017/SC-009、21类/模块、48方法、137来源字段与每任务文档/用法门；嵌套schema/ABI/完整caller仍OPEN，禁止声称全部READY |
| A182-22 | HIGH / OPEN | 合并记录unit/integration仍失败，无法作为完整稳定交付基线 | 保留首边界及来源记录；本轮不修改或重跑另一工作单元修复 |

## Revision 4 Findings

| ID | Severity / status | Finding | Disposition |
| --- | --- | --- | --- |
| A182-23 | HIGH / FIXED_IN_DESIGN | 只有T015正式资格前整体audit，不能阻止各单元先跑focused tests再发现明显逻辑错误 | 新FR-018/SC-010/PO-015和SR-001--009，每单元S0先于unit/integration；T015保留整体范围 |
| A182-24 | HIGH / FIXED_IN_DESIGN | 原L0混合Static/compile，易以lint/编译或结构扫描冒充实际逻辑审查 | S0单列源码/设计/测试逻辑对照、walkthrough/finding及证据；L0仍只证明构建边界 |
| A182-25 | HIGH / FIXED_IN_DESIGN | 审查结论没有明确测试scope/逐层入口和修复失效，旧PASS可能放行新代码或更广实验 | hash+AllowedTestScope+TestEntryChecks；BLOCK修复复审，行为变化STALE；具名RED不放行正常测试 |

## Readiness Scorecard

| Rubric / gate | Verdict | Evidence / limit |
| --- | --- | --- |
| Intent fidelity | PASS for design | 完整 native model-to-result；Python 仅兼容，模型不进 Core |
| Necessity and scope | PASS for design | CD-013/014 修复真实断点；不新增网络协议、模型或性能目标 |
| Architecture / ownership | PASS for design | Core packet trust；DI offer policy/plan；Provider 独立授权；model task adapter |
| Cross-artifact consistency | PASS for revision 2 checks | 16 FR、8 SC、14 CD、14 PO、17任务；链接/依赖检查见 evidence |
| Code reality | PASS for reviewed scope | exact workspace 来源，指明 planned 接口；不等于穷尽 O-004 |
| Security / distributed correctness | BLOCK for implementation | admission/cancel/rollback 已定义；完整 wire/journal/权限字段仍由 O-004 控制 |
| Task executability | BLOCK | T001 需关闭 OPEN 和大批次叶子契约；后续任务不可绕过 |
| Validation design | PASS for design | 独立 oracle、定向 counterfactual、每单元先S0再测试；harness先S0/自检后T015整体audit，最后正式验收 |
| Evidence integrity | PASS for design | revision 1/2历史与合并快照分开；合并unit/integration记录只读取，持续源码漂移已记录 |
| Migration / rollback | BLOCK for final inventory | 已定义分类/删除/混合版本/持久化回退契约；逐 caller 数据清单仍 O-004 |
| Performance / operations | PASS for scope | 不承诺语言带来加速；deadline/有界队列/清理身份保留；SIF/Tiger 外部承担 |
| Documentation quality | PASS for draft | 英文 IDs/markers，中文叙述；historical evidence 不回填为 revision 3 |
| Parameters / state / reuse | BLOCK for leaf signatures | native ports 和 authority 明确；未知字段不得由 implementation improvisation 补齐 |
| Delivery / uncertainty | BLOCK | O-001--005 未完成；原生 ABI/工件/安装闭合仍待验证 |

## Task Cohesion Review

- T002 安装链接与 T010 完整 requester 有独立验收，不用 stub 伪造同一功能完成。
- T003--007 各有不同语义 oracle（策略、wire、grant、装配字节、token），分开合理。
- T008 admission 与 model I/O 是独立信任/资源边界；当前是设计批次，T001 必须细化。
- T009 host 提供 T012 Provider 绑定所缺的先决接口，不重写 runtime。
- T012 绑定与 T013 所有调用方/旧路径退出的 gate 不同，有独立迁移价值。
- T014 harness、T015 审计、T016 正式运行有明确依赖和不同交付，不把一次测试机械拆单。
- 每个实现单元包含自己的S0读码/修复复审和定向测试/回归/证据；17任务不机械扩张。

## Traceability and Counterfactual Review

无新增用户无关功能。CD-013/PO-013 补 FR-001/002/004/009/016；
CD-014/PO-014 补 FR-001/009/010/012。追踪表覆盖全部 FR/CD/T/PO。
以下错误实现必须被证明设计检出：Python strategy/runner/helper 回接、
伪 ACK trust、harness 代做准备、错 publication 身份、取消后晋升旧结果、
Provider stop 误停共享服务、未知 journal 自动降级、审计后更改 collector。

## Verdict and Next Actions

本轮已修正静态审查时点、语义审查与编译混淆、范围和失效门禁；原符号/依赖未决项保留。
整体仍 **DRAFT / BLOCK for implementation**，不能把结构 PASS 写成 READY。
下一步在合并修复稳定后执行 T001，冻结原生依赖、兼容字段/调用方、
隔离设计并细化工作单元，再对相应范围作 readiness review。
