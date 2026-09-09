# R6-B1 Collector Verdict Boundary

日期：2026-09-08。此批次完成 T014-A 的本地 collector/status 语义修正，未启动
MiniNDN、namespace、网络请求或 C++ 构建。目标是保持观察失败与已观察到的业务/隔离
违反之间的边界，避免 collector 失败被误报为协议结果。

## Scope and allocation basis

本批只包含 `tests/standalone/run-spec182-native-closure.py` 的
`collect_trace`/`evaluate_case` 与对应独立 Python fixtures。它们共享同一 standalone
入口、同一 trace/evidence contract、同一可观察 status 出口，并可在不改变 native
source closure 的情况下独立验证；T014-B harness registration、真实 child/namespace/
endpoint 观察和 T016 资格不属于本批。

五 lane coverage matrix：

| Lane | Coverage | Evidence |
| --- | --- | --- |
| production entry/callers | covered for standalone collector entry | `collect_trace` and `evaluate_case` are the only production harness functions in scope; real MiniNDN callers remain T016 |
| implementation and wire | covered for verdict state boundary; N/A for native wire | integrity violations and policy violations are stored separately; no native API/header changed |
| test/harness/oracle | covered for local independent fixtures; gap for real isolation | `tests/python/test_spec182_native_closure.py` supplies named trace, timeout, evidence and policy fixtures; it does not fabricate namespace PASS |
| build/source closure | N/A for C++; covered for Python source syntax | `py_compile` and diff checks ran; no native target or extension changed |
| migration/evidence | covered for local evidence contract; gap for T016 qualification | this record and `tasks.md` preserve `FAIL`/`UNQUALIFIED` boundary; real process-tree, namespace, socket and cleanup evidence remain open |

## Review trace

- Skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`
- SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`
- Baseline: `52123b1c`
- Diff scope: collector, its Python tests, `tasks.md`, and this evidence record
- Review query: status state machine, trace completeness, required evidence, timeout/cleanup,
  exit-code mapping, independent expected outcomes, and no native ownership change
- Finding: prior code used one `violations` list to mark every observation incomplete, causing
  an observed policy violation to become `FAIL` for the wrong reason and preventing a meaningful
  exit-2 observation boundary. The patch separates `integrityViolations` from
  `policyViolations`; re-review found no additional actionable issue.

## Implementation result

`collect_trace` now treats missing or unpaired trace data as incomplete observation while
retaining policy violations as complete observations. `evaluate_case` returns:

- `PASS` when required evidence, expected exit, observation integrity and policy checks all pass;
- `FAIL` when a complete observation reports a Python mapping/exec, undeclared endpoint, or
  business exit mismatch;
- `UNQUALIFIED` when the run times out, required evidence is missing, or trace observation is
  incomplete.

This preserves the runner contract of exit `0` for complete PASS, exit `1` for business/isolation
FAIL, and exit `2` for preflight/observation `UNQUALIFIED`. The tests use independent expected
statuses and do not call the evaluator to generate their oracle.

## Validation

```text
python3 -m py_compile tests/standalone/run-spec182-native-closure.py tests/python/test_spec182_native_closure.py
# exit 0

PYTHONPATH=pythonWrapper:NDNSF-DistributedInference \
  python3 -m pytest -q tests/python/test_spec182_native_closure.py
# 12 passed in 0.15s

python3 - <<'PY'
from pathlib import Path
import ast
for name in ('tests/standalone/run-spec182-native-closure.py',
             'tests/python/test_spec182_native_closure.py'):
    ast.parse(Path(name).read_text(encoding='utf-8'), filename=name)
print('static source assertions: PASS')
PY
# exit 0

git diff --check
# exit 0
```

Compile/build misses: none observed. Runtime/test misses: real bubblewrap/strace observation,
descendant lifecycle, network endpoint allow-list and MiniNDN campaign were not run. No C++
build was applicable because no native source/header or build registration changed.

## Result and closure

`STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`; not
`QUALIFICATION_PASS`.

Closure decision: `CLOSED_FOR_VALIDATION` for the local collector verdict semantics. The next
dependency is T014-B harness registration; T016 still owns real namespace, short-lived child,
socket, cleanup and no-Python qualification, so this evidence does not close T014-A globally or
the Spec182 migration.
