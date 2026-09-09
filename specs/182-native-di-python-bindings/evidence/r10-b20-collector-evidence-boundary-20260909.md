# R10-B20 Collector Evidence Boundary — 2026-09-09

## Scope and boundary

R10-B20 closes the evidence-generation gap exposed by R10-B19. The canonical
collector now derives `identity`, `process-tree`, `namespace`, `exec-map`,
`endpoints` and `cleanup` from actual `run`/trace observations. A case may
declare one independent `businessOracle.stdoutMarker`; the marker can provide
only `business-oracle` when it is observed in captured stdout. No protocol
result, native DI success or T016 qualification is inferred from a marker.

Missing trace, incomplete strace pairing, timeout, Python/policy violation or a
missing declared marker remains fail-closed under the existing evaluator.

## Source and review trace

- Source baseline: `1fed7929` (`test: wire spec182 owner to native runner`).
- Batch diff: `tests/standalone/run-spec182-native-closure.py`,
  `tests/python/test_spec182_native_closure.py`, and the R10-B20 task/plan
  registration. Pre-existing native dependency/generation contract changes were
  excluded.
- Official review skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`.
  SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`.
- Review result: **No findings.** The read-only review followed the collector
  call path, manifest validation, run record, test/oracle fixtures and default
  registration-only route. No introduced correctness defect was found.

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` | `run_case`, `collect_trace`, `evaluate_case`; owner `_execute_runner_case` | `rg -n "collect_trace\\(|evaluate_case\\(" tests Experiments`; call-path read | Canonical owner and CLI paths use the same collector/evaluator; no duplicate verdict path. |
| `implementation and wire` | `covered` | `load_case`, `run_case`, `collect_trace` in `tests/standalone/run-spec182-native-closure.py` | `sed -n '131,190p'`; `sed -n '335,430p'`; `git diff` | Evidence requires observed PID/exec/exit/command/run state; marker validation is bounded and stdout-only. |
| `test/harness/oracle` | `covered` | `tests/python/test_spec182_native_closure.py` trace and marker fixtures | `pytest -q tests/python/test_spec182_native_closure.py`; selector registration read | 31 cases pass; marker-present case reaches `PASS` only with all evidence, marker-missing case remains `UNQUALIFIED`. |
| `build/source closure` | `N/A` | Python collector/harness only | `python3 -m py_compile ...`; `git diff --check` | No C++ source, target or ABI changed; native build is not applicable. |
| `migration/evidence` | `covered` | `tasks.md`, `plan.md`, R10-B20 evidence and fresh raw run | `validate_design.py`; inspect `.codex-tmp/spec182-r10-b20-20260909054500/` | Durable evidence records the six derived classes and the remaining business-marker boundary; T014/T016 remain open. |

## Validation

```text
pytest -q tests/python/test_spec182_native_closure.py
31 passed in 0.18s
python3 -m py_compile tests/standalone/run-spec182-native-closure.py \
  tests/python/test_spec182_native_closure.py
python3 specs/182-native-di-python-bindings/checklists/validate_design.py
ok: true; tasks=17; execution_cards=40; DONE=16; PARTIAL=23; NOT_STARTED=1
git diff --check
```

Fresh root owner/runner output is preserved at
`.codex-tmp/spec182-r10-b20-20260909054500/`. It used the R10-B19 `I01`
`/bin/true` process probe and exited 2. `runner-result.json` records:

- evaluator `status=UNQUALIFIED` with
  `MISSING_EVIDENCE:business-oracle`;
- process `returncode=0`, `observation.complete=true`,
  `integrityViolations=[]`, `policyViolations=[]`;
- evidence `cleanup`, `endpoints`, `exec-map`, `identity`, `namespace` and
  `process-tree`;
- three observed PIDs, one successful native exec and three exit events.

This is the intended boundary: collector evidence is now present and trace
trust is established, but the probe has no independent DI business marker.

## Batch retrospective and closure

- `static`: no introduced regression; the review covered all five required
  lanes and kept business evidence separate from process/isolation evidence.
- `compile/link`: no native lane applies; syntax, design validation and diff
  checks passed.
- `runtime/test`: the fresh owner-alive composition produced complete trace
  evidence and correctly stayed `UNQUALIFIED` for the missing marker.
- `unobserved`: real native DI request/result oracle, counterexample execution,
  maintained caller migration, no-Python proof and T016 qualification.

Closure decision: `CLOSED_FOR_VALIDATION` for collector evidence derivation;
T014 remains partial and T016 remains `PARTIAL/UNQUALIFIED`.
