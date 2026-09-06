# Spec182 Design Audit

**Revision**: 1 | **Verdict**: DRAFT / BLOCK for implementation
**Evidence layer**: source review / design review only; runtime NOT_RUN
**Baseline**: evidence/design-baseline.json
这是 design readiness 自审，不是 post-implementation convergence PASS。

## Findings

| ID | Severity / status | Finding / evidence | Required disposition |
| --- | --- | --- | --- |
| A182-01 | BLOCK / OPEN | Spec181 尚未关闭；baseline 含 17 个 workspace-existing 路径 | T001 读取最终交付并刷新 O-001 |
| A182-02 | BLOCK / OPEN | native assembler 实际 runPythonHelper；原生 ONNX/protobuf 字节复现未证明 | O-002 固定依赖/API/向量，T006 实现前关闭 |
| A182-03 | BLOCK / OPEN | native tokenizer 当前 fork/exec Python；native backend ABI 未冻结 | O-003 冻结原生库/内存/线程/安装契约，T007 前关闭 |
| A182-04 | BLOCK / OPEN | Python 公开/兼容接口、持久化和所有 callback caller 尚未穷举；部分 planned types 为迁移语义索引 | O-004 补全 signature/field/caller manifest；T002--011 前关闭 |
| A182-05 | BLOCK / OPEN | no-Python gate 尚未证明能发现 renamed/embedded/remote helper | O-005，T012 的反例必须先于正式 T014 |
| A182-06 | REVIEW / OPEN | T003--009 的批次部分超过原子变更建议，selectors 未冻结 | T001 沿独立行为细化预算、命令、恢复点；不直接交大包实施 |
| A182-07 | PASS / design only | 完整 C++ 请求、原生策略、动态冷装配/文本、Python 薄绑定、Core 边界均明确 | FR-001--016；不是实现证据 |
| A182-08 | PASS / design only | 本地开发与实验机器责任分离；181 未被提前关闭/切换 | INV-008/009，最终以 authoring hash check 核对 |

## Readiness Gate Review

| Gate | Verdict | Rationale / controlling artifact |
| --- | --- | --- |
| Intent | PASS | 用户明确完整 native call；短 Python API 不能替代原生闭合 |
| Baseline | PASS for design; BLOCK for implementation | 记录 committed/workspace-existing；等待最终 181 baseline |
| Architecture invariants | PASS | INV-001--009，Core 机制复用、Provider 独立验证、无平行 owner |
| Change inventory | BLOCK | CD 已命名主体和 caller，O-004 完整消费者/叶子清单未闭合 |
| Functions / parameters | BLOCK | public contract 已写；O-004 native types/defaults/桥接签名待冻结 |
| State | BLOCK | owner/终态/清理约束已写，旧 journal 字段迁移待 O-004 |
| Call flow / reuse | PASS at architecture level | FLOW-001--004，不把模型差异移入 Core |
| Compatibility | BLOCK | O-004 未关闭，禁止静默 fallback |
| Delivery | BLOCK | 独立库目标明确；O-002/O-003 的版本/ABI/lock 尚未关闭 |
| Traceability | PASS for draft | FR/CD/T/PO 双向覆盖；结构检查单独记录 |
| Evidence / proof obligations | PASS for design | 独立向量、真实入口、语义 counterfactual；没有宣称运行 PASS |
| Decision boundary / expected diff | PASS for draft; BLOCK for execution | 每单元预算/禁改/恢复；大批次 T001 需细化 |
| Negative paths / ladder | PASS for design | 授权、取消、失败、旁路、首边界明确 |
| Completion record | PASS for design | 实际完成字段与三种状态分开，所有实现未勾选 |
| Uncertainty | BLOCK | O-001--005 显式控制，不用细节猜测补齐 |
| Consistency | See evidence/design-review.md | 文档检查通过不消除上述语义 OPEN |

## Counterfactual Walkthrough

- 只把 Python 类包成 C++ 回调：FR-003/SC-001/PO-009 不通过。
- C++ requester 启动 Python 装配或 tokenizer：PO-001/005/006 不通过。
- 全部提前预切并只测 warm：FR-016/PO-005 不通过。
- Provider 复用类型后删除独立授权：INV-003/PO-004 不通过。
- 只迁 YOLO 不迁 Qwen 文本/状态：SC-005 不通过。
- 两边各自生成 expected digest：PO-003 独立 oracle 不满足。
- 把 Data wire-size/启动失败作为正确拒绝：N10/PO-011 不通过。
- 把文档创建当完成：tasks 0/15、Execution NOT_STARTED，不能通过关闭门。

## Verdict

本轮可以交付完整的后续设计草案，但不能标 READY_FOR_IMPLEMENTATION。
完成的工作是需求/架构与可审查的迁移、证明设计；待关闭的细节和事实有明确 owner、
有界调查和受阻任务。继续 Spec181，之后 T001 关闭控制性 OPEN 并重新审查。
