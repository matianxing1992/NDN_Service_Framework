# Spec Kit Audit Principles

**Authority**: Supersedes any ad-hoc audit criteria in individual specs.
All Spec Kit audits (pre-implementation and post-implementation) MUST apply
these principles in order. Each principle is a gate; skip none.

## 1. 意图一致性 (Intent Fidelity)

准确落实用户需求、导师意见、禁止事项和冻结边界。不能偷换目标。

- Does the spec deliver exactly what was asked?
- Does it respect explicit exclusions and frozen boundaries?
- Is every "rejected alternative" honestly represented?
- Are advisor/committee constraints reflected?

## 2. 必要性与 Occam 原则 (Necessity & Occam)

每个机制和任务都必须对应具体问题。已有机制能解决时不得重新造轮子。

- Does every new struct, enum, method, and task solve a concrete problem?
- Can an existing mechanism satisfy the requirement with less change?
- Is each addition simpler than the alternative it replaces?

## 3. 架构与归属 (Architecture & Ownership)

通用机制放 Core，应用策略留在 APP。不得把 UAV、codec、workload 特判写入 Core。

- Does each changed file own the right layer of abstraction?
- Are there workload-specific branches (UAV, audio, codec, sensor) in Core?
- Is the boundary between Core, StreamFacade, App, and ServiceProvider clean?

## 4. 跨文档一致性 (Cross-Document Consistency)

spec → plan → contracts → tasks → traceability → success criteria → evidence
必须完整可追溯，术语、默认值和 API 一致。

- Are all FRs traced to tasks? All SCs to verification?
- Do plan.md and contracts/ use the same field names, defaults, and types?
- Do C++ and Python contracts agree field-for-field?

## 5. 代码事实核查 (Code Fact Verification)

用 CodeGraph 对照真实源码、调用者和测试。不能因 tasks 打勾就认定实现完成。

- For each changed API, trace all callers via CodeGraph.
- Verify APIs exist in source and match the contracts.
- Tests actually exercise the changed paths (check coverage via CodeGraph).

## 6. 安全与分布式正确性 (Security & Distributed Correctness)

检查认证、授权、加密、重放、并发、超时、丢包、重排序、restart、stale state、
partial failure 和 fail-closed。

- Signature/identity path: who signs, who validates, what breaks if wrong?
- Token/replay: are tokens consumed? Is replay detected?
- Concurrency: are there races in route registration, buffer ops, status?
- Network: timeout, Nack, loss, reorder, duplicate — handled or fail-closed?
- Session: stale epoch, unknown mode, mixed checkpoint → rejected?

## 7. 任务可执行性 (Task Executability)

每个任务必须包含明确目标文件、依赖、行为变化和验收证据。不能为了显得详细而
过度拆成大量机械任务。

- Does each task have specific files, a clear behavioral outcome, and a gate?
- Are tasks cohesive (one behavior, one owner, one gate)?
- Is there evidence of mechanical fragmentation (write test → implement → run test split)?

## 8. 验证设计 (Validation Design)

测试规模和风险匹配。网络功能优先 MiniNDN。基线与处理组必须保持拓扑、负载、
时长、配置一致。

- Do tests match risk level (unit for internal, MiniNDN for network/security/perf)?
- For any comparative claim: are topology, load, duration, and config identical?
- Is MiniNDN the default for final validation (not host NFD)?

## 9. 证据完整性 (Evidence Integrity)

严格区分四层，不允许把"代码写了"包装成"实验验证通过"：

1. **proposed** — 文档声称 (spec.md, plan.md)
2. **implemented** — 代码实现 (source compiled, passing unit tests)
3. **executed** — 测试执行 (test/experiment ran, output captured)
4. **measured** — 实验测量 (reproducible MiniNDN campaign, statistically valid)

Each SC must declare which layer it satisfies. Layer 4 evidence from one
experiment cannot be silently reused for a different claim.

## 10. 冻结证据保护 (Frozen Evidence Protection)

正式矩阵不能选择性补跑、调参覆盖失败或丢弃负结果。

- Are frozen Spec result directories listed with their canonical hashes?
- Does the changed-file boundary exclude frozen experiment artifacts?
- Are negative results preserved and honestly reported?

## 11. 迁移与回滚 (Migration & Rollback)

兼容路径、feature flag、旧 API 和临时实现必须有删除条件、负责人和回滚方法。

- Is the existing API preserved with identical behavior?
- Can old peers reject the new mode? Can the new mode be rolled back?
- Are temporary/deferred items (e.g., tombstone semantics) explicitly listed
  with owner, conditions, and deletion criteria?

## 12. 结论门禁 (Verdict Gate)

- **BLOCK**: 存在 CRITICAL 或可推翻设计的 HIGH 问题。实现不得开始或继续。
- **CONDITIONAL PASS**: 只有明确、有限的修复项。实现可开始但修复必须在 merge 前完成。
- **PASS**: 需求、设计、任务、代码与证据真正闭环。

## Audit Conduct

- **默认只报告问题，不自动修改**。只有用户明确要求"审核并修复"时，才修改 Spec。
- **四层分开报告**：每次审计结论必须将"文档声称、代码实现、测试执行、实验测量"
  四层分开，不得混为一谈。
- **CodeGraph 证据必须具体**：每个 call-chain 发现必须引用具体文件:行号。
- **审计结果写入** `evidence/pre-implementation-audit.md`（实现前）或
  `evidence/post-implementation-audit.md`（实现后）。
