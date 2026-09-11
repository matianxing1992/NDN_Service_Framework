# Spec184 Dynamic Workflow Revision

**Date**: 2026-09-11
**Status**: `DOCUMENTATION_UPDATED` / no product or qualification result changed

本记录固定 Spec184 后续批次采用的最小动态分析循环：`Freeze → Sample → Run → Classify`。
批次先完成五 lane 静态覆盖和组合审查，再在一张 `Dynamic gate card` 中冻结 risk/profile、
具名 C++ selector、源码/工具链身份、输出目录和预算；参数按风险/行为等价类取少量正常、
边界、非法及生命周期/并发顺序样本。C++ fixture/oracle 判定协议和模型语义，动态工具只
报告内存、线程、未定义行为或解析崩溃；Python 只能编排外部设施或启动 C++ executable。

B5 不为 80 行 qualification matrix 建 80 套动态任务。每个不同继承风险/行为类别默认一条
正例和一条负例，未代表的行仍在 qualification matrix 中保持 `PARTIAL`，并由 candidate-bound
资格运行补齐。任何 `DYNAMIC_FAIL` 或 `NOT_RUN` 都保留首个失败边界；动态无报告不能升级
`QUALIFICATION_PASS`；只有矩阵中已登记 case 的工具观察和 C++ 业务断言均满足预期时才可写
`DYNAMIC_PASS`。

## Changed artifacts

- `skills/speckit-code-design/SKILL.md`: added the four-step batch loop.
- `skills/speckit-code-design/references/batch-quality-gates.md`: made the loop authoritative.
- `.specify/templates/{spec-template.md,plan-template.md,tasks-template.md}`: future specs inherit
  the batch-level loop and no per-parameter task rule.
- `specs/184-native-di-closure/{spec.md,plan.md,tasks.md}`: applied the procedure and B5 sample
  budget; progress and candidate invalidation are recorded.
- `specs/184-native-di-closure/contracts/{qualification-matrix.md,promotion-candidate.md}`:
  marked the pre-revision candidate and row bindings as historical until a fresh digest is made.

## Review trace

Read-only review used `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`, against baseline
`3aad9712`. Scope was the complete workflow/template/spec184 documentation diff, including
candidate invalidation and all changed status text. Findings: `none`; the stale-candidate note
and the qualification-matrix note were added to prevent old runtime evidence being rebound.

## Current document hashes

```text
spec.md sha256:f20955988edd1f6cd93f376703a47538d9039f519bf71f5bf9f377d616926940
plan.md sha256:a055d66e73140c6b517891f3c8b276bbe5d14e8a3d4e4604ba6dea6109e01e6d
tasks.md sha256:7288f73ed49f57b76bc66eccc2dbdf5fda82f40b7d35d5684dc4ac8400d73a62
qualification-matrix.md sha256:6afbb1c76f8abf4e9b151b6729dd69d50ae7e5ea5bf3773167dbd1ea4d2a462e
```

## Validation

```text
python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py --require-entrypoints --require-personal
python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py specs/184-native-di-closure --strict
git diff --check
```

These checks cover workflow/template synchronization, Spec structure and whitespace only. No C++
build, dynamic run, model run, MiniNDN run, SIF/Tiger run, or qualification verdict was produced by
this documentation change. The previous candidate is stale because the validation contract hashes
changed; a fresh convergence audit and candidate digest are required before T007 resumes.
