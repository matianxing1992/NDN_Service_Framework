# Local Gate Source Identity

**Status**: BLOCK
**Evidence layer**: source inspection / executed (fixture children only)

## First Boundary

维护 `scripts/run_spec180_local_gate.py::run_local_gate` 校验清单字段、
条目脚本/二进制哈希和命令形状，但未把 `sourceRevision` 与实际
checkout 核对。`spec180_inventory.py` 仅检查该字段的字符串格式；
因此条目哈希正确并不能证明整个运行源码属于声明的提交。

使用现有 `test_spec180_local_gate.py` 的 fixture 构造器和真实 gate
执行函数复现：`/tmp/spec181-gate-identity-0fj11yan` 不存在 Git HEAD
（查询退出 128），清单填入 40 个 `b` 的虚构 sourceRevision，gate
仍返回 **PASS**。六个 fixture 子进程均退出 0、cleanup PASS。
这些子进程只执行小型脚本/测试，没有 MiniNDN、模型或正式资格运行。

原始结构化记录保留在 ignored workspace temporary directory 的
`spec181-local-gate-identity-20260906-r1/probe.json`；fixture 与输出目录
保留在上述临时路径。该 PASS 是需要修复的 gate 裁决，不是资格证据。

## Repair Boundary

下一单元为 T008 的正式 local gate 加入实际 checkout/提交与未提交
源码校验，并证明错误身份在任何子进程和结果目录创建前被拒绝。
既有缺 oracle、崩溃和清理 fixture 应使用明确封存的对应源码，不能
绕开新门。`effectiveConfigDigest` 当前亦为传入声明，其实际消费
绑定语义仍须核查；本记录不声称已确定或关闭整个配置封印设计。

T007 仍 BLOCK，整体保持 5/12；不先运行正式资格矩阵发现这些基本
身份缺口。CodeGraph 的旧调用图曾把无实际 import 的生成器关联到
候选 helper，已用当前源中的精确 import 搜索排除，不据此引入依赖。
