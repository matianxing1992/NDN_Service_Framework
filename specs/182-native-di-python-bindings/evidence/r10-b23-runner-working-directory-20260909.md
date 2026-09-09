# R10-B23 Runner Working Directory and Config Boundary

**Status**: DONE for the bounded runner working-directory/config boundary; T014/T016 remain PARTIAL/UNQUALIFIED  
**Date**: 2026-09-09  
**Baseline**: `3acae7ef`  
**Owner**: T014/T016 local runner

## Scope and stable exit

R10-B23 repairs the R10-B22 setup boundary where the native integration selector could not load
relative `examples/trust-any.conf` inside the minimal root. The runner accepts an optional process
`workingDirectory` only when it is `/probe-root` or a descendant, or `/tmp`; a declared data/config
artifact is staged under `/probe-root/examples/` and the bubblewrap launch changes directory there.
Host paths and environment-based config injection remain unavailable.

The stable exit is a fresh owner/runner `PO-001` run reaching the existing native DI selector with a
complete trace and `SPEC182_NATIVE_DI_REQUEST_RESULT_OK`. This batch is harness-only and cannot close
multi-process transport, maintained callers, negative cases, or T016 qualification.

## Minimum Review Record

Review path: `/home/tianxing/.codex/skills/review-agent/SKILL.md`  
Review SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`  
Review baseline: `3acae7ef`  
Diff scope: `tests/standalone/run-spec182-native-closure.py`,
`tests/python/test_spec182_native_closure.py`, this evidence, and R10-B23 task/plan rows.

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | covered | `run_case` → `make_launch` → staged integration selector | CodeGraph/targeted source review | cwd is a runner concern; native selector and owner handoff remain unchanged. |
| `implementation and wire` | covered | `load_case`, `make_launch`, process `workingDirectory`, data/config artifact staging | targeted source review; path-boundary tests | only staged-root or `/tmp` cwd is accepted; no host path is introduced. |
| `test/harness/oracle` | covered | manifest validation and `PO-001` marker oracle | focused Python selectors; marker remains independent | test covers accepted/rejected cwd and staged config placement; business marker still cannot promote qualification. |
| `build/source closure` | N/A | Python runner/harness only | `py_compile`; no native source changes | no native rebuild is applicable to this batch. |
| `migration/evidence` | gap | owner/runner config handoff and true multi-process transport | fresh owner/runner run recorded below | R10-B22 setup failure is repaired; T016 and maintained callers remain open. |

## Validation record

| Field | Result |
| --- | --- |
| `Static findings` | `STATIC_PASS`; official `review-agent` re-review found no introduced defect |
| `Compile/link misses` | N/A; Python-only harness change |
| `Runtime/test misses` | `FOCUSED_BEHAVIOR_PASS`; fresh owner/runner `PO-001` reached the native selector and emitted the independent marker |
| `Unobserved` | arbitrary cwd rejection, host config escape, multi-process transport and T016 qualification |
| `Build measurement` | N/A; no native build |
| `Behavior result` | `PASS` for isolated `PO-001`: returncode 0, complete trace, all seven evidence classes, no integrity/policy violations |
| `Evidence / remaining` | `.codex-tmp/spec182-r10-b23-runner-working-dir-owner/`; stdout contains `SPEC182_NATIVE_DI_REQUEST_RESULT_OK`; staged config is `closure-run/root/examples/trust-any.conf` |

## Commands and observed result

Focused checks completed before the runtime retry:

```text
pytest -q tests/python/test_spec182_native_closure.py
33 passed in 0.21s
python3 -m py_compile tests/standalone/run-spec182-native-closure.py tests/python/test_spec182_native_closure.py
python3 specs/182-native-di-python-bindings/checklists/validate_design.py
errors=[]
```

The fresh root command used the existing tracked MiniNDN owner manifest and generated
`.codex-tmp/spec182-r10-b22-native-di-manifest-v5.json`:

```text
sudo -n env PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  python3 Experiments/NDNSF_DI_NativeClosure_Minindn.py \
  --manifest .codex-tmp/spec182-r10-b19-20260909052040-owner.json \
  --output .codex-tmp/spec182-r10-b23-runner-working-dir-owner --execute-owner \
  --runner-manifest .codex-tmp/spec182-r10-b22-native-di-manifest-v5.json \
  --runner-case PO-001
```

The command returned `0`. `runner-result.json` records evaluator `PASS`, process returncode
`0`, `timedOut=false`, `durationMs=7835`, `complete=true`, 16 observed PIDs, one successful
exec, ten exit events, and evidence `business-oracle`, `cleanup`, `endpoints`, `exec-map`,
`identity`, `namespace`, and `process-tree`. The captured stdout contains
`SPEC182_NATIVE_DI_REQUEST_RESULT_OK`; the native test also reports
`NDNSF_INTEGRATION_BOOTSTRAP_READY` and the R4-B6 grant/request line. The result remains an
isolated-process native DI business observation, not cross-process requester/provider transport
or final T016 qualification.

## Batch Retrospective

- `static`: the official review path and SHA above were re-run against the runner, manifest
  validation, launch argv, artifact staging, call sites and tests; no introduced finding.
- `compile/link`: not applicable; no native target changed.
- `runtime/test`: the fresh owner/runner run passed after staging `trust-any.conf` and changing
  into `/probe-root`; the prior R10-B22 setup boundary is therefore closed.
- `unobserved`: host-path escape, multi-process transport, maintained callers and final qualification.
- Batch expansion: none planned; native negative cases use a separate selector/batch.

## Closure decision

`CLOSED_FOR_VALIDATION`: the bounded working-directory/config boundary passed static review,
33 focused Python cases, `py_compile`, design validation and a fresh root owner/runner `PO-001`
run. This does not close multi-process transport, maintained callers, I02--I08 or T016
qualification; those remain the next production-chain batches.
