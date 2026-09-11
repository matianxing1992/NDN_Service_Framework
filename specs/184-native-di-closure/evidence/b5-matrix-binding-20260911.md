# Spec184 B5 Matrix Binding Checkpoint

**Status**: PARTIAL / T006 row binding complete; implementation gaps, candidate promotion and qualification remain open
**Batch**: B5 / T006
**Source checkpoint**: `6e722998`
**Development identity**: `DEV-865e1ee2` (source-only focused checkpoint, not a promoted candidate)

T006 本轮完成资格矩阵的逐项盘点，不把历史迁移或局部 selector 误记为最终资格。矩阵现
明确列出 14 个继承父任务、16 个 `PO`、8 个 isolation `I`、19 个 `FR`、14 个 `CD` 和
9 个 `INV`，共 **80 行**；每行有 owner、入口/selector、负例边界、动态 profile/不变量、
候选身份、证据路径和 `OPEN`/`PARTIAL` 状态。仍未补齐的 native tokenizer/parser、完整
process/no-Python 负例、真实模型输入、完整 wire/qualification 和 handoff bundle 继续
由 T006/T007/T008 负责。

## Matrix validation

第一版矩阵检查脚本把继承父任务的旧六列行误当成新增行，触发
`AssertionError: ('182:T004', 6)`；这是验证器范围错误，不是产品结果。原始输出保留在
`.codex-tmp/spec184-b5-matrix-validation.log`。改变门禁后，检查只对新增行应用完整字段
规则，并重新确认所有 ID 集合和动态矩阵引用；修正命令输出见
`.codex-tmp/spec184-b5-matrix-validation-r2.log`，结果为：

```text
parents: found=14 expected=14 missing=[]
po: found=16 expected=16 missing=[]
isolation: found=8 expected=8 missing=[]
fr: found=19 expected=19 missing=[]
cd: found=14 expected=14 missing=[]
inv: found=9 expected=9 missing=[]
matrix_schema: PASS
dynamic-matrix-reference: PASS (shared gate + Spec184 spec/plan + B4 evidence)
```

## Five-lane coverage

| Lane | Result | Boundary |
| --- | --- | --- |
| production entry / callers | covered | B4 caller matrix 12 rows and current source paths |
| implementation / wire | partial | existing C++ owner and inherited contract references are bound; several cross-process/serialization selectors remain open |
| test / harness / oracle | partial | current B1–B4 selectors and historical I/PO selectors are mapped; complete current candidate run is not executed |
| build / source closure | partial | focused target hashes are recorded; ordered full candidate manifest and dependency/config digest are not frozen |
| migration / evidence | partial | compatibility/rollback and external owner boundaries are explicit; Python retirement, no-Python and Tiger/SIF remain open |

## Dynamic parameter matrix

The shared skill and Spec184 now require one bounded matrix per logical batch. T006 rows reference
the owning batch profile instead of creating a dynamic task for every parameter. The C++ oracle
owns expected success/rejection/cancellation semantics; sanitizer, TSan or parser-fuzz output is
only the memory/thread/undefined-behaviour observation. Uncovered values are listed as qualification
rows rather than silently omitted.

## Review trace and next gate

The read-only review uses `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`, against checkpoint
`6e722998`; scope is the qualification matrix, promotion-candidate status and Spec184 task/plan
references. No product source changed. `Closure decision: OPEN_FOR_NEXT_BATCH` because the matrix
is structurally complete but the candidate is not promoted and required runtime rows remain open.
The next work is to bind exact current selectors/artifact/config hashes, close missing component
oracles, and run a fresh design-to-code convergence audit before T007.

