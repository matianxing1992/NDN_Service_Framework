# R10-B19 Owner-to-Runner Handoff Evidence — 2026-09-09

## Scope and boundary

R10-B19 closes the bounded接线 from the tracked MiniNDN owner to the canonical
Spec182 native-closure runner. `--execute-owner --runner-manifest` keeps the
requester/provider namespaces and NFD applications alive while the runner
loads, stages, launches, observes and evaluates one selected case. The owner
writes the node context and the canonical runner result before cleanup.

This batch does **not** claim native DI business success, maintained-caller
execution, cross-process request success, no-Python qualification, or T016
qualification. The selected `/bin/true` case is an isolation/process probe and
has no business oracle evidence; its final status therefore remains
`UNQUALIFIED`.

## Source and review trace

- Source baseline: `da358746` (`docs: record spec182 runner trace boundary`).
- Reviewed diff: `Experiments/NDNSF_DI_NativeClosure_Minindn.py` plus the R10-B19
  task/checkpoint entry and this evidence record. Pre-existing changes to
  `contracts/native-dependency-design.md` and
  `contracts/native-generation-design.md` were excluded from the batch.
- Official review skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`.
  SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`.
- Project review profile: `skills/speckit-code-design/references/review-agent.md`.
- Review result: **No findings.** The review read the owner entry, runner call
  sites, lifecycle/cleanup, manifest selection, test registration and the
  canonical runner implementation. No introduced correctness defect was found.

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` | `main`, `run_campaign`, `_run_owned_campaign` in `Experiments/NDNSF_DI_NativeClosure_Minindn.py` | `rg -n "run_campaign|runner_manifest|runner_case|_run_owned_campaign" Experiments tests`; root owner command below | Runner options are gated by `--execute-owner`; default registration-only route is unchanged. |
| `implementation and wire` | `covered` | `_execute_runner_case`, `run_case(..., nodes=...)`, `collect_trace`, `evaluate_case` | `sed -n '160,290p' Experiments/NDNSF_DI_NativeClosure_Minindn.py`; `sed -n '250,390p' tests/standalone/run-spec182-native-closure.py` | Live node context is passed through the canonical runner; owner cleanup remains in `finally`; no second collector or business oracle was added. |
| `test/harness/oracle` | `covered` | `tests/python/test_spec182_native_closure.py`; frozen case registration; root owner/runner output | `pytest -q tests/python/test_spec182_native_closure.py`; `load_registration`; inspect `result.json`, `runner-result.json`, `node-context.json` | 29 focused cases pass. Root probe has complete trace and empty integrity violations but no business evidence, so evaluator correctly returns `UNQUALIFIED`. |
| `build/source closure` | `N/A` | Python owner/runner and existing standalone harness | `python3 -m py_compile ...`; `git diff --check` | No native source or build target changed; native rebuild is not applicable to this handoff-only batch. |
| `migration/evidence` | `covered` | `tests/fixtures/spec182/case-manifest.json`, topology, tasks/evidence links | `rg -n "R10-B19|spec182-native-closure.conf|CANONICAL_RUNNER_RESULT_RECORDED" specs/182-native-di-python-bindings tests Experiments` | Owner-alive handoff is recorded with durable raw output; native DI cases, maintained caller, no-Python path and T016 remain open. |

## Validation

Focused checks:

```text
pytest -q tests/python/test_spec182_native_closure.py
29 passed in 0.13s
python3 -m py_compile Experiments/NDNSF_DI_NativeClosure_Minindn.py \
  tests/standalone/run-spec182-native-closure.py \
  tests/python/test_spec182_native_closure.py
git diff --check
```

The fresh root run used the system-first runtime path and a copied owner
manifest whose `campaignCase` was `I01`:

```text
sudo -n env PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  python3 Experiments/NDNSF_DI_NativeClosure_Minindn.py \
  --manifest .codex-tmp/spec182-r10-b19-20260909052040-owner.json \
  --output .codex-tmp/spec182-r10-b19-20260909052040 \
  --execute-owner \
  --runner-manifest .codex-tmp/spec182-r10-b19-20260909052040-runner.json \
  --runner-case I01
exit code: 2
```

Raw output: `.codex-tmp/spec182-r10-b19-20260909052040/`.

- `result.json`: `status=UNQUALIFIED`,
  `reason=CANONICAL_RUNNER_RESULT_RECORDED`, `campaignCase=I01`.
- `runner-result.json`: evaluator `UNQUALIFIED`; runner `returncode=0`;
  observation `complete=true`; `integrityViolations=[]`; `policyViolations=[]`;
  four trace events. The command begins with `nsenter --net=/proc/self/fd/12`,
  proving the held owner namespace descriptor was handed to the runner.
- `node-context.json`: requester/provider have distinct namespace inodes and
  owner PID/start-tick identities, absolute NFD UNIX sockets, and reciprocal
  peer IDs.

## Batch retrospective and closure

- `static`: no introduced regression; static review covered lifecycle, context
  identity, runner selection, cleanup, tests and evidence wiring.
- `compile/link`: no native compile/link lane applies; Python syntax and diff
  checks passed.
- `runtime/test`: the owner and runner composed successfully; the process and
  trace observation passed, while the evaluator stayed `UNQUALIFIED` for
  missing business evidence.
- `unobserved`: executable native DI case, real request/result oracle,
  maintained caller, cross-process/no-Python behavior and T016 qualification.

Closure decision: `CLOSED_FOR_VALIDATION` for the owner-to-runner handoff only;
the Spec182 feature remains `PARTIAL` and T016 remains `PARTIAL/UNQUALIFIED`.
