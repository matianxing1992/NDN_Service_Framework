# R6-B8 Collector Role and Cold-Path Coverage

日期：2026-09-09。本批修正 T014-A collector evaluator 对声明角色和 cold path
观测的漏检。没有修改 C++ 运行时、协议 wire 或 MiniNDN owner；真实 namespace、child、
endpoint 和资格执行仍由 T016 负责。

## Scope and allocation basis

本批只包含 `tests/standalone/run-spec182-native-closure.py` 的 manifest 校验与
`evaluate_case` 状态判定，以及独立 Python fixtures。它们共享 T014-A 的观察出口，能够
在不改变 native source closure 的情况下验证“缺观测”和“观测到错误角色/冷暖状态”的
边界；T014-B 注册和 T016 真实运行不属于本批。

## Coverage matrix

| Lane | Coverage | Evidence |
| --- | --- | --- |
| production entry/callers | covered for standalone evaluator | `load_case` and `evaluate_case`; MiniNDN caller remains T016 |
| implementation and wire | covered for evidence verdict; N/A for native wire | declared `requiredRoles` and `cold` markers are validated and consumed only by collector verdict logic |
| test/harness/oracle | covered for independent local fixtures; gap for real isolation | missing, valid, duplicate, mismatched role/cold observations are asserted without deriving expected status from the evaluator |
| build/source closure | N/A for C++; covered for Python source syntax | `py_compile`, focused pytest and `git diff --check` |
| migration/evidence | covered for local status boundary; gap for T016 | this record preserves `PASS` only with complete role/cold observation and leaves real node/netns execution open |

## Review trace

- Skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`
- SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`
- Baseline: `04f361e7`
- Diff scope: collector, its Python tests, and this evidence record
- Review query: optional manifest field validation, missing-versus-mismatched observation
  status, duplicate role handling, required evidence interaction, and no qualification claim
  without real namespace context
- Finding: the first review found duplicate role observations could collapse to a set and pass;
  duplicate entries now produce `ROLE_OBSERVATION_INVALID` and `UNQUALIFIED`. No further
  actionable finding remains.

## Implementation result

`load_case` validates optional `requiredRoles` and `cold` markers. `evaluate_case` now treats a
missing or malformed role/cold observation as `UNQUALIFIED`, while an observed role-set or
cold/warm mismatch is a complete `FAIL`. A complete `PASS` requires the declared roles exactly
once and the declared cold marker. Existing trace-integrity and policy-violation boundaries are
unchanged.

## Validation

```text
/usr/bin/python3 -m py_compile \
  tests/standalone/run-spec182-native-closure.py \
  tests/python/test_spec182_native_closure.py
# exit 0

PYTHONPATH=pythonWrapper:NDNSF-DistributedInference \
  /usr/bin/python3 -m pytest -q tests/python/test_spec182_native_closure.py
# 18 passed in 0.13s

git diff --check
# exit 0
```

No native build was applicable. No MiniNDN, namespace, NFD, endpoint, child-process or
cross-process qualification run was attempted.

## Result and closure

`STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`; not
`QUALIFICATION_PASS`.

Closure decision: `CLOSED_FOR_VALIDATION` for local role/cold evaluator semantics. T014-A still
depends on real process and namespace observation, and T016 remains the owner of final
qualification.
