# R6-B2 Qualification Harness Registration

日期：2026-09-08。此批次登记 T014-B 的唯一 campaign manifest 和 MiniNDN owner 边界；不
启动 namespace、NFD、网络请求或 C++ 构建。真实资格仍由 T016 在具名 node/netns 上执行。

## Scope and allocation basis

本批共享一个稳定生产入口：`Experiments/NDNSF_DI_NativeClosure_Minindn.py` 只负责 campaign
registration/资源边界，所有被测观察仍转交
`tests/standalone/run-spec182-native-closure.py`。接口契约是
`spec182-native-qualification-v1` 的 case IDs、T016 owner、`runSeconds`/
`cleanupSeconds`/`traceBytes` limits 和 fresh output directory。独立 Python selector
验证注册与缺 node context 的结果记录；没有 native source 或 build registration 变化。

Coverage matrix：

| Lane | Coverage | Evidence |
| --- | --- | --- |
| production entry/callers | covered | `run_campaign` and `load_registration` are the maintained campaign owner; canonical runner path is checked |
| implementation and wire | covered for registration boundary; N/A for DI wire | manifest schema, case owner, expected status and limits are validated; no product protocol is executed |
| test/harness/oracle | covered for registration/fresh-run behavior; gap for real harness | two dedicated tests independently check 22 IDs and persisted `UNQUALIFIED`; no test fabricates a qualification PASS |
| build/source closure | N/A for C++; covered for Python/JSON syntax | `py_compile`, JSON parse and Spec structure validator pass; no native target changed |
| migration/evidence | covered for durable manifest/evidence; gap for T016 | `case-manifest.json` is the single registration source; real node setup, collector trace, cleanup and PO outcomes remain T016 |

## Registration contract

The frozen manifest now contains 22 explicit records: counterexamples `I01` through `I08` and
acceptance owners `PO-001` through `PO-014`. Each record has a unique ID, an expected status and
`executeOwner: T016`. `load_registration` rejects a missing/wrong schema, non-canonical runner,
duplicate or unowned case, invalid expected status, missing required ID, and non-positive limits.

`run_campaign` resolves the output path and requires a new directory. With no externally supplied
MiniNDN node context it writes a durable result containing the selected case, registered IDs,
runner and limits, with status `UNQUALIFIED` and reason
`MININDN_NODE_CONTEXT_NOT_PROVIDED`. It never turns registration or startup into a business
result and does not implement a second collector.

## Review trace

- Skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`
- SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`
- Baseline: `194155b4`
- Diff scope: campaign owner, frozen case manifest, native-closure Python tests, `tasks.md`,
  and this evidence record
- Review query: schema and ID completeness, canonical runner binding, T016 ownership, deadline/
  cleanup, fresh output semantics, exception handling, no second collector and no qualification
  claim without node context
- Finding: the first review caught that an existing output directory could be mutated on an
  invalid-manifest exception path; the owner now rejects pre-existing output before any write,
  and the dedicated regression test confirms the directory is unchanged. Re-review found no
  additional actionable issue. The expected `UNQUALIFIED` boundary is retained when MiniNDN
  metadata is absent.

## Validation

```text
PYTHONPATH=pythonWrapper:NDNSF-DistributedInference \
  python3 -m pytest -q tests/python/test_spec182_native_closure.py \
    tests/python/test_spec182_legacy_exclusion.py
# 21 passed

python3 -m py_compile Experiments/NDNSF_DI_NativeClosure_Minindn.py \
  tests/standalone/run-spec182-native-closure.py tests/python/test_spec182_native_closure.py
# exit 0

python3 - <<'PY'
import json
from pathlib import Path
d=json.loads(Path('tests/fixtures/spec182/case-manifest.json').read_text())
q=d['qualification']
assert q['schema']=='spec182-native-qualification-v1'
assert q['runner']=='tests/standalone/run-spec182-native-closure.py'
ids={item['id'] for item in q['cases']}
assert ids >= {f'I{i:02d}' for i in range(1,9)} | {f'PO-{i:03d}' for i in range(1,15)}
print('manifest registration: PASS', len(ids))
PY
# exit 0; 22 IDs

python3 specs/182-native-di-python-bindings/checklists/validate_design.py
# ok=true

git diff --check
# exit 0
```

Compile/build misses: none; C++ build is not applicable. Runtime/test misses: no MiniNDN
node/netns, NFD, socket, process-tree, collector trace, cleanup, or PO execution was attempted.

## Result and closure

`STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`; not
`QUALIFICATION_PASS`.

Closure decision: `CLOSED_FOR_VALIDATION` for manifest registration and fresh-run refusal. T016
still owns real I01--I08 and PO execution; T015 convergence and T016 qualification cannot be
marked complete from this local harness record.
