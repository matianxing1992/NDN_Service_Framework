# Spec184 Qualification Matrix

**Status**: PLANNED / rows require T006 binding
**Source**: [transfer matrix](transfer-matrix.md) and inherited Spec182 proof contracts

本表是最终资格的唯一行级入口。迁移记录的结构检查不能关闭任何一行；每行必须绑定
当前 candidate、源码/二进制身份、真实 C++ target/selector 或外部 owner、负例、所有子
进程 exit、cleanup 和 evidence path。`TRANSFERRED` 只表示归属，`PASS` 只允许来自当前
candidate 的完整结果。

## Inherited Parent Obligations

| Row | Inherited obligation | Spec184 owner | Required evidence | Initial status |
| --- | --- | --- | --- | --- |
| 182:T004 | canonical seal/非法投影与 Core/Provider 真实接收资格 | T006/T007 | C++ selector、wire/source identity、负例结果 | OPEN |
| 182:T005 | grant IO 缺陷 F-01、签发/验证/消费完整资格 | T001/T006/T007 | `Spec184AuthorityIoOwnership`、真实 authority/provider 结果 | OPEN |
| 182:T006 | cold ONNX 装配 bytes/取消/清理、Python helper 退出验收 | T006/T007 | native assembly target、process exit 和 cleanup | OPEN |
| 182:T007 | tokenizer encode/decode oracle 与无解释器依赖 | T006/T007 | C++ tokenizer selector、dependency/source closure | OPEN |
| 182:T008 | 原生输入/模型/工件准备与 admission 完整验收 | T006/T007 | preparation/admission C++ target、artifact-bound result | OPEN |
| 182:T009 | 共用 Provider host 注册、执行、停止与入口一致性 | T005/T006/T007 | provider target、caller row、stop/cleanup evidence | OPEN |
| 182:T010 | F-01/F-02、完整 request lifecycle/负例 | T001/T002/T006/T007 | B1 selectors、交错/取消/晚回调结果 | OPEN |
| 182:T011 | F-03/F-04、continuation/受限恢复/lineage 剩余资格 | T003/T004/T006/T007 | durable/checkpoint selectors、独立读取与失败保留 | OPEN |
| 182:T012 | 薄绑定支持模式，无 Python strategy/state owner | T005/T006/T007 | binding/source closure、native owner and parity evidence | OPEN |
| 182:T013 | 维护默认路由与 legacy retirement | T005/T006/T007 | caller matrix、default route、zero-use/rollback evidence | OPEN |
| 182:T014 | no-Python harness/collector、依赖排除和负例 | T006/T007 | C++ process/no-Python owner、trace pairing and exits | OPEN |
| 182:T015 | FR/CD/INV/PO、接线、oracle、build、设计总对账 | T006 | row completeness、fresh convergence audit | OPEN |
| 182:T016 | 同源完整 unit/integration、MiniNDN/no-Python 本地资格 | T007 | candidate-bound complete results and child exits | OPEN |
| 182:T017 | 唯一交付基线、维护文档、两入口示例与外部边界 | T008 | final source/bundle identity and handoff | OPEN |

## Required Additional Rows

T006 必须在本表追加一行对应每个 `PO-001`–`PO-016`，以及适用的 `I`, `FR`, `CD`,
`INV`。不得使用 `PO-*` 或 `I-*` 通配行代替实际行。新增行至少包含：

`rowId`, `sourceContract`, `ownerTask`, `productionEntry`, `C++ target/selector or external owner`,
`negative/recovery boundary`, `candidateId`, `source/runtime/config hashes`, `evidencePath`, `status`。

## Gate Rules

任何行缺少 selector/owner、candidate identity、child exit 或首失败边界时保持 `OPEN` 或
`PARTIAL`。T006 的矩阵静态合成和 fresh design-code convergence audit 必须为 `PASS`，
T007 才能开始；T007 的局部 selector PASS 不能提升其他行或整个 Spec 的状态。
