# Spec182 Transfer Matrix

**Date**: 2026-09-11 | **Baseline**: `94c1e644` | **Status**: TRANSFERRED / acceptance INCOMPLETE

## Authority

来源：[182 tasks](../../182-native-di-python-bindings/tasks.md)、
[request-chain audit](../../182-native-di-python-bindings/evidence/request-chain-static-audit-20260911.md)。
已完成182:T001/T002/T003不重复建任务，只保留基线；其余14个父任务逐项如下。
TRANSFERRED 是执行归属，不是行为 PASS。182 原 checkbox 保持原值。

## Open Parent Obligations

| Source | Remaining obligation | Spec184 owner |
| --- | --- | --- |
| 182:T004 | canonical seal/非法投影与 Core/Provider 真实接收资格 | T006/T007 |
| 182:T005 | grant IO 缺陷 F-01、签发/验证/消费完整资格 | T001/T006/T007 |
| 182:T006 | cold ONNX 装配 bytes/取消/清理、Python helper 退出验收 | T006/T007 |
| 182:T007 | tokenizer encode/decode oracle 与无解释器依赖 | T006/T007 |
| 182:T008 | 原生输入/模型/工件准备与 admission 完整验收 | T006/T007 |
| 182:T009 | 共用 Provider host 注册、执行、停止与入口一致性 | T005/T006/T007 |
| 182:T010 | F-01/F-02、完整请求 lifecycle/负例 | T001/T002/T006/T007 |
| 182:T011 | F-03/F-04、continuation/受限恢复/lineage 剩余资格 | T003/T004/T006/T007 |
| 182:T012 | 薄绑定支持模式，无 Python strategy/state owner | T005/T006/T007 |
| 182:T013 | 维护默认路由与 legacy retirement | T005/T006/T007 |
| 182:T014 | no-Python harness/collector、依赖排除和负例 | T006/T007 |
| 182:T015 | FR/CD/INV/PO、接线、oracle、build、设计总对账 | T006；各批静态门 |
| 182:T016 | 同源完整 unit/integration、MiniNDN/no-Python 本地资格 | T007 |
| 182:T017 | 唯一交付基线、维护文档、两入口示例与外部边界 | T008 |

## Audit Batch Mapping

| Source | Destination | Meaning |
| --- | --- | --- |
| R12-A / F-01,F-02 | B1 / T001,T002 | 请求线程/取消 |
| R12-B / F-03 | B2 / T003 | durable outcome |
| R12-C / F-04 | B3 / T004 | 安全导出 |
| R12-D / G-01 | B4 / T005 | 当前 caller/mode |
| R12-E / G-02 | B5 / T006–T008 | 组件、harness、资格、交付；不是只测试四个修复 |

## Inherited Contracts and Evidence

以下文档冻结引用，不复制第二套大契约；其旧执行顺序不再生效：
[code-design](../../182-native-di-python-bindings/contracts/code-design.md)、
[proof-design](../../182-native-di-python-bindings/contracts/proof-design.md)、
[work-units](../../182-native-di-python-bindings/contracts/work-units.md)、
[native-first](../../182-native-di-python-bindings/contracts/native-first-execution.md)。
182 原19 FR、11 SC、CD/INV、PO-001–016及适用 I 矩阵作为继承验收基线，要求不因新 FR 编号变少而删除。
T006 负责将每个未关闭 selector/模型/负例列入新矩阵；已有实现的义务可以“待验证”，不能误标“待重写”。
历史 R11 各 G 卡的未完成内容按父任务归属承接；如发现新增目标或真正重复项须写依据后处理，不能默认删除。

当前可复用正例和首失败边界见
[cross-process evidence](../../182-native-di-python-bindings/evidence/r11-b10-g13-current-cross-process-chain-20260910.md)
与 [failure index](../../../docs/failure-log.md)。原始日志仍在旧目录，历史引用不改名搬走。
Spec184 新执行证据写自己的 evidence/ 与新 run-id；不得覆盖182原始记录。
SIF/Tiger 属实验机工作，保持外部资格边界；本地 T007 不能被一次 tiny process PASS 替代。
